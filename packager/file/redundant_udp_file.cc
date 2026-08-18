// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/file/redundant_udp_file.h>

#include <chrono>
#include <cstring>

#include <absl/log/log.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_split.h>

#include <packager/macros/compiler.h>
#include <packager/macros/logging.h>

namespace shaka {
namespace {

const char kUdpPrefix[] = "udp://";
// 2 MB of merged output buffering between reader threads and the demuxer.
const uint64_t kCacheSize = 2 * 1024 * 1024;
// Receive chunk per socket read. UDP datagrams for TS are ≤ ~1500 bytes
// (7 x 188 = 1316 typically); one datagram per Read call.
const size_t kReadChunkSize = 65536;
// Injected when a leg has no explicit timeout so reader threads wake up to
// drive health ticks and honor shutdown.
const char kDefaultTimeoutOption[] = "timeout=100000";
// Stats summary period.
const int64_t kStatsLogPeriodMs = 60000;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Returns true and consumes "<key>=<int>" if |pair| starts with |key|.
bool ParseIntParam(const std::string& pair, const char* key, int64_t* out) {
  const std::string prefix = std::string(key) + "=";
  if (pair.compare(0, prefix.size(), prefix) != 0)
    return false;
  return absl::SimpleAtoi(pair.substr(prefix.size()), out);
}

}  // namespace

RedundantUdpFile::RedundantUdpFile(const char* url)
    : File(url), cache_(kCacheSize) {}

RedundantUdpFile::~RedundantUdpFile() {}

// static
bool RedundantUdpFile::ParseUrl(const std::string& url,
                                std::vector<std::string>* leg_urls,
                                RedundantInputMerger::Config* config) {
  leg_urls->clear();
  *config = RedundantInputMerger::Config();

  std::vector<std::string> legs = absl::StrSplit(url, '|');
  if (legs.size() < 2) {
    LOG(ERROR) << "redundant:// needs at least 2 legs separated by '|': "
               << url;
    return false;
  }

  // Global merger parameters are recognized "&key=value" pairs at the tail of
  // the last leg; unrecognized pairs stay with the leg's UDP options.
  std::string& last = legs.back();
  std::vector<std::string> parts = absl::StrSplit(last, '&');
  size_t leg_parts = parts.size();
  while (leg_parts > 1) {
    const std::string& pair = parts[leg_parts - 1];
    int64_t value = 0;
    if (pair.compare(0, 5, "mode=") == 0) {
      const std::string mode = pair.substr(5);
      if (mode == "merge") {
        config->mode = RedundantInputMerger::Mode::kMerge;
      } else if (mode == "failover") {
        config->mode = RedundantInputMerger::Mode::kFailover;
      } else {
        LOG(ERROR) << "Unknown redundant:// mode: " << mode;
        return false;
      }
    } else if (ParseIntParam(pair, "dedup_window_ms", &value)) {
      config->dedup_window_ms = value;
    } else if (ParseIntParam(pair, "dedup_window_pkts", &value)) {
      config->dedup_window_pkts = static_cast<size_t>(value);
    } else if (ParseIntParam(pair, "failover_timeout_ms", &value)) {
      config->failover_timeout_ms = value;
    } else {
      break;  // Not a merger parameter; it belongs to the leg URL.
    }
    --leg_parts;
  }
  last = parts[0];
  for (size_t i = 1; i < leg_parts; ++i)
    last += "&" + parts[i];

  for (std::string& leg : legs) {
    if (leg.compare(0, strlen(kUdpPrefix), kUdpPrefix) != 0) {
      LOG(ERROR) << "redundant:// legs must be udp:// URLs, got: " << leg;
      return false;
    }
    // Ensure a receive timeout so the reader thread can tick and stop.
    if (leg.find("timeout=") == std::string::npos) {
      leg += (leg.find('?') == std::string::npos ? "?" : "&");
      leg += kDefaultTimeoutOption;
    }
    leg_urls->push_back(leg);
  }
  config->num_legs = leg_urls->size();
  return true;
}

bool RedundantUdpFile::Open() {
  if (!ParseUrl(file_name(), &leg_urls_, &config_))
    return false;

  merger_.reset(new RedundantInputMerger(
      config_, [this](const uint8_t* packet) {
        cache_.Write(packet, RedundantInputMerger::kTsPacketSize);
      }));

  size_t opened = 0;
  legs_.resize(leg_urls_.size(), nullptr);
  for (size_t i = 0; i < leg_urls_.size(); ++i) {
    // Legs are raw sockets for our reader threads; the buffered
    // ThreadedIoFile wrapper from File::Open would add a redundant cache
    // and thread per leg.
    legs_[i] = File::OpenWithNoBuffering(leg_urls_[i].c_str(), "r");
    if (!legs_[i]) {
      LOG(WARNING) << "redundant:// failed to open leg " << i << ": "
                   << leg_urls_[i];
      continue;
    }
    ++opened;
  }
  if (opened == 0) {
    LOG(ERROR) << "redundant:// could not open any leg.";
    return false;
  }

  for (size_t i = 0; i < legs_.size(); ++i) {
    if (legs_[i])
      threads_.emplace_back(&RedundantUdpFile::ReaderThread, this, i);
  }
  return true;
}

void RedundantUdpFile::ReaderThread(size_t leg_index) {
  std::vector<uint8_t> buffer(kReadChunkSize);
  while (!stop_.load(std::memory_order_relaxed)) {
    const int64_t bytes =
        legs_[leg_index]->Read(buffer.data(), buffer.size());
    if (stop_.load(std::memory_order_relaxed))
      break;
    const int64_t now = NowMs();
    std::lock_guard<std::mutex> lock(merger_mutex_);
    if (bytes > 0) {
      merger_->OnBytes(leg_index, buffer.data(), static_cast<size_t>(bytes),
                       now);
    } else {
      // Timeout or transient error: advance health so a silent active leg
      // fails over even when only this thread is awake.
      merger_->OnTick(now);
    }
    MaybeLogStats(now);
  }
}

void RedundantUdpFile::MaybeLogStats(int64_t now_ms) {
  // Called with merger_mutex_ held.
  if (now_ms - last_stats_log_ms_ < kStatsLogPeriodMs)
    return;
  const bool first = last_stats_log_ms_ == 0;
  last_stats_log_ms_ = now_ms;
  if (first)
    return;  // Skip the incomplete first period.
  for (size_t i = 0; i < config_.num_legs; ++i) {
    const RedundantInputMerger::LegStats stats = merger_->GetLegStats(i);
    LOG(INFO) << "redundant_input: leg=" << i << " pkts=" << stats.packets
              << " dropped_dup=" << stats.dropped_dup
              << " resyncs=" << stats.resyncs
              << " cc_errors=" << stats.cc_errors << " state="
              << (stats.state == RedundantInputMerger::LegState::kHealthy
                      ? "HEALTHY"
                      : "UNHEALTHY")
              << " active="
              << (merger_->active_leg() == i ? "yes" : "no");
  }
  LOG(INFO) << "redundant_input: switches=" << merger_->switches()
            << " emitted_cc_errors=" << merger_->emitted_cc_errors()
            << " max_skew_ms=" << merger_->max_skew_ms()
            << " window_evictions=" << merger_->window_evictions();
}

bool RedundantUdpFile::Close() {
  stop_.store(true, std::memory_order_relaxed);
  // Join reader threads BEFORE closing the legs they read from: every leg
  // has a receive timeout (injected in ParseUrl when absent), so readers
  // observe |stop_| within one timeout period. Closing first would delete
  // the leg objects out from under threads still blocked in Read().
  for (std::thread& thread : threads_) {
    if (thread.joinable())
      thread.join();
  }
  threads_.clear();
  for (File*& leg : legs_) {
    if (leg) {
      leg->Close();  // UdpFile::Close deletes the leg object.
      leg = nullptr;
    }
  }
  cache_.Close();
  delete this;
  return true;
}

int64_t RedundantUdpFile::Read(void* buffer, uint64_t length) {
  // Blocks until data is available; returns 0 only once closed and drained.
  return static_cast<int64_t>(cache_.Read(buffer, length));
}

int64_t RedundantUdpFile::Write(const void* buffer, uint64_t length) {
  UNUSED(buffer);
  UNUSED(length);
  NOTIMPLEMENTED() << "RedundantUdpFile is read-only.";
  return -1;
}

void RedundantUdpFile::CloseForWriting() {}

int64_t RedundantUdpFile::Size() {
  return -1;
}

bool RedundantUdpFile::Flush() {
  NOTIMPLEMENTED() << "RedundantUdpFile is read-only.";
  return false;
}

bool RedundantUdpFile::Seek(uint64_t position) {
  UNUSED(position);
  return false;
}

bool RedundantUdpFile::Tell(uint64_t* position) {
  UNUSED(position);
  return false;
}

}  // namespace shaka

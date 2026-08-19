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
#include <prometheus/collectable.h>
#include <prometheus/metric_family.h>

#include <packager/macros/compiler.h>
#include <packager/macros/logging.h>
#include <packager/metrics/metrics_service.h>

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

// Exports RedundantUdpFile::GetStatsSnapshot() as Prometheus families.
// Detach() severs the back-pointer before the file self-deletes in Close();
// the MetricsService holds this object weakly, so its own destruction needs
// no unregistration.
class RedundantUdpStatsCollector : public prometheus::Collectable {
 public:
  RedundantUdpStatsCollector(RedundantUdpFile* file, std::string input_label)
      : file_(file), input_(std::move(input_label)) {}

  void Detach() {
    std::lock_guard<std::mutex> lock(mutex_);
    file_ = nullptr;
  }

  std::vector<prometheus::MetricFamily> Collect() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_)
      return {};
    const RedundantUdpFile::StatsSnapshot snap = file_->GetStatsSnapshot();

    std::vector<prometheus::MetricFamily> families;
    // add_family returns an index into |families|, not a pointer: the
    // vector keeps growing via push_back as more families are added, and an
    // index (unlike a pointer/reference) stays valid across any subsequent
    // reallocation.
    auto add_family = [&](const char* name, const char* help,
                          prometheus::MetricType type) {
      prometheus::MetricFamily family;
      family.name = name;
      family.help = help;
      family.type = type;
      families.push_back(std::move(family));
      return families.size() - 1;
    };
    auto add_metric = [&](size_t family_index, double value,
                          int leg /* -1 = global */) {
      prometheus::MetricFamily& family = families[family_index];
      prometheus::ClientMetric metric;
      metric.label.push_back({"input", input_});
      if (leg >= 0)
        metric.label.push_back({"leg", std::to_string(leg)});
      if (family.type == prometheus::MetricType::Counter)
        metric.counter.value = value;
      else
        metric.gauge.value = value;
      family.metric.push_back(std::move(metric));
    };

    using prometheus::MetricType;
    const size_t packets = add_family("shaka_redundant_leg_packets_total",
                                      "Well-framed TS packets per leg.",
                                      MetricType::Counter);
    const size_t dropped =
        add_family("shaka_redundant_leg_dropped_dup_total",
                   "Packets dropped as duplicates per leg.",
                   MetricType::Counter);
    const size_t resyncs = add_family("shaka_redundant_leg_resyncs_total",
                                      "Sync-byte resyncs per leg.",
                                      MetricType::Counter);
    const size_t cc_errors =
        add_family("shaka_redundant_leg_cc_errors_total",
                   "Continuity-counter errors per leg.", MetricType::Counter);
    const size_t healthy = add_family("shaka_redundant_leg_healthy",
                                      "1 when the leg is HEALTHY, else 0.",
                                      MetricType::Gauge);
    const size_t active = add_family("shaka_redundant_leg_active",
                                     "1 for the active leg (failover mode).",
                                     MetricType::Gauge);
    for (size_t i = 0; i < snap.legs.size(); ++i) {
      const auto& leg = snap.legs[i];
      const int leg_index = static_cast<int>(i);
      add_metric(packets, static_cast<double>(leg.packets), leg_index);
      add_metric(dropped, static_cast<double>(leg.dropped_dup), leg_index);
      add_metric(resyncs, static_cast<double>(leg.resyncs), leg_index);
      add_metric(cc_errors, static_cast<double>(leg.cc_errors), leg_index);
      add_metric(healthy,
                 leg.state == RedundantInputMerger::LegState::kHealthy ? 1 : 0,
                 leg_index);
      add_metric(active, snap.active_leg == i ? 1 : 0, leg_index);
    }
    add_metric(add_family("shaka_redundant_switches_total",
                          "Active-leg switches (failover mode).",
                          MetricType::Counter),
               static_cast<double>(snap.switches), -1);
    add_metric(add_family("shaka_redundant_emitted_cc_errors_total",
                          "Continuity-counter errors in the emitted stream.",
                          MetricType::Counter),
               static_cast<double>(snap.emitted_cc_errors), -1);
    add_metric(add_family("shaka_redundant_max_skew_ms",
                          "Largest observed inter-leg arrival skew, ms.",
                          MetricType::Gauge),
               static_cast<double>(snap.max_skew_ms), -1);
    add_metric(add_family("shaka_redundant_window_evictions_total",
                          "Dedup-window evictions by the packet-count bound.",
                          MetricType::Counter),
               static_cast<double>(snap.window_evictions), -1);
    return families;
  }

 private:
  mutable std::mutex mutex_;
  RedundantUdpFile* file_;
  const std::string input_;
};

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

  // Register only once success is certain: if Open() returns false, the
  // File::Open factory destroys this object directly (not via Close()), so
  // a collector registered any earlier could still be scraped mid-teardown.
  stats_collector_ =
      std::make_shared<RedundantUdpStatsCollector>(this, file_name());
  MetricsService::Instance().RegisterCollectable(stats_collector_);
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

RedundantUdpFile::StatsSnapshot RedundantUdpFile::SnapshotLocked() const {
  StatsSnapshot snap;
  for (size_t i = 0; i < config_.num_legs; ++i)
    snap.legs.push_back(merger_->GetLegStats(i));
  snap.active_leg = merger_->active_leg();
  snap.switches = merger_->switches();
  snap.emitted_cc_errors = merger_->emitted_cc_errors();
  snap.max_skew_ms = merger_->max_skew_ms();
  snap.window_evictions = merger_->window_evictions();
  return snap;
}

RedundantUdpFile::StatsSnapshot RedundantUdpFile::GetStatsSnapshot() {
  std::lock_guard<std::mutex> lock(merger_mutex_);
  return SnapshotLocked();
}

void RedundantUdpFile::MaybeLogStats(int64_t now_ms) {
  // Called with merger_mutex_ held.
  if (now_ms - last_stats_log_ms_ < kStatsLogPeriodMs)
    return;
  const bool first = last_stats_log_ms_ == 0;
  last_stats_log_ms_ = now_ms;
  if (first)
    return;  // Skip the incomplete first period.
  const StatsSnapshot snap = SnapshotLocked();
  for (size_t i = 0; i < snap.legs.size(); ++i) {
    const RedundantInputMerger::LegStats& stats = snap.legs[i];
    LOG(INFO) << "redundant_input: leg=" << i << " pkts=" << stats.packets
              << " dropped_dup=" << stats.dropped_dup
              << " resyncs=" << stats.resyncs
              << " cc_errors=" << stats.cc_errors << " state="
              << (stats.state == RedundantInputMerger::LegState::kHealthy
                      ? "HEALTHY"
                      : "UNHEALTHY")
              << " active=" << (snap.active_leg == i ? "yes" : "no");
  }
  LOG(INFO) << "redundant_input: switches=" << snap.switches
            << " emitted_cc_errors=" << snap.emitted_cc_errors
            << " max_skew_ms=" << snap.max_skew_ms
            << " window_evictions=" << snap.window_evictions;
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
  if (stats_collector_) {
    // Sever the collector's back-pointer before self-deletion; a scrape
    // holding the shared_ptr sees an empty result instead of a dangle.
    stats_collector_->Detach();
    stats_collector_.reset();
  }
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

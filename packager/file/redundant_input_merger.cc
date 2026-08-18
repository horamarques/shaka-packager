// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/file/redundant_input_merger.h>

#include <algorithm>
#include <string_view>

#include <absl/log/check.h>
#include <absl/log/log.h>

namespace shaka {
namespace {

const uint8_t kTsSyncByte = 0x47;
const uint16_t kNullPid = 0x1FFF;
// Hashes of packets emitted within this window are kept to skip
// already-emitted packets right after a failover switch.
const int64_t kFailoverRingMs = 100;

// 64-bit hash of a full 188-byte packet, used as the dedup key. We do not
// store the packets themselves for byte comparison: with the window bounded
// to at most a few thousand entries, the probability of a 64-bit collision
// (i.e. a false dedup drop) is on the order of 2^-64 * window_size^2 --
// astronomically unlikely, and the consequence is a single dropped packet.
uint64_t HashPacket(const uint8_t* packet) {
  return std::hash<std::string_view>()(
      std::string_view(reinterpret_cast<const char*>(packet),
                       RedundantInputMerger::kTsPacketSize));
}

}  // namespace

RedundantInputMerger::RedundantInputMerger(const Config& config,
                                           EmitCallback emit_cb)
    : config_(config), emit_cb_(std::move(emit_cb)), legs_(config.num_legs) {
  DCHECK_GE(config_.num_legs, 1u);
  DCHECK(emit_cb_);
}

RedundantInputMerger::~RedundantInputMerger() = default;

void RedundantInputMerger::OnBytes(size_t leg,
                                   const uint8_t* data,
                                   size_t size,
                                   int64_t arrival_time_ms) {
  DCHECK_LT(leg, legs_.size());
  AdvanceTime(arrival_time_ms);
  legs_[leg].buffer.insert(legs_[leg].buffer.end(), data, data + size);
  FramePackets(leg, arrival_time_ms);
  CheckHealth();
}

void RedundantInputMerger::OnTick(int64_t now_ms) {
  AdvanceTime(now_ms);
  CheckHealth();
}

RedundantInputMerger::LegStats RedundantInputMerger::GetLegStats(
    size_t leg) const {
  DCHECK_LT(leg, legs_.size());
  return legs_[leg].stats;
}

void RedundantInputMerger::AdvanceTime(int64_t time_ms) {
  if (!time_initialized_) {
    // Anchor silence tracking to the first observed timestamp so that legs
    // are not declared UNHEALTHY before the stream has even started.
    for (Leg& leg : legs_)
      leg.last_packet_ms = time_ms;
    time_initialized_ = true;
  }
  now_ms_ = std::max(now_ms_, time_ms);
}

void RedundantInputMerger::FramePackets(size_t leg_index,
                                        int64_t arrival_time_ms) {
  Leg& leg = legs_[leg_index];
  std::vector<uint8_t>& buffer = leg.buffer;
  size_t pos = 0;
  while (buffer.size() - pos >= kTsPacketSize) {
    if (leg.synced) {
      if (buffer[pos] == kTsSyncByte) {
        HandlePacket(leg_index, &buffer[pos], arrival_time_ms);
        pos += kTsPacketSize;
        continue;
      }
      leg.synced = false;
      leg.pending_resync = true;
    }
    // Misaligned: scan for a 0x47 with a second 0x47 at +188 confirming.
    bool resynced = false;
    while (pos < buffer.size()) {
      if (buffer[pos] != kTsSyncByte) {
        ++pos;
        continue;
      }
      if (pos + kTsPacketSize >= buffer.size()) {
        // Candidate found but the confirming byte has not arrived yet; keep
        // the tail and wait for more data.
        break;
      }
      if (buffer[pos + kTsPacketSize] == kTsSyncByte) {
        resynced = true;
        break;
      }
      ++pos;
    }
    if (!resynced)
      break;
    leg.synced = true;
    if (leg.pending_resync) {
      ++leg.stats.resyncs;
      leg.pending_resync = false;
    }
  }
  buffer.erase(buffer.begin(), buffer.begin() + pos);
}

void RedundantInputMerger::HandlePacket(size_t leg_index,
                                        const uint8_t* packet,
                                        int64_t arrival_time_ms) {
  Leg& leg = legs_[leg_index];
  ++leg.stats.packets;
  leg.last_packet_ms = arrival_time_ms;
  // A well-framed packet flips the leg back to HEALTHY. It does NOT steal
  // the active role in failover mode (no flap-back).
  leg.stats.state = LegState::kHealthy;
  TrackCc(packet, &leg.cc_state, &leg.stats.cc_errors);

  const uint64_t hash = HashPacket(packet);
  if (config_.mode == Mode::kMerge) {
    EvictDedupWindow();
    if (window_set_.count(hash) != 0) {
      ++leg.stats.dropped_dup;
      return;
    }
    window_set_.insert(hash);
    window_fifo_.push_back({hash, now_ms_});
    Emit(packet, hash);
    return;
  }

  // Failover mode: only the active leg emits; standby legs are framed and
  // health-tracked above.
  if (leg_index != active_leg_)
    return;
  if (resuming_after_switch_) {
    EvictRing();
    if (ring_counts_.count(hash) != 0) {
      ++leg.stats.dropped_dup;
      return;
    }
    // Emission resumes at the first unseen packet.
    resuming_after_switch_ = false;
  }
  Emit(packet, hash);
}

void RedundantInputMerger::Emit(const uint8_t* packet, uint64_t hash) {
  TrackCc(packet, &emitted_cc_state_, &emitted_cc_errors_);
  if (config_.mode == Mode::kFailover) {
    ++ring_counts_[hash];
    ring_fifo_.push_back({hash, now_ms_});
    EvictRing();
  }
  emit_cb_(packet);
}

void RedundantInputMerger::CheckHealth() {
  for (Leg& leg : legs_) {
    if (leg.stats.state == LegState::kHealthy &&
        now_ms_ - leg.last_packet_ms > config_.failover_timeout_ms) {
      leg.stats.state = LegState::kUnhealthy;
    }
  }
  if (config_.mode != Mode::kFailover ||
      legs_[active_leg_].stats.state == LegState::kHealthy) {
    return;
  }
  // Active leg is UNHEALTHY: switch to the lowest-index HEALTHY leg. If no
  // leg is healthy there is nothing to emit anyway; keep the current active.
  for (size_t i = 0; i < legs_.size(); ++i) {
    if (legs_[i].stats.state == LegState::kHealthy) {
      LOG(WARNING) << "redundant_input: active leg " << active_leg_
                   << " unhealthy, switching to leg " << i;
      active_leg_ = i;
      ++switches_;
      resuming_after_switch_ = true;
      break;
    }
  }
}

void RedundantInputMerger::EvictDedupWindow() {
  while (!window_fifo_.empty() &&
         (window_fifo_.size() >= config_.dedup_window_pkts ||
          window_fifo_.front().time_ms < now_ms_ - config_.dedup_window_ms)) {
    window_set_.erase(window_fifo_.front().hash);
    window_fifo_.pop_front();
  }
}

void RedundantInputMerger::EvictRing() {
  while (!ring_fifo_.empty() &&
         ring_fifo_.front().time_ms < now_ms_ - kFailoverRingMs) {
    auto it = ring_counts_.find(ring_fifo_.front().hash);
    DCHECK(it != ring_counts_.end());
    if (--it->second == 0)
      ring_counts_.erase(it);
    ring_fifo_.pop_front();
  }
}

void RedundantInputMerger::TrackCc(const uint8_t* packet,
                                   CcState* cc_state,
                                   uint64_t* cc_errors) {
  const uint16_t pid = ((packet[1] & 0x1F) << 8) | packet[2];
  if (pid == kNullPid)
    return;
  const bool has_payload = (packet[3] & 0x10) != 0;
  const uint8_t cc = packet[3] & 0x0F;
  auto [it, inserted] = cc_state->try_emplace(pid, cc);
  if (inserted)
    return;
  if (has_payload) {
    const uint8_t expected = (it->second + 1) & 0x0F;
    // A repeat of the previous CC is a legal duplicate, not an error.
    if (cc != expected && cc != it->second)
      ++*cc_errors;
  }
  it->second = cc;
}

}  // namespace shaka

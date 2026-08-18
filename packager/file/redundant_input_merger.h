// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_FILE_REDUNDANT_INPUT_MERGER_H_
#define PACKAGER_FILE_REDUNDANT_INPUT_MERGER_H_

#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <packager/macros/classes.h>

namespace shaka {

/// Merges the byte streams of N redundant MPEG-TS input legs carrying the
/// same multiplex (SMPTE 2022-7 style) into a single ordered stream of
/// 188-byte TS packets. Pure logic: no sockets, no threads.
///
/// This class is NOT thread-safe. Callers own synchronization and must
/// serialize all calls (OnBytes, OnTick and the getters).
class RedundantInputMerger {
 public:
  static constexpr size_t kTsPacketSize = 188;

  enum class Mode {
    /// First-arrival dedup across all legs (hitless).
    kMerge,
    /// Emit only the active leg; switch on active-leg failure.
    kFailover,
  };

  enum class LegState {
    kHealthy,
    kUnhealthy,
  };

  struct Config {
    Mode mode = Mode::kMerge;
    /// Dedup window bound in stream arrival time.
    int64_t dedup_window_ms = 200;
    /// Dedup window bound in packets; the window is bounded by BOTH limits.
    size_t dedup_window_pkts = 4096;
    /// A leg with no well-framed packet for longer than this is UNHEALTHY.
    int64_t failover_timeout_ms = 200;
    size_t num_legs = 2;
  };

  struct LegStats {
    LegState state = LegState::kHealthy;
    /// Well-framed packets seen on this leg.
    uint64_t packets = 0;
    /// Packets dropped as duplicates (merge dedup window, or failover
    /// post-switch ring skip).
    uint64_t dropped_dup = 0;
    /// Times framing lost and re-established 0x47 sync on this leg.
    uint64_t resyncs = 0;
    /// Continuity-counter errors on this leg's own framed stream.
    uint64_t cc_errors = 0;
  };

  /// Called once per emitted 188-byte packet, in order.
  using EmitCallback = std::function<void(const uint8_t* packet188)>;

  RedundantInputMerger(const Config& config, EmitCallback emit_cb);
  ~RedundantInputMerger();

  /// Feeds raw bytes received on one leg. Arbitrary chunking is allowed;
  /// bytes are accumulated and framed into 188-byte packets internally.
  /// @param leg is the 0-based leg index, less than Config::num_legs.
  /// @param arrival_time_ms is the arrival timestamp of these bytes; it must
  ///        not decrease across calls for the same leg.
  void OnBytes(size_t leg, const uint8_t* data, size_t size,
               int64_t arrival_time_ms);

  /// Advances silence-timeout detection without new bytes; drives the leg
  /// health state machine (and failover switching) when legs go quiet.
  void OnTick(int64_t now_ms);

  LegStats GetLegStats(size_t leg) const;
  /// Number of active-leg switches performed (failover mode).
  uint64_t switches() const { return switches_; }
  /// Currently active leg (meaningful in failover mode).
  size_t active_leg() const { return active_leg_; }
  /// Continuity-counter errors observed across the EMITTED stream.
  uint64_t emitted_cc_errors() const { return emitted_cc_errors_; }

 private:
  // Per-PID continuity-counter tracking state (last CC value per PID).
  using CcState = std::unordered_map<uint16_t, uint8_t>;

  struct Leg {
    LegStats stats;
    std::vector<uint8_t> buffer;
    // Framing starts optimistic; the first non-0x47 leading byte triggers a
    // scan for 0x47 with a second 0x47 at +188 confirming.
    bool synced = true;
    // True while sync is lost; the next successful resync increments
    // stats.resyncs.
    bool pending_resync = false;
    // Arrival time of the last well-framed packet; silence is measured from
    // here (bytes that never frame into a packet do not count as life signs).
    int64_t last_packet_ms = 0;
    CcState cc_state;
  };

  struct HashedEntry {
    uint64_t hash;
    int64_t time_ms;
  };

  // Advances the merger clock and lazily anchors per-leg silence tracking to
  // the first observed timestamp.
  void AdvanceTime(int64_t time_ms);
  // Cuts complete packets out of the leg's buffer, resyncing on 0x47 with
  // 188-stride confirmation after any misalignment.
  void FramePackets(size_t leg_index, int64_t arrival_time_ms);
  void HandlePacket(size_t leg_index, const uint8_t* packet,
                    int64_t arrival_time_ms);
  void Emit(const uint8_t* packet, uint64_t hash);
  // Marks silent legs UNHEALTHY and, in failover mode, switches away from an
  // UNHEALTHY active leg to the lowest-index HEALTHY leg.
  void CheckHealth();
  void EvictDedupWindow();
  void EvictRing();
  // Updates per-PID CC tracking with |packet|, bumping *cc_errors on a
  // discontinuity.
  static void TrackCc(const uint8_t* packet, CcState* cc_state,
                      uint64_t* cc_errors);

  const Config config_;
  const EmitCallback emit_cb_;

  std::vector<Leg> legs_;

  // Monotonic merger clock: max of all timestamps seen so far.
  int64_t now_ms_ = 0;
  bool time_initialized_ = false;

  // Merge-mode dedup window: set for O(1) lookup plus FIFO for eviction by
  // age and count. Only unseen hashes are inserted, so the two stay 1:1.
  std::unordered_set<uint64_t> window_set_;
  std::deque<HashedEntry> window_fifo_;

  // Failover mode.
  size_t active_leg_ = 0;
  uint64_t switches_ = 0;
  // After a switch, packets from the new active leg whose hash is in the
  // ring of recently emitted packets are skipped until the first unseen one.
  bool resuming_after_switch_ = false;
  // Ring of hashes emitted in the last ~100 ms. Count map because the
  // emitted stream may legitimately repeat byte-identical packets.
  std::unordered_map<uint64_t, int> ring_counts_;
  std::deque<HashedEntry> ring_fifo_;

  // CC tracking across the emitted stream.
  CcState emitted_cc_state_;
  uint64_t emitted_cc_errors_ = 0;

  DISALLOW_COPY_AND_ASSIGN(RedundantInputMerger);
};

}  // namespace shaka

#endif  // PACKAGER_FILE_REDUNDANT_INPUT_MERGER_H_

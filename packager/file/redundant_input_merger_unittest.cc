// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/file/redundant_input_merger.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

namespace shaka {
namespace {

constexpr size_t kPacketSize = RedundantInputMerger::kTsPacketSize;
constexpr uint16_t kTestPid = 0x100;

using Mode = RedundantInputMerger::Mode;
using LegState = RedundantInputMerger::LegState;

// Builds a valid 188-byte TS packet with payload, CC = seq mod 16, and the
// sequence number embedded in the payload so every packet is distinct.
std::vector<uint8_t> BuildTsPacket(uint32_t seq, uint16_t pid = kTestPid) {
  std::vector<uint8_t> packet(kPacketSize, 0xFF);
  packet[0] = 0x47;
  packet[1] = (pid >> 8) & 0x1F;
  packet[2] = pid & 0xFF;
  packet[3] = 0x10 | (seq & 0x0F);  // Payload only; CC follows seq.
  packet[4] = (seq >> 24) & 0xFF;
  packet[5] = (seq >> 16) & 0xFF;
  packet[6] = (seq >> 8) & 0xFF;
  packet[7] = seq & 0xFF;
  return packet;
}

// Concatenation of BuildTsPacket(seq) for each seq, i.e. the byte stream a
// single healthy leg would produce.
std::vector<uint8_t> BuildReference(const std::vector<uint32_t>& seqs) {
  std::vector<uint8_t> out;
  for (uint32_t seq : seqs) {
    const std::vector<uint8_t> packet = BuildTsPacket(seq);
    out.insert(out.end(), packet.begin(), packet.end());
  }
  return out;
}

std::vector<uint32_t> SeqRange(uint32_t count) {
  std::vector<uint32_t> seqs(count);
  for (uint32_t i = 0; i < count; ++i)
    seqs[i] = i;
  return seqs;
}

struct FeedEvent {
  int64_t time_ms;
  size_t leg;
  uint32_t seq;
};

// Feeds whole packets to the merger in arrival-time order (stable for ties).
void Feed(RedundantInputMerger* merger, std::vector<FeedEvent> events) {
  std::stable_sort(events.begin(), events.end(),
                   [](const FeedEvent& a, const FeedEvent& b) {
                     return a.time_ms < b.time_ms;
                   });
  for (const FeedEvent& event : events) {
    const std::vector<uint8_t> packet = BuildTsPacket(event.seq);
    merger->OnBytes(event.leg, packet.data(), packet.size(), event.time_ms);
  }
}

class RedundantInputMergerTest : public ::testing::Test {
 protected:
  std::unique_ptr<RedundantInputMerger> MakeMerger(
      const RedundantInputMerger::Config& config) {
    return std::make_unique<RedundantInputMerger>(
        config, [this](const uint8_t* packet) {
          output_.insert(output_.end(), packet, packet + kPacketSize);
        });
  }

  size_t EmittedPackets() const { return output_.size() / kPacketSize; }

  std::vector<uint8_t> output_;
};

// Test 1: two identical legs produce output byte-exact equal to a single-leg
// reference; every packet on the late leg is dropped as a duplicate.
TEST_F(RedundantInputMergerTest, IdenticalLegsMergeByteExact) {
  const uint32_t kNumPackets = 300;
  RedundantInputMerger::Config config;
  auto merger = MakeMerger(config);

  std::vector<FeedEvent> events;
  for (uint32_t i = 0; i < kNumPackets; ++i) {
    events.push_back({static_cast<int64_t>(i) * 5, 0, i});
    events.push_back({static_cast<int64_t>(i) * 5 + 1, 1, i});
  }
  Feed(merger.get(), events);

  EXPECT_EQ(BuildReference(SeqRange(kNumPackets)), output_);
  EXPECT_EQ(kNumPackets, merger->GetLegStats(0).packets);
  EXPECT_EQ(kNumPackets, merger->GetLegStats(1).packets);
  EXPECT_EQ(0u, merger->GetLegStats(0).dropped_dup);
  EXPECT_EQ(kNumPackets, merger->GetLegStats(1).dropped_dup);
  EXPECT_EQ(0u, merger->emitted_cc_errors());
}

// Test 2: leg B delivering the same stream 150 ms late (inside the 200 ms
// window) still yields byte-exact output.
TEST_F(RedundantInputMergerTest, SkewedLegsMergeByteExact) {
  const uint32_t kNumPackets = 200;
  RedundantInputMerger::Config config;
  auto merger = MakeMerger(config);

  std::vector<FeedEvent> events;
  for (uint32_t i = 0; i < kNumPackets; ++i) {
    events.push_back({static_cast<int64_t>(i) * 5, 0, i});
    events.push_back({static_cast<int64_t>(i) * 5 + 150, 1, i});
  }
  Feed(merger.get(), events);

  EXPECT_EQ(BuildReference(SeqRange(kNumPackets)), output_);
  EXPECT_EQ(kNumPackets, merger->GetLegStats(1).dropped_dup);
  EXPECT_EQ(0u, merger->emitted_cc_errors());
}

// Test 3: leg A dies mid-stream in merge mode; leg B keeps the output
// byte-exact with zero gap.
TEST_F(RedundantInputMergerTest, LegDeathMidStreamMergeZeroGap) {
  const uint32_t kNumPackets = 200;
  const uint32_t kKillAt = 100;
  RedundantInputMerger::Config config;
  auto merger = MakeMerger(config);

  std::vector<FeedEvent> events;
  for (uint32_t i = 0; i < kNumPackets; ++i) {
    if (i < kKillAt)
      events.push_back({static_cast<int64_t>(i) * 5, 0, i});
    events.push_back({static_cast<int64_t>(i) * 5 + 20, 1, i});
  }
  Feed(merger.get(), events);

  EXPECT_EQ(BuildReference(SeqRange(kNumPackets)), output_);
  EXPECT_EQ(0u, merger->emitted_cc_errors());
}

// Test 4: 1% packet loss on each leg, on disjoint packets; the legs heal
// each other and the output stays byte-exact.
TEST_F(RedundantInputMergerTest, DisjointLossOnBothLegsHealsByteExact) {
  const uint32_t kNumPackets = 400;
  RedundantInputMerger::Config config;
  auto merger = MakeMerger(config);

  std::vector<FeedEvent> events;
  for (uint32_t i = 0; i < kNumPackets; ++i) {
    if (i % 100 != 7)
      events.push_back({static_cast<int64_t>(i) * 5, 0, i});
    if (i % 100 != 53)
      events.push_back({static_cast<int64_t>(i) * 5 + 2, 1, i});
  }
  Feed(merger.get(), events);

  EXPECT_EQ(BuildReference(SeqRange(kNumPackets)), output_);
  EXPECT_EQ(0u, merger->emitted_cc_errors());
}

// Test 5: the same packet lost on BOTH legs: that packet is absent, all
// others present, and nothing crashes.
TEST_F(RedundantInputMergerTest, SharedLossPassesGapThrough) {
  const uint32_t kNumPackets = 200;
  const uint32_t kLostSeq = 50;
  RedundantInputMerger::Config config;
  auto merger = MakeMerger(config);

  std::vector<FeedEvent> events;
  for (uint32_t i = 0; i < kNumPackets; ++i) {
    if (i == kLostSeq)
      continue;
    events.push_back({static_cast<int64_t>(i) * 5, 0, i});
    events.push_back({static_cast<int64_t>(i) * 5 + 2, 1, i});
  }
  Feed(merger.get(), events);

  std::vector<uint32_t> expected_seqs;
  for (uint32_t i = 0; i < kNumPackets; ++i) {
    if (i != kLostSeq)
      expected_seqs.push_back(i);
  }
  EXPECT_EQ(BuildReference(expected_seqs), output_);
  // The gap shows up as exactly one CC discontinuity on the emitted stream.
  EXPECT_EQ(1u, merger->emitted_cc_errors());
}

// Test 6: failover mode; the active leg goes silent past the timeout. The
// switch happens exactly once (driven by OnTick), loss is bounded to the
// silence window, and packets already emitted are skipped via the ring.
TEST_F(RedundantInputMergerTest, FailoverSwitchesOnceOnSilence) {
  RedundantInputMerger::Config config;
  config.mode = Mode::kFailover;
  config.failover_timeout_ms = 50;
  auto merger = MakeMerger(config);

  // Both legs carry seq 0..20 until t=100; leg 0 (active) emits them.
  std::vector<FeedEvent> events;
  for (uint32_t i = 0; i <= 20; ++i) {
    events.push_back({static_cast<int64_t>(i) * 5, 0, i});
    events.push_back({static_cast<int64_t>(i) * 5 + 1, 1, i});
  }
  Feed(merger.get(), events);
  ASSERT_EQ(21u, EmittedPackets());

  // Leg 0 goes silent. Leg 1 keeps flowing (standby: framed, not emitted).
  Feed(merger.get(), {{105, 1, 21}, {110, 1, 22}, {115, 1, 23}, {120, 1, 24}});
  EXPECT_EQ(0u, merger->switches());
  EXPECT_EQ(21u, EmittedPackets());

  // Silence detection advances via OnTick: leg 0 is 60 ms silent (> 50 ms),
  // leg 1 is 40 ms silent (healthy) => exactly one switch.
  merger->OnTick(160);
  EXPECT_EQ(1u, merger->switches());
  EXPECT_EQ(1u, merger->active_leg());
  EXPECT_EQ(LegState::kUnhealthy, merger->GetLegStats(0).state);
  EXPECT_EQ(LegState::kHealthy, merger->GetLegStats(1).state);

  // A repeat of seq 20 (emitted by leg 0 at t=100, still inside the ~100 ms
  // ring) is skipped; the first unseen packet resumes emission.
  const std::vector<uint8_t> repeat = BuildTsPacket(20);
  merger->OnBytes(1, repeat.data(), repeat.size(), 165);
  EXPECT_EQ(21u, EmittedPackets());
  EXPECT_EQ(1u, merger->GetLegStats(1).dropped_dup);
  Feed(merger.get(), {{170, 1, 25}, {175, 1, 26}});
  EXPECT_EQ(23u, EmittedPackets());

  // Loss is bounded to the silence window: seq 21..24 (standby-only) lost.
  std::vector<uint32_t> expected_seqs = SeqRange(21);
  expected_seqs.push_back(25);
  expected_seqs.push_back(26);
  EXPECT_EQ(BuildReference(expected_seqs), output_);
  EXPECT_EQ(1u, merger->switches());
}

// Test 7: failover flap: leg A down, back up, down again => still exactly
// one switch (a recovered leg becomes standby; no flap-back).
TEST_F(RedundantInputMergerTest, FailoverNoFlapBack) {
  RedundantInputMerger::Config config;
  config.mode = Mode::kFailover;
  config.failover_timeout_ms = 50;
  auto merger = MakeMerger(config);

  std::vector<FeedEvent> events;
  for (uint32_t i = 0; i <= 10; ++i) {
    events.push_back({static_cast<int64_t>(i) * 5, 0, i});
    events.push_back({static_cast<int64_t>(i) * 5 + 1, 1, i});
  }
  Feed(merger.get(), events);

  // Leg 0 down; leg 1 keeps flowing; switch to leg 1.
  Feed(merger.get(), {{55, 1, 11}, {65, 1, 12}, {75, 1, 13}});
  merger->OnTick(105);
  EXPECT_EQ(1u, merger->switches());
  EXPECT_EQ(1u, merger->active_leg());

  // Leg 0 recovers: HEALTHY again, but it does not steal the active role.
  const std::vector<uint8_t> recovery = BuildTsPacket(100);
  merger->OnBytes(0, recovery.data(), recovery.size(), 110);
  EXPECT_EQ(LegState::kHealthy, merger->GetLegStats(0).state);
  EXPECT_EQ(1u, merger->active_leg());
  EXPECT_EQ(1u, merger->switches());

  // Leg 0 down again while leg 1 stays healthy: no further switch.
  Feed(merger.get(), {{120, 1, 14}, {140, 1, 15}, {160, 1, 16}});
  merger->OnTick(200);
  EXPECT_EQ(LegState::kUnhealthy, merger->GetLegStats(0).state);
  EXPECT_EQ(1u, merger->switches());
  EXPECT_EQ(1u, merger->active_leg());
}

// Test 8: mid-stream garbage bytes, then clean packets: framing resyncs
// within 2 packets and the resyncs counter increments.
TEST_F(RedundantInputMergerTest, ResyncAfterMidStreamGarbage) {
  RedundantInputMerger::Config config;
  auto merger = MakeMerger(config);

  for (uint32_t i = 0; i < 2; ++i) {
    const std::vector<uint8_t> packet = BuildTsPacket(i);
    merger->OnBytes(0, packet.data(), packet.size(), i * 5);
  }
  ASSERT_EQ(2u, EmittedPackets());

  // Garbage (no 0x47) lands mid-stream.
  const std::vector<uint8_t> garbage(100, 0xAA);
  merger->OnBytes(0, garbage.data(), garbage.size(), 10);

  // First clean packet after the garbage: not emitted yet; the resync scan
  // needs the confirming 0x47 of the following packet.
  const std::vector<uint8_t> packet2 = BuildTsPacket(2);
  merger->OnBytes(0, packet2.data(), packet2.size(), 15);
  EXPECT_EQ(2u, EmittedPackets());

  // Second clean packet confirms sync: recovery within 2 packets.
  const std::vector<uint8_t> packet3 = BuildTsPacket(3);
  merger->OnBytes(0, packet3.data(), packet3.size(), 20);
  EXPECT_EQ(4u, EmittedPackets());
  EXPECT_EQ(1u, merger->GetLegStats(0).resyncs);

  const std::vector<uint8_t> packet4 = BuildTsPacket(4);
  merger->OnBytes(0, packet4.data(), packet4.size(), 25);
  EXPECT_EQ(BuildReference(SeqRange(5)), output_);
  EXPECT_EQ(0u, merger->emitted_cc_errors());
}

// Test 9: after more than dedup_window_pkts distinct packets, a repeat of
// packet #0 has been evicted from the window and is EMITTED again, proving
// the window (and thus memory) is bounded.
TEST_F(RedundantInputMergerTest, DedupWindowEvictionBoundsMemory) {
  const uint32_t kNumPackets = 5000;  // > default dedup_window_pkts (4096).
  RedundantInputMerger::Config config;
  auto merger = MakeMerger(config);

  // Same arrival time for all: only the packet-count bound can evict.
  for (uint32_t i = 0; i < kNumPackets; ++i) {
    const std::vector<uint8_t> packet = BuildTsPacket(i);
    merger->OnBytes(0, packet.data(), packet.size(), 1000);
  }
  ASSERT_EQ(kNumPackets, EmittedPackets());

  const std::vector<uint8_t> repeat = BuildTsPacket(0);
  merger->OnBytes(0, repeat.data(), repeat.size(), 1000);
  EXPECT_EQ(kNumPackets + 1, EmittedPackets());
  EXPECT_EQ(0u, merger->GetLegStats(0).dropped_dup);
  EXPECT_EQ(std::vector<uint8_t>(output_.end() - kPacketSize, output_.end()),
            repeat);
}

}  // namespace
}  // namespace shaka

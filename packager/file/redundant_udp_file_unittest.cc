// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/file/redundant_udp_file.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <packager/file.h>
#include <packager/metrics/metrics_service.h>

namespace shaka {
namespace {

using Config = RedundantInputMerger::Config;
using Mode = RedundantInputMerger::Mode;

TEST(RedundantUdpFileParseTest, TwoLegsWithDefaults) {
  std::vector<std::string> legs;
  Config config;
  ASSERT_TRUE(RedundantUdpFile::ParseUrl(
      "udp://239.1.1.1:5000?interface=10.0.0.1|udp://239.2.2.2:5000", &legs,
      &config));
  ASSERT_EQ(2u, legs.size());
  // Original options survive; a default timeout is injected when absent.
  EXPECT_EQ(0u, legs[0].find("udp://239.1.1.1:5000?interface=10.0.0.1"));
  EXPECT_NE(std::string::npos, legs[0].find("timeout="));
  EXPECT_NE(std::string::npos, legs[1].find("timeout="));
  EXPECT_EQ(Mode::kMerge, config.mode);
  EXPECT_EQ(2u, config.num_legs);
}

TEST(RedundantUdpFileParseTest, GlobalParamsStripped) {
  std::vector<std::string> legs;
  Config config;
  ASSERT_TRUE(RedundantUdpFile::ParseUrl(
      "udp://1.2.3.4:1000|udp://1.2.3.4:2000?interface=eth0"
      "&mode=failover&dedup_window_ms=300&failover_timeout_ms=500",
      &legs, &config));
  ASSERT_EQ(2u, legs.size());
  EXPECT_EQ(Mode::kFailover, config.mode);
  EXPECT_EQ(300, config.dedup_window_ms);
  EXPECT_EQ(500, config.failover_timeout_ms);
  // The leg keeps its own option but not the merger params.
  EXPECT_NE(std::string::npos, legs[1].find("interface=eth0"));
  EXPECT_EQ(std::string::npos, legs[1].find("mode="));
  EXPECT_EQ(std::string::npos, legs[1].find("dedup_window_ms="));
}

TEST(RedundantUdpFileParseTest, ExplicitTimeoutPreserved) {
  std::vector<std::string> legs;
  Config config;
  ASSERT_TRUE(RedundantUdpFile::ParseUrl(
      "udp://1.2.3.4:1000?timeout=250000|udp://1.2.3.4:2000", &legs,
      &config));
  EXPECT_NE(std::string::npos, legs[0].find("timeout=250000"));
}

TEST(RedundantUdpFileParseTest, RejectsSingleLeg) {
  std::vector<std::string> legs;
  Config config;
  EXPECT_FALSE(RedundantUdpFile::ParseUrl("udp://1.2.3.4:1000", &legs,
                                          &config));
}

TEST(RedundantUdpFileParseTest, RejectsNonUdpLeg) {
  std::vector<std::string> legs;
  Config config;
  EXPECT_FALSE(RedundantUdpFile::ParseUrl(
      "udp://1.2.3.4:1000|http://example.com/x", &legs, &config));
}

TEST(RedundantUdpFileParseTest, RejectsUnknownMode) {
  std::vector<std::string> legs;
  Config config;
  EXPECT_FALSE(RedundantUdpFile::ParseUrl(
      "udp://1.2.3.4:1000|udp://1.2.3.4:2000&mode=bogus", &legs, &config));
}

// Loopback smoke test: send the same synthetic TS packets to two local UDP
// ports and verify the merged output is byte-exact and deduplicated.
class RedundantUdpFileLoopbackTest : public ::testing::Test {
 protected:
  void SetUp() override {
    sender_ = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(sender_, 0);
    // Pick two free ports by binding ephemeral listeners first.
    for (int i = 0; i < 2; ++i) {
      int probe = socket(AF_INET, SOCK_DGRAM, 0);
      ASSERT_GE(probe, 0);
      sockaddr_in addr = {};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      addr.sin_port = 0;
      ASSERT_EQ(0, bind(probe, reinterpret_cast<sockaddr*>(&addr),
                        sizeof(addr)));
      socklen_t len = sizeof(addr);
      ASSERT_EQ(0, getsockname(probe, reinterpret_cast<sockaddr*>(&addr),
                               &len));
      ports_[i] = ntohs(addr.sin_port);
      close(probe);  // Freed; RedundantUdpFile re-binds it right away.
    }
  }

  void TearDown() override { close(sender_); }

  void SendTo(uint16_t port, const uint8_t* data, size_t size) {
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    sendto(sender_, data, size, 0, reinterpret_cast<sockaddr*>(&addr),
           sizeof(addr));
  }

  static std::vector<uint8_t> MakePacket(uint16_t pid, uint8_t cc,
                                         uint8_t fill) {
    std::vector<uint8_t> packet(188, fill);
    packet[0] = 0x47;
    packet[1] = (pid >> 8) & 0x1F;
    packet[2] = pid & 0xFF;
    packet[3] = 0x10 | (cc & 0x0F);  // payload only + CC
    return packet;
  }

  int sender_ = -1;
  uint16_t ports_[2] = {0, 0};
};

// Collects |target| bytes from |file| on this thread. The producer keeps
// sending keepalive packets until |stop_keepalive| is set, so the blocking
// Read always makes progress and the function terminates without needing to
// Close() the file from another thread (File is not safe for concurrent
// Read+Close, matching UdpFile semantics).
std::vector<uint8_t> CollectBytes(File* file, size_t target) {
  std::vector<uint8_t> received;
  received.reserve(target);
  std::vector<uint8_t> chunk(4096);
  while (received.size() < target) {
    const int64_t bytes = file->Read(chunk.data(), chunk.size());
    if (bytes <= 0)
      break;
    received.insert(received.end(), chunk.begin(), chunk.begin() + bytes);
  }
  return received;
}

TEST_F(RedundantUdpFileLoopbackTest, MergesDualLegByteExact) {
  const std::string url = "redundant://udp://127.0.0.1:" +
                          std::to_string(ports_[0]) +
                          "?timeout=50000|udp://127.0.0.1:" +
                          std::to_string(ports_[1]) + "?timeout=50000";
  File* file = File::Open(url.c_str(), "r");
  ASSERT_TRUE(file != nullptr);

  const int kNumPackets = 64;
  std::vector<uint8_t> expected;
  for (int i = 0; i < kNumPackets; ++i) {
    std::vector<uint8_t> packet =
        MakePacket(0x100, static_cast<uint8_t>(i), static_cast<uint8_t>(i));
    expected.insert(expected.end(), packet.begin(), packet.end());
  }

  std::atomic<bool> stop_keepalive{false};
  std::thread producer([&]() {
    for (int i = 0; i < kNumPackets; ++i) {
      // Both legs get every packet.
      SendTo(ports_[0], expected.data() + i * 188, 188);
      SendTo(ports_[1], expected.data() + i * 188, 188);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Keepalives (distinct PID/fill) guarantee reader progress even if a
    // payload datagram is lost; they arrive strictly after the payload.
    uint8_t ka_cc = 0;
    while (!stop_keepalive.load()) {
      std::vector<uint8_t> ka = MakePacket(0x1FE, ka_cc++, 0xEE);
      SendTo(ports_[0], ka.data(), ka.size());
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  });

  std::vector<uint8_t> received = CollectBytes(file, expected.size());
  stop_keepalive.store(true);
  producer.join();
  ASSERT_TRUE(file->Close());

  ASSERT_GE(received.size(), expected.size());
  EXPECT_EQ(0, memcmp(expected.data(), received.data(), expected.size()))
      << "merged output must be byte-exact and deduplicated";
}

TEST_F(RedundantUdpFileLoopbackTest, SurvivesLegSilence) {
  const std::string url = "redundant://udp://127.0.0.1:" +
                          std::to_string(ports_[0]) +
                          "?timeout=50000|udp://127.0.0.1:" +
                          std::to_string(ports_[1]) + "?timeout=50000";
  File* file = File::Open(url.c_str(), "r");
  ASSERT_TRUE(file != nullptr);

  const int kNumPackets = 32;
  std::vector<uint8_t> expected;
  for (int i = 0; i < kNumPackets; ++i) {
    // Unique fill per packet: with a constant fill, packets 16..31 would be
    // byte-identical to 0..15 (CC wraps mod 16) and correctly deduped.
    std::vector<uint8_t> packet =
        MakePacket(0x100, static_cast<uint8_t>(i), static_cast<uint8_t>(i));
    expected.insert(expected.end(), packet.begin(), packet.end());
  }

  std::atomic<bool> stop_keepalive{false};
  std::thread producer([&]() {
    for (int i = 0; i < kNumPackets; ++i) {
      // Leg 0 is dead from the start; only leg 1 delivers.
      SendTo(ports_[1], expected.data() + i * 188, 188);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    uint8_t ka_cc = 0;
    while (!stop_keepalive.load()) {
      std::vector<uint8_t> ka = MakePacket(0x1FE, ka_cc++, 0xEE);
      SendTo(ports_[1], ka.data(), ka.size());
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  });

  std::vector<uint8_t> received = CollectBytes(file, expected.size());
  stop_keepalive.store(true);
  producer.join();
  ASSERT_TRUE(file->Close());

  ASSERT_GE(received.size(), expected.size());
  EXPECT_EQ(0, memcmp(expected.data(), received.data(), expected.size()));
}

// Builds a valid, distinct 188-byte TS packet (mirrors BuildTsPacket in
// redundant_input_merger_unittest.cc).
std::vector<uint8_t> BuildTsPacket(uint32_t seq) {
  std::vector<uint8_t> packet(RedundantInputMerger::kTsPacketSize, 0xFF);
  packet[0] = 0x47;
  packet[1] = 0x01;
  packet[2] = 0x00;
  packet[3] = 0x10 | (seq & 0x0F);
  packet[4] = (seq >> 24) & 0xFF;
  packet[5] = (seq >> 16) & 0xFF;
  packet[6] = (seq >> 8) & 0xFF;
  packet[7] = seq & 0xFF;
  return packet;
}

// Returns the value of |family_name| for the metric whose "leg" label is
// |leg| (or the first metric in the family when |leg| is empty, for global
// families), or -1 when the family/metric is absent.
double RedundantMetricValue(const std::string& family_name,
                            const std::string& leg) {
  for (const auto& family :
       MetricsService::Instance().CollectAllForTesting()) {
    if (family.name != family_name)
      continue;
    for (const auto& metric : family.metric) {
      bool leg_matches = leg.empty();
      for (const auto& label : metric.label) {
        if (label.name == "leg" && label.value == leg)
          leg_matches = true;
      }
      if (leg_matches) {
        return family.type == prometheus::MetricType::Counter
                   ? metric.counter.value
                   : metric.gauge.value;
      }
    }
  }
  return -1;
}

TEST(RedundantUdpFileMetricsTest, SnapshotIsExportedViaCollectable) {
  // Probe two free loopback ports (same pattern as
  // RedundantUdpFileLoopbackTest::SetUp).
  uint16_t ports[2] = {0, 0};
  for (int i = 0; i < 2; ++i) {
    int probe = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(probe, 0);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ASSERT_EQ(0,
              bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
    socklen_t len = sizeof(addr);
    ASSERT_EQ(0,
              getsockname(probe, reinterpret_cast<sockaddr*>(&addr), &len));
    ports[i] = ntohs(addr.sin_port);
    close(probe);  // Freed; RedundantUdpFile re-binds it right away.
  }

  const std::string url = "redundant://udp://127.0.0.1:" +
                          std::to_string(ports[0]) +
                          "?timeout=50000|udp://127.0.0.1:" +
                          std::to_string(ports[1]) + "?timeout=50000";
  File* reader = File::Open(url.c_str(), "r");
  ASSERT_TRUE(reader != nullptr);

  const int sender = socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(sender, 0);
  auto send_to = [&](uint16_t port, const uint8_t* data, size_t size) {
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    sendto(sender, data, size, 0, reinterpret_cast<sockaddr*>(&addr),
           sizeof(addr));
  };

  const int kNumPackets = 10;
  std::vector<uint8_t> expected;
  for (int i = 0; i < kNumPackets; ++i) {
    const std::vector<uint8_t> packet = BuildTsPacket(i);
    expected.insert(expected.end(), packet.begin(), packet.end());
  }

  std::atomic<bool> stop_keepalive{false};
  std::thread producer([&]() {
    for (int i = 0; i < kNumPackets; ++i) {
      // Both legs get every packet, leg 0 first: the merge dedup window
      // drops the later (leg 1) arrival of each duplicate.
      send_to(ports[0], expected.data() + i * 188, 188);
      send_to(ports[1], expected.data() + i * 188, 188);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Keepalives guarantee reader progress even if a payload datagram is
    // lost; distinct seq values keep them out of the payload dedup window.
    uint32_t ka_seq = 1000;
    while (!stop_keepalive.load()) {
      const std::vector<uint8_t> ka = BuildTsPacket(ka_seq++);
      send_to(ports[0], ka.data(), ka.size());
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  });

  std::vector<uint8_t> received;
  received.reserve(expected.size());
  std::vector<uint8_t> chunk(4096);
  while (received.size() < expected.size()) {
    const int64_t bytes = reader->Read(chunk.data(), chunk.size());
    if (bytes <= 0)
      break;
    received.insert(received.end(), chunk.begin(), chunk.begin() + bytes);
  }
  stop_keepalive.store(true);
  producer.join();
  close(sender);

  ASSERT_GE(received.size(), expected.size());

  EXPECT_GE(RedundantMetricValue("shaka_redundant_leg_packets_total", "0"),
            10.0);
  EXPECT_GE(RedundantMetricValue("shaka_redundant_leg_packets_total", "1"),
            10.0);
  EXPECT_GE(RedundantMetricValue("shaka_redundant_leg_dropped_dup_total", "1"),
            1.0);
  EXPECT_DOUBLE_EQ(1.0,
                   RedundantMetricValue("shaka_redundant_leg_healthy", "0"));
  EXPECT_DOUBLE_EQ(
      0.0, RedundantMetricValue("shaka_redundant_switches_total", ""));

  ASSERT_TRUE(reader->Close());

  // After Close(), the collectable must be detached and destroyed: the
  // family disappears from scrapes entirely (stale-free absence, not a
  // stale last value).
  EXPECT_DOUBLE_EQ(
      -1.0, RedundantMetricValue("shaka_redundant_leg_packets_total", "0"));
}

}  // namespace
}  // namespace shaka

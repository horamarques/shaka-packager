// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <packager/file.h>
#include <packager/metrics/metrics_service.h>

namespace shaka {
namespace {

// Returns the value of |family_name| for the metric whose "input" label is
// |input|, or -1 when absent.
double MetricValue(const std::string& family_name, const std::string& input) {
  for (const auto& family : MetricsService::Instance().CollectAllForTesting()) {
    if (family.name != family_name)
      continue;
    for (const auto& metric : family.metric) {
      for (const auto& label : metric.label) {
        if (label.name == "input" && label.value == input)
          return family.type == prometheus::MetricType::Counter
                     ? metric.counter.value
                     : metric.gauge.value;
      }
    }
  }
  return -1;
}

TEST(UdpFileMetricsTest, CountsBytesDatagramsAndTimeouts) {
  // Bind an ephemeral port to find a free one, then reuse it.
  int probe = socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(probe, 0);
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(0, bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
  socklen_t addr_len = sizeof(addr);
  ASSERT_EQ(0, getsockname(probe, reinterpret_cast<sockaddr*>(&addr),
                           &addr_len));
  const int port = ntohs(addr.sin_port);
  close(probe);

  const std::string url =
      "udp://127.0.0.1:" + std::to_string(port) + "?timeout=200000";
  File* reader = File::OpenWithNoBuffering(url.c_str(), "r");
  ASSERT_TRUE(reader);

  // The label value is the URL without the scheme prefix consumed by the
  // factory; capture whatever file_name() reports via the registry after
  // the first read.
  int sender = socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(sender, 0);
  addr.sin_port = htons(port);
  const std::vector<uint8_t> datagram(1316, 0x47);
  for (int i = 0; i < 3; ++i) {
    ASSERT_EQ(static_cast<ssize_t>(datagram.size()),
              sendto(sender, datagram.data(), datagram.size(), 0,
                     reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
  }
  close(sender);

  std::vector<uint8_t> buffer(65536);
  int received = 0;
  // 3 reads succeed; the 4th hits the 200ms timeout.
  for (int i = 0; i < 4; ++i) {
    if (reader->Read(buffer.data(), buffer.size()) > 0)
      ++received;
  }
  ASSERT_EQ(3, received);

  const std::string input = "127.0.0.1:" + std::to_string(port) +
                            "?timeout=200000";
  EXPECT_DOUBLE_EQ(3.0,
                   MetricValue("shaka_udp_datagrams_received_total", input));
  EXPECT_DOUBLE_EQ(3.0 * 1316,
                   MetricValue("shaka_udp_bytes_received_total", input));
  EXPECT_GE(MetricValue("shaka_udp_recv_timeouts_total", input), 1.0);
  EXPECT_GT(MetricValue("shaka_udp_last_receive_timestamp_seconds", input),
            0.0);
#if !defined(__linux__)
  EXPECT_DOUBLE_EQ(-1.0, MetricValue("shaka_udp_kernel_drops_total", input));
#endif

  reader->Close();
}

}  // namespace
}  // namespace shaka

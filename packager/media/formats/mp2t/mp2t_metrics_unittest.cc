// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <packager/media/base/media_sample.h>
#include <packager/media/base/stream_info.h>
#include <packager/media/base/text_sample.h>
#include <packager/media/formats/mp2t/mp2t_media_parser.h>
#include <packager/media/test/test_data_util.h>
#include <packager/metrics/metrics_service.h>

namespace shaka {
namespace media {
namespace mp2t {
namespace {

double MetricValue(const std::string& family_name) {
  double total = 0;
  bool found = false;
  for (const auto& family :
       MetricsService::Instance().CollectAllForTesting()) {
    if (family.name != family_name)
      continue;
    for (const auto& metric : family.metric) {
      found = true;
      total += family.type == prometheus::MetricType::Counter
                   ? metric.counter.value
                   : metric.gauge.value;
    }
  }
  return found ? total : -1;
}

class Mp2tMetricsTest : public testing::Test {
 protected:
  void ParseBytes(const std::vector<uint8_t>& buffer) {
    parser_.Init(
        [](const std::vector<std::shared_ptr<StreamInfo>>&) {},
        [](uint32_t, std::shared_ptr<MediaSample>) { return true; },
        [](uint32_t, std::shared_ptr<TextSample>) { return true; },
        nullptr);
    ASSERT_TRUE(parser_.Parse(buffer.data(), static_cast<int>(buffer.size())));
  }
  Mp2tMediaParser parser_;
};

TEST_F(Mp2tMetricsTest, DroppedPacketsIncrementCcErrors) {
  const std::vector<uint8_t> buffer = ReadTestDataFile("bear-640x360.ts");
  ASSERT_FALSE(buffer.empty());
  // Drop every 10th 188-byte packet to force CC gaps on enabled PIDs.
  std::vector<uint8_t> holey;
  for (size_t i = 0; i * 188 < buffer.size(); ++i) {
    if (i % 10 == 9)
      continue;
    holey.insert(holey.end(), buffer.begin() + i * 188,
                 buffer.begin() + std::min((i + 1) * 188, buffer.size()));
  }
  const double cc_before = MetricValue("shaka_ts_cc_errors_total");
  ParseBytes(holey);
  const double cc_after = MetricValue("shaka_ts_cc_errors_total");
  EXPECT_GT(cc_after, cc_before < 0 ? 0 : cc_before);
  // The biggest-PTS gauge must be exported and positive.
  EXPECT_GT(MetricValue("shaka_media_latest_pts_seconds"), 0.0);
}

TEST_F(Mp2tMetricsTest, TeiFlagIsCounted) {
  // One syntactically valid TS packet with TEI set on an unknown PID.
  std::vector<uint8_t> packet(188, 0xFF);
  packet[0] = 0x47;
  packet[1] = 0x80 | 0x1F;  // TEI=1, PUSI=0, priority=0, pid high = 0x1F..
  packet[2] = 0xFE;         // ..pid low: 0x1FFE (unknown, not NULL 0x1FFF)
  packet[3] = 0x10;         // no adaptation field, payload only, CC=0
  const double tei_before = MetricValue("shaka_ts_tei_packets_total");
  ParseBytes(packet);
  const double tei_after = MetricValue("shaka_ts_tei_packets_total");
  EXPECT_DOUBLE_EQ((tei_before < 0 ? 0 : tei_before) + 1, tei_after);
}

}  // namespace
}  // namespace mp2t
}  // namespace media
}  // namespace shaka

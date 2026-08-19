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

// Like MetricValue, but restricted to the series carrying label
// {"pid": pid_value}. Needed because the registry is process-global and
// shared across the whole mp2t_unittest binary: an unfiltered MetricValue
// sums across every PID's series, so it cannot tell "this PID's gauge was
// written" from "some other PID's gauge was written".
double MetricValueForLabel(const std::string& family_name,
                            const std::string& pid_value) {
  double total = 0;
  bool found = false;
  for (const auto& family :
       MetricsService::Instance().CollectAllForTesting()) {
    if (family.name != family_name)
      continue;
    for (const auto& metric : family.metric) {
      bool label_matches = false;
      for (const auto& label : metric.label) {
        if (label.name == "pid" && label.value == pid_value) {
          label_matches = true;
          break;
        }
      }
      if (!label_matches)
        continue;
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
}

// PID 256 is the video PID of bear-640x360.ts and carries ~79% of its
// packets (verified by counting PIDs in the raw file). This test parses the
// clip uncorrupted -- unlike DroppedPacketsIncrementCcErrors above, dropping
// 1-in-10 packets was verified (by instrumenting this test during
// development) to break the H.264 video elementary stream badly enough that
// no video access unit ever completes, so pid 256's gauge stays unset in the
// holey-parse case; only the audio PID happens to survive that corruption.
// A clean parse is therefore the only way to reliably exercise this task's
// OnEmitMediaSample -> set_latest_pts_seconds wiring on the video PID.
TEST_F(Mp2tMetricsTest, LatestPtsGaugeTracksVideoPid) {
  const std::vector<uint8_t> buffer = ReadTestDataFile("bear-640x360.ts");
  ASSERT_FALSE(buffer.empty());
  ParseBytes(buffer);
  const double pts_after =
      MetricValueForLabel("shaka_media_latest_pts_seconds", "256");

  // A before/after delta comparison (pts_after >= pts_before) was tried
  // first, per plan, but instrumenting this test proved it unsound: the
  // gauge is a plain prometheus::Gauge::Set(), not a running max, and
  // mp2t_media_parser_unittest.cc separately parses a PTS-wraparound clip
  // on this same PID 256 elsewhere in this binary, legitimately driving the
  // shared gauge to ~95458 (a ~2^33-tick timestamp expressed in seconds).
  // When that test runs before this one in the full mp2t_unittest suite,
  // pts_before is ~95458 and this clip's true PTS (~2.6s) is *smaller*, so
  // ">=" fails even though set_latest_pts_seconds() fired correctly -- the
  // gauge is doing exactly what "latest observed PTS" should do. Since
  // gtest runs sequentially, nothing else can write this PID's gauge
  // between our ParseBytes() call and the read directly below, so pts_after
  // always reflects *our* parse. The proof is therefore a tight,
  // file-specific envelope instead of a delta: bear-640x360.ts is a ~2.7s
  // clip, and this repeatedly measures a PTS of ~2.60s -- a window far from
  // both 0 (never written) and the ~95458s a wraparound-clip write would
  // leave behind, so landing inside it is strong evidence this test's own
  // parse (and not some earlier test's leftover value) produced it.
  EXPECT_GT(pts_after, 2.0);
  EXPECT_LT(pts_after, 2.7);
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

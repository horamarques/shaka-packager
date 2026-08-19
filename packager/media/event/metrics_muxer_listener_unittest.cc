// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/event/metrics_muxer_listener.h>

#include <string>

#include <gtest/gtest.h>

#include <packager/media/base/video_stream_info.h>
#include <packager/media/event/muxer_listener_test_helper.h>
#include <packager/metrics/metrics_service.h>

namespace shaka {
namespace media {
namespace {

double MetricValue(const std::string& family_name,
                   const std::string& stream_label,
                   const std::string& direction = "") {
  for (const auto& family :
       MetricsService::Instance().CollectAllForTesting()) {
    if (family.name != family_name)
      continue;
    for (const auto& metric : family.metric) {
      bool stream_ok = false, direction_ok = direction.empty();
      for (const auto& label : metric.label) {
        if (label.name == "stream" && label.value == stream_label)
          stream_ok = true;
        if (label.name == "direction" && label.value == direction)
          direction_ok = true;
      }
      if (stream_ok && direction_ok) {
        return family.type == prometheus::MetricType::Counter
                   ? metric.counter.value
                   : metric.gauge.value;
      }
    }
  }
  return -1;
}

TEST(MetricsMuxerListenerTest, TalliesSegmentsCuesAndKeyRotations) {
  // Unique label per test run keeps assertions absolute despite the
  // process-global registry.
  const std::string label = "test_stream_7";
  MetricsMuxerListener listener(label);

  MuxerOptions muxer_options;
  std::shared_ptr<StreamInfo> stream_info =
      CreateVideoStreamInfo(GetDefaultVideoStreamInfoParams());
  listener.OnMediaStart(muxer_options, *stream_info, 90000,
                        MuxerListener::kContainerMpeg2ts);

  listener.OnNewSegment("seg_1.ts", /*start_time=*/900000,
                        /*duration=*/180000, /*segment_file_size=*/500000,
                        /*segment_number=*/1);
  listener.OnNewSegment("seg_2.ts", 1080000, 180000, 600000, 2);

  EXPECT_DOUBLE_EQ(2.0, MetricValue("shaka_segments_emitted_total", label));
  EXPECT_DOUBLE_EQ(1100000.0,
                   MetricValue("shaka_segment_bytes_total", label));
  EXPECT_DOUBLE_EQ(2.0,
                   MetricValue("shaka_last_segment_duration_seconds", label));
  EXPECT_DOUBLE_EQ((1080000.0 + 180000.0) / 90000.0,
                   MetricValue("shaka_last_segment_timestamp_seconds", label));

  listener.OnCueEvent(1260000, "", /*is_cue_out=*/true, 30.0);
  listener.OnCueEvent(1290000, "", /*is_cue_out=*/false, 0.0);
  EXPECT_DOUBLE_EQ(1.0,
                   MetricValue("shaka_cue_events_total", label, "out"));
  EXPECT_DOUBLE_EQ(1.0, MetricValue("shaka_cue_events_total", label, "in"));

  const std::vector<uint8_t> key_id(16, 1), iv(8, 2);
  listener.OnEncryptionInfoReady(/*is_initial_encryption_info=*/true,
                                 FOURCC_cenc, key_id, iv, {});
  listener.OnEncryptionInfoReady(/*is_initial_encryption_info=*/false,
                                 FOURCC_cenc, key_id, iv, {});
  EXPECT_DOUBLE_EQ(1.0, MetricValue("shaka_key_rotations_total", label));
}

}  // namespace
}  // namespace media
}  // namespace shaka

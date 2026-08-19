// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_MEDIA_EVENT_METRICS_MUXER_LISTENER_H_
#define PACKAGER_MEDIA_EVENT_METRICS_MUXER_LISTENER_H_

#include <string>

#include <packager/media/event/muxer_listener.h>

namespace prometheus {
class Counter;
class Gauge;
}  // namespace prometheus

namespace shaka {
namespace media {

/// MuxerListener that tallies per-segment output metrics into the process
/// MetricsService registry. Purely observational: every callback is a
/// counter/gauge update, no I/O.
class MetricsMuxerListener : public MuxerListener {
 public:
  /// @param stream_label becomes the {stream} label on every metric.
  explicit MetricsMuxerListener(const std::string& stream_label);

  /// @name MuxerListener implementation overrides.
  /// @{
  void OnEncryptionInfoReady(bool is_initial_encryption_info,
                             FourCC protection_scheme,
                             const std::vector<uint8_t>& key_id,
                             const std::vector<uint8_t>& iv,
                             const std::vector<ProtectionSystemSpecificInfo>&
                                 key_system_info) override;
  void OnEncryptionStart() override;
  void OnMediaStart(const MuxerOptions& muxer_options,
                    const StreamInfo& stream_info,
                    int32_t time_scale,
                    ContainerType container_type) override;
  void OnSampleDurationReady(int32_t sample_duration) override;
  void OnMediaEnd(const MediaRanges& media_ranges,
                  float duration_seconds) override;
  void OnNewSegment(const std::string& segment_name,
                    int64_t start_time,
                    int64_t duration,
                    uint64_t segment_file_size,
                    int64_t segment_number) override;
  void OnKeyFrame(int64_t timestamp,
                  uint64_t start_byte_offset,
                  uint64_t size) override;
  void OnCueEvent(int64_t timestamp,
                  const std::string& cue_data,
                  bool is_cue_out,
                  double duration_in_seconds) override;
  /// @}

 private:
  MetricsMuxerListener(const MetricsMuxerListener&) = delete;
  MetricsMuxerListener& operator=(const MetricsMuxerListener&) = delete;

  int32_t time_scale_ = 0;

  prometheus::Counter* const segments_total_;
  prometheus::Counter* const segment_bytes_total_;
  prometheus::Gauge* const last_segment_duration_seconds_;
  prometheus::Gauge* const last_segment_timestamp_seconds_;
  prometheus::Counter* const cue_out_events_total_;
  prometheus::Counter* const cue_in_events_total_;
  prometheus::Counter* const key_rotations_total_;
};

}  // namespace media
}  // namespace shaka

#endif  // PACKAGER_MEDIA_EVENT_METRICS_MUXER_LISTENER_H_

// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/event/metrics_muxer_listener.h>

#include <prometheus/counter.h>
#include <prometheus/gauge.h>

#include <packager/macros/compiler.h>
#include <packager/metrics/metrics_service.h>

namespace shaka {
namespace media {
namespace {

prometheus::Counter* AddCounter(const char* name,
                                const char* help,
                                const prometheus::Labels& labels) {
  return &prometheus::BuildCounter()
              .Name(name)
              .Help(help)
              .Register(MetricsService::Instance().registry())
              .Add(labels);
}

prometheus::Gauge* AddGauge(const char* name,
                            const char* help,
                            const prometheus::Labels& labels) {
  return &prometheus::BuildGauge()
              .Name(name)
              .Help(help)
              .Register(MetricsService::Instance().registry())
              .Add(labels);
}

}  // namespace

MetricsMuxerListener::MetricsMuxerListener(const std::string& stream_label)
    : segments_total_(AddCounter("shaka_segments_emitted_total",
                                 "Media segments emitted.",
                                 {{"stream", stream_label}})),
      segment_bytes_total_(AddCounter("shaka_segment_bytes_total",
                                      "Total bytes of emitted segments.",
                                      {{"stream", stream_label}})),
      last_segment_duration_seconds_(
          AddGauge("shaka_last_segment_duration_seconds",
                   "Duration of the most recent segment.",
                   {{"stream", stream_label}})),
      last_segment_timestamp_seconds_(
          AddGauge("shaka_last_segment_timestamp_seconds",
                   "Media-timeline end of the most recent segment.",
                   {{"stream", stream_label}})),
      cue_out_events_total_(AddCounter("shaka_cue_events_total",
                                       "Ad-cue events by direction.",
                                       {{"stream", stream_label},
                                        {"direction", "out"}})),
      cue_in_events_total_(AddCounter("shaka_cue_events_total",
                                      "Ad-cue events by direction.",
                                      {{"stream", stream_label},
                                       {"direction", "in"}})),
      key_rotations_total_(AddCounter("shaka_key_rotations_total",
                                      "Encryption key rotations.",
                                      {{"stream", stream_label}})) {}

void MetricsMuxerListener::OnEncryptionInfoReady(
    bool is_initial_encryption_info,
    FourCC protection_scheme,
    const std::vector<uint8_t>& key_id,
    const std::vector<uint8_t>& iv,
    const std::vector<ProtectionSystemSpecificInfo>& key_system_info) {
  UNUSED(protection_scheme);
  UNUSED(key_id);
  UNUSED(iv);
  UNUSED(key_system_info);
  if (!is_initial_encryption_info)
    key_rotations_total_->Increment();
}

void MetricsMuxerListener::OnEncryptionStart() {}

void MetricsMuxerListener::OnMediaStart(const MuxerOptions& muxer_options,
                                        const StreamInfo& stream_info,
                                        int32_t time_scale,
                                        ContainerType container_type) {
  UNUSED(muxer_options);
  UNUSED(stream_info);
  UNUSED(container_type);
  time_scale_ = time_scale;
}

void MetricsMuxerListener::OnSampleDurationReady(int32_t sample_duration) {
  UNUSED(sample_duration);
}

void MetricsMuxerListener::OnMediaEnd(const MediaRanges& media_ranges,
                                      float duration_seconds) {
  UNUSED(media_ranges);
  UNUSED(duration_seconds);
}

void MetricsMuxerListener::OnNewSegment(const std::string& segment_name,
                                        int64_t start_time,
                                        int64_t duration,
                                        uint64_t segment_file_size,
                                        int64_t segment_number) {
  UNUSED(segment_name);
  UNUSED(segment_number);
  segments_total_->Increment();
  segment_bytes_total_->Increment(static_cast<double>(segment_file_size));
  if (time_scale_ > 0) {
    last_segment_duration_seconds_->Set(static_cast<double>(duration) /
                                        time_scale_);
    last_segment_timestamp_seconds_->Set(
        static_cast<double>(start_time + duration) / time_scale_);
  }
}

void MetricsMuxerListener::OnKeyFrame(int64_t timestamp,
                                      uint64_t start_byte_offset,
                                      uint64_t size) {
  UNUSED(timestamp);
  UNUSED(start_byte_offset);
  UNUSED(size);
}

void MetricsMuxerListener::OnCueEvent(int64_t timestamp,
                                      const std::string& cue_data,
                                      bool is_cue_out,
                                      double duration_in_seconds) {
  UNUSED(timestamp);
  UNUSED(cue_data);
  UNUSED(duration_in_seconds);
  (is_cue_out ? cue_out_events_total_ : cue_in_events_total_)->Increment();
}

}  // namespace media
}  // namespace shaka

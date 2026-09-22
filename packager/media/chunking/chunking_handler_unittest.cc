// Copyright 2017 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/chunking/chunking_handler.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <packager/chunking_params.h>
#include <packager/media/base/media_handler.h>
#include <packager/media/base/media_handler_test_base.h>
#include <packager/media/chunking/epoch_segment_numbering.h>
#include <packager/status.h>
#include <packager/status/status_test_util.h>

using ::testing::_;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

namespace shaka {
namespace media {
namespace {
const size_t kStreamIndex = 0;
const int32_t kTimeScale0 = 800;
const int32_t kTimeScale1 = 1000;
const int64_t kDuration = 300;
const bool kKeyFrame = true;
const bool kIsSubsegment = true;
const bool kEncrypted = true;

// No matcher for segment_number exists in media_handler_test_base.h, so we
// pull the numbers out of the kSegmentInfo entries directly.
std::vector<int64_t> GetSegmentNumbers(
    const std::vector<std::unique_ptr<StreamData>>& stream_data_vector) {
  std::vector<int64_t> segment_numbers;
  for (const auto& stream_data : stream_data_vector) {
    if (stream_data->stream_data_type == StreamDataType::kSegmentInfo)
      segment_numbers.push_back(stream_data->segment_info->segment_number);
  }
  return segment_numbers;
}

}  // namespace

class ChunkingHandlerTest : public MediaHandlerGraphTestBase {
 public:
  void SetUpChunkingHandler(int num_inputs,
                            const ChunkingParams& chunking_params) {
    chunking_handler_.reset(new ChunkingHandler(chunking_params));
    SetUpGraph(num_inputs, num_inputs, chunking_handler_);
    ASSERT_OK(chunking_handler_->Initialize());
  }

  Status Process(std::unique_ptr<StreamData> stream_data) {
    return chunking_handler_->Process(std::move(stream_data));
  }

  Status OnFlushRequest(int stream_index) {
    return chunking_handler_->OnFlushRequest(stream_index);
  }

  // Overrides the wall-clock reading used to seed the PTS wrap offset, via
  // the ChunkingHandlerTest friendship (there is no production setter).
  void SetNowForTesting(int64_t now_us) {
    chunking_handler_->now_us_for_testing_ = [now_us] { return now_us; };
  }

 protected:
  std::shared_ptr<ChunkingHandler> chunking_handler_;
};

TEST_F(ChunkingHandlerTest, AudioNoSubsegmentsThenFlush) {
  ChunkingParams chunking_params;
  chunking_params.segment_duration_in_seconds = 1;
  SetUpChunkingHandler(1, chunking_params);

  ASSERT_OK(Process(StreamData::FromStreamInfo(
      kStreamIndex, GetAudioStreamInfo(kTimeScale0))));
  EXPECT_THAT(
      GetOutputStreamDataVector(),
      ElementsAre(IsStreamInfo(kStreamIndex, kTimeScale0, !kEncrypted, _)));

  for (int i = 0; i < 5; ++i) {
    ClearOutputStreamDataVector();
    ASSERT_OK(Process(StreamData::FromMediaSample(
        kStreamIndex, GetMediaSample(i * kDuration, kDuration, kKeyFrame))));
    // One output stream_data except when i == 3, which also has SegmentInfo.
    if (i == 3) {
      EXPECT_THAT(GetOutputStreamDataVector(),
                  ElementsAre(IsSegmentInfo(kStreamIndex, 0, kDuration * 3,
                                            !kIsSubsegment, !kEncrypted),
                              IsMediaSample(kStreamIndex, i * kDuration,
                                            kDuration, !kEncrypted, _)));
    } else {
      EXPECT_THAT(GetOutputStreamDataVector(),
                  ElementsAre(IsMediaSample(kStreamIndex, i * kDuration,
                                            kDuration, !kEncrypted, _)));
    }
  }

  ClearOutputStreamDataVector();
  ASSERT_OK(OnFlushRequest(kStreamIndex));
  EXPECT_THAT(
      GetOutputStreamDataVector(),
      ElementsAre(IsSegmentInfo(kStreamIndex, kDuration * 3, kDuration * 2,
                                !kIsSubsegment, !kEncrypted)));
}

TEST_F(ChunkingHandlerTest, AudioWithSubsegments) {
  ChunkingParams chunking_params;
  chunking_params.segment_duration_in_seconds = 1;
  chunking_params.subsegment_duration_in_seconds = 0.5;
  SetUpChunkingHandler(1, chunking_params);

  ASSERT_OK(Process(StreamData::FromStreamInfo(
      kStreamIndex, GetAudioStreamInfo(kTimeScale0))));
  for (int i = 0; i < 5; ++i) {
    ASSERT_OK(Process(StreamData::FromMediaSample(
        kStreamIndex, GetMediaSample(i * kDuration, kDuration, kKeyFrame))));
  }
  EXPECT_THAT(
      GetOutputStreamDataVector(),
      ElementsAre(
          IsStreamInfo(kStreamIndex, kTimeScale0, !kEncrypted, _),
          IsMediaSample(kStreamIndex, 0, kDuration, !kEncrypted, _),
          IsMediaSample(kStreamIndex, kDuration, kDuration, !kEncrypted, _),
          IsSegmentInfo(kStreamIndex, 0, kDuration * 2, kIsSubsegment,
                        !kEncrypted),
          IsMediaSample(kStreamIndex, 2 * kDuration, kDuration, !kEncrypted, _),
          IsSegmentInfo(kStreamIndex, 0, kDuration * 3, !kIsSubsegment,
                        !kEncrypted),
          IsMediaSample(kStreamIndex, 3 * kDuration, kDuration, !kEncrypted, _),
          IsMediaSample(kStreamIndex, 4 * kDuration, kDuration, !kEncrypted,
                        _)));
}

TEST_F(ChunkingHandlerTest, VideoAndSubsegmentAndNonzeroStart) {
  ChunkingParams chunking_params;
  chunking_params.segment_duration_in_seconds = 1;
  chunking_params.subsegment_duration_in_seconds = 0.3;
  SetUpChunkingHandler(1, chunking_params);

  ASSERT_OK(Process(StreamData::FromStreamInfo(
      kStreamIndex, GetVideoStreamInfo(kTimeScale1))));
  const int64_t kVideoStartTimestamp = 12345;
  for (int i = 0; i < 6; ++i) {
    // Alternate key frame.
    const bool is_key_frame = (i % 2) == 1;
    ASSERT_OK(Process(StreamData::FromMediaSample(
        kStreamIndex, GetMediaSample(kVideoStartTimestamp + i * kDuration,
                                     kDuration, is_key_frame))));
  }
  EXPECT_THAT(
      GetOutputStreamDataVector(),
      ElementsAre(
          IsStreamInfo(kStreamIndex, kTimeScale1, !kEncrypted, _),
          // The first samples @ kStartTimestamp is discarded - not key frame.
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration,
                        kDuration, !kEncrypted, _),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 2,
                        kDuration, !kEncrypted, _),
          // The next segment boundary 13245 / 1000 != 12645 / 1000.
          IsSegmentInfo(kStreamIndex, kVideoStartTimestamp + kDuration,
                        kDuration * 2, !kIsSubsegment, !kEncrypted),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 3,
                        kDuration, !kEncrypted, _),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 4,
                        kDuration, !kEncrypted, _),
          // The subsegment has duration kDuration * 2 since it can only
          // terminate before key frame.
          IsSegmentInfo(kStreamIndex, kVideoStartTimestamp + kDuration * 3,
                        kDuration * 2, kIsSubsegment, !kEncrypted),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 5,
                        kDuration, !kEncrypted, _)));
}

TEST_F(ChunkingHandlerTest, CueEvent) {
  ChunkingParams chunking_params;
  chunking_params.segment_duration_in_seconds = 1;
  chunking_params.subsegment_duration_in_seconds = 0.5;
  SetUpChunkingHandler(1, chunking_params);

  ASSERT_OK(Process(StreamData::FromStreamInfo(
      kStreamIndex, GetVideoStreamInfo(kTimeScale1))));
  ClearOutputStreamDataVector();

  const int64_t kVideoStartTimestamp = 12345;
  const double kCueTimeInSeconds =
      static_cast<double>(kVideoStartTimestamp + kDuration) / kTimeScale1;

  auto cue_event = std::make_shared<CueEvent>();
  cue_event->time_in_seconds = kCueTimeInSeconds;

  for (int i = 0; i < 6; ++i) {
    const bool is_key_frame = true;
    ASSERT_OK(Process(StreamData::FromMediaSample(
        kStreamIndex, GetMediaSample(kVideoStartTimestamp + i * kDuration,
                                     kDuration, is_key_frame))));
    if (i == 0) {
      ASSERT_OK(Process(StreamData::FromCueEvent(kStreamIndex, cue_event)));
    }
  }

  EXPECT_THAT(
      GetOutputStreamDataVector(),
      ElementsAre(
          IsMediaSample(kStreamIndex, kVideoStartTimestamp, kDuration,
                        !kEncrypted, _),
          // A new segment is created due to the existance of Cue.
          IsSegmentInfo(kStreamIndex, kVideoStartTimestamp, kDuration,
                        !kIsSubsegment, !kEncrypted),
          IsCueEvent(kStreamIndex, kCueTimeInSeconds),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 1,
                        kDuration, !kEncrypted, _),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 2,
                        kDuration, !kEncrypted, _),
          IsSegmentInfo(kStreamIndex, kVideoStartTimestamp + kDuration,
                        kDuration * 2, kIsSubsegment, !kEncrypted),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 3,
                        kDuration, !kEncrypted, _),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 4,
                        kDuration, !kEncrypted, _),
          IsSegmentInfo(kStreamIndex, kVideoStartTimestamp + kDuration,
                        kDuration * 4, !kIsSubsegment, !kEncrypted),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 5,
                        kDuration, !kEncrypted, _)));
}

TEST_F(ChunkingHandlerTest, LowLatencyDash) {
  ChunkingParams chunking_params;
  chunking_params.low_latency_dash_mode = true;
  chunking_params.segment_duration_in_seconds = 1;
  SetUpChunkingHandler(1, chunking_params);

  // Each completed segment will contain 2 chunks
  const int64_t kChunkDurationInMs = 500;
  const int64_t kSegmentDurationInMs = 1000;

  ASSERT_OK(Process(StreamData::FromStreamInfo(
      kStreamIndex, GetVideoStreamInfo(kTimeScale1))));

  for (int i = 0; i < 4; ++i) {
    ASSERT_OK(Process(StreamData::FromMediaSample(
        kStreamIndex, GetMediaSample(i * kChunkDurationInMs, kChunkDurationInMs,
                                     kKeyFrame))));
  }

  // NOTE: Each MediaSample will create a chunk, dispatching SegmentInfo
  EXPECT_THAT(
      GetOutputStreamDataVector(),
      ElementsAre(
          IsStreamInfo(kStreamIndex, kTimeScale1, !kEncrypted, _),
          // Chunk 1 for segment 1
          IsMediaSample(kStreamIndex, 0, kChunkDurationInMs, !kEncrypted, _),
          IsSegmentInfo(kStreamIndex, 0, kChunkDurationInMs, kIsSubsegment,
                        !kEncrypted),
          // Chunk 2 for segment 1
          IsMediaSample(kStreamIndex, kChunkDurationInMs, kChunkDurationInMs,
                        !kEncrypted, _),
          IsSegmentInfo(kStreamIndex, 0, 2 * kChunkDurationInMs, !kIsSubsegment,
                        !kEncrypted),
          // Chunk 1 for segment 2
          IsMediaSample(kStreamIndex, kSegmentDurationInMs, kChunkDurationInMs,
                        !kEncrypted, _),
          IsSegmentInfo(kStreamIndex, kSegmentDurationInMs, kChunkDurationInMs,
                        kIsSubsegment, !kEncrypted),
          // Chunk 2 for segment 2
          IsMediaSample(kStreamIndex, kSegmentDurationInMs + kChunkDurationInMs,
                        kChunkDurationInMs, !kEncrypted, _)));
}

TEST_F(ChunkingHandlerTest, EpochAnchoredSegmentNumbers) {
  // Anchor PTS 0 at 2026-01-01T00:00:00Z with 1 second segments, so the
  // expected first number is that instant in seconds.
  const int64_t k2026Us = 1767225600000000LL;
  ChunkingParams chunking_params;
  chunking_params.segment_duration_in_seconds = 1;
  chunking_params.segment_number_epoch_us = k2026Us;
  SetUpChunkingHandler(1, chunking_params);
  // Pin the clock to the anchor itself (PTS 0) so the wrap-offset seeding
  // added for restart-safety resolves to zero wraps here, independent of the
  // real wall-clock date this test happens to run on.
  SetNowForTesting(k2026Us);

  ASSERT_OK(Process(StreamData::FromStreamInfo(
      kStreamIndex, GetVideoStreamInfo(kTimeScale1))));
  // Two 1-second segments starting at PTS 0.
  ASSERT_OK(Process(StreamData::FromMediaSample(
      kStreamIndex, GetMediaSample(0, kTimeScale1, kKeyFrame))));
  ASSERT_OK(Process(StreamData::FromMediaSample(
      kStreamIndex, GetMediaSample(kTimeScale1, kTimeScale1, kKeyFrame))));
  ASSERT_OK(Process(StreamData::FromMediaSample(
      kStreamIndex, GetMediaSample(2 * kTimeScale1, kTimeScale1, kKeyFrame))));

  const int64_t expected_first = k2026Us / 1000000;
  EXPECT_THAT(GetSegmentNumbers(GetOutputStreamDataVector()),
              ElementsAre(expected_first, expected_first + 1));
}

TEST_F(ChunkingHandlerTest, WithoutEpochUsesStartSegmentNumber) {
  ChunkingParams chunking_params;
  chunking_params.segment_duration_in_seconds = 1;
  chunking_params.start_segment_number = 7;
  SetUpChunkingHandler(1, chunking_params);

  ASSERT_OK(Process(StreamData::FromStreamInfo(
      kStreamIndex, GetVideoStreamInfo(kTimeScale1))));
  ASSERT_OK(Process(StreamData::FromMediaSample(
      kStreamIndex, GetMediaSample(0, kTimeScale1, kKeyFrame))));
  ASSERT_OK(Process(StreamData::FromMediaSample(
      kStreamIndex, GetMediaSample(kTimeScale1, kTimeScale1, kKeyFrame))));

  EXPECT_THAT(GetSegmentNumbers(GetOutputStreamDataVector()), ElementsAre(7));
}

TEST_F(ChunkingHandlerTest, EpochNumbersSurviveRestartAfterPtsWrap) {
  // Simulate a restart 30 hours into a stream: the input PTS has wrapped
  // once, so the raw value is small, but the number must continue as though
  // the instance had been running throughout.
  const int32_t kTimeScale90k = 90000;
  const int64_t k2026Us = 1767225600000000LL;
  const int64_t kThirtyHoursUs = 30LL * 3600 * 1000000;
  ChunkingParams chunking_params;
  chunking_params.segment_duration_in_seconds = 1;
  chunking_params.segment_number_epoch_us = k2026Us;
  SetUpChunkingHandler(1, chunking_params);
  SetNowForTesting(k2026Us + kThirtyHoursUs);

  ASSERT_OK(Process(StreamData::FromStreamInfo(
      kStreamIndex, GetVideoStreamInfo(kTimeScale90k))));
  // Raw PTS just after the wrap.
  ASSERT_OK(Process(StreamData::FromMediaSample(
      kStreamIndex, GetMediaSample(0, kTimeScale90k, kKeyFrame))));
  ASSERT_OK(Process(StreamData::FromMediaSample(
      kStreamIndex,
      GetMediaSample(kTimeScale90k, kTimeScale90k, kKeyFrame))));

  // 2^33 ticks at 90kHz is 95443 whole seconds.
  const int64_t expected = k2026Us / 1000000 + 95443;
  EXPECT_THAT(GetSegmentNumbers(GetOutputStreamDataVector()),
              ElementsAre(expected));
}

TEST_F(ChunkingHandlerTest, EpochNumbersMonotonicAcrossMidStreamWrap) {
  // Task 3's tests used PTS values far below the 2^33 wrap threshold, so they
  // would still pass even if EpochSegmentNumber were fed the WRAPPED
  // timestamp instead of the value pts_unwrapper_ has unwrapped. Drive the
  // handler across a real mid-stream wrap (no restart involved: the clock
  // matches the stream exactly, so the seeded wrap offset is zero) and
  // confirm segment numbers keep climbing instead of jumping back to the
  // small numbers implied by the raw wrapped PTS.
  const int32_t kTimeScale90k = 90000;
  const int64_t k2026Us = 1767225600000000LL;
  // One segment before the 2^33 wrap boundary.
  const int64_t kPreWrapPts = (kPtsWrapAround / kTimeScale90k) * kTimeScale90k;
  // The same timeline position as an encoder emitting 33-bit PTS would
  // actually send it: wrapped back down near zero.
  const int64_t kPostWrapPts1 =
      (kPreWrapPts + kTimeScale90k) % kPtsWrapAround;
  const int64_t kPostWrapPts2 = kPostWrapPts1 + kTimeScale90k;

  ChunkingParams chunking_params;
  chunking_params.segment_duration_in_seconds = 1;
  chunking_params.segment_number_epoch_us = k2026Us;
  SetUpChunkingHandler(1, chunking_params);
  SetNowForTesting(k2026Us + PtsToMicroseconds(kPreWrapPts, kTimeScale90k));

  ASSERT_OK(Process(StreamData::FromStreamInfo(
      kStreamIndex, GetVideoStreamInfo(kTimeScale90k))));
  ASSERT_OK(Process(StreamData::FromMediaSample(
      kStreamIndex, GetMediaSample(kPreWrapPts, kTimeScale90k, kKeyFrame))));
  ASSERT_OK(Process(StreamData::FromMediaSample(
      kStreamIndex,
      GetMediaSample(kPostWrapPts1, kTimeScale90k, kKeyFrame))));
  ASSERT_OK(Process(StreamData::FromMediaSample(
      kStreamIndex,
      GetMediaSample(kPostWrapPts2, kTimeScale90k, kKeyFrame))));

  const int64_t first =
      k2026Us / 1000000 + kPreWrapPts / kTimeScale90k;
  EXPECT_THAT(GetSegmentNumbers(GetOutputStreamDataVector()),
              ElementsAre(first, first + 1));
}

}  // namespace media
}  // namespace shaka

// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/formats/mp4/low_latency_segment_segmenter.h>

#include <cstdint>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <packager/file.h>
#include <packager/file/file_closer.h>
#include <packager/macros/status.h>
#include <packager/media/base/fourccs.h>
#include <packager/media/base/media_handler.h>
#include <packager/media/base/media_handler_test_base.h>
#include <packager/media/base/media_sample.h>
#include <packager/media/base/muxer_options.h>
#include <packager/media/base/muxer_util.h>
#include <packager/media/base/stream_info.h>
#include <packager/media/formats/mp4/box_definitions.h>
#include <packager/status.h>
#include <packager/status/status_test_util.h>

namespace shaka {
namespace media {
namespace mp4 {
namespace {

const int32_t kTimeScale = 1000;
const int64_t kDuration = 100;
// A segment number that is neither 0 nor the internal counter's first value,
// so tests can tell which source the filename actually came from.
const int64_t kSegmentNumber = 42;
const bool kKeyFrame = true;

// Returns true if a file exists at |file_name| (and closes it again).
bool FileExists(const std::string& file_name) {
  std::unique_ptr<File, FileCloser> file(File::Open(file_name.c_str(), "r"));
  return file != nullptr;
}

}  // namespace

class LowLatencySegmentSegmenterTest : public MediaHandlerTestBase {
 protected:
  // The LL segmenter opens each segment file in append ("a") mode, which the
  // memory:// test file system does not support, so this needs real files on
  // disk (matching the local-path style other segmenter tests here use).
  MuxerOptions CreateMuxerOptions() const {
    MuxerOptions options;
    options.output_file_name = "ll_segmenter_unittest-init.mp4";
    options.segment_template = "ll_segmenter_unittest-segment-$Number$.m4s";
    return options;
  }

  void TearDown() override {
    File::Delete("ll_segmenter_unittest-init.mp4");
    File::Delete(
        GetSegmentName("ll_segmenter_unittest-segment-$Number$.m4s", 0, 0, 0)
            .c_str());
    File::Delete(GetSegmentName("ll_segmenter_unittest-segment-$Number$.m4s",
                                0, kSegmentNumber, 0)
                     .c_str());
  }

  // Builds and initializes a segmenter, writes a single chunk that also
  // completes its segment (the minimal flow that reaches
  // WriteInitialChunk's filename decision), with |segment_number| as the
  // real segment number for that chunk.
  Status WriteOneSegment(const MuxerOptions& options,
                        int64_t segment_number,
                        LowLatencySegmentSegmenter** out_segmenter) {
    std::unique_ptr<FileType> ftyp(new FileType);
    std::unique_ptr<Movie> moov(new Movie);
    moov->tracks.resize(1);
    moov->extends.tracks.resize(1);
    // The init segment is written as part of Initialize() below, and its
    // box serialization CHECK-fails on an incomplete moov, so this minimal
    // test moov still needs a valid video sample entry.
    SampleDescription& description =
        moov->tracks[0].media.information.sample_table.description;
    description.type = kVideo;
    VideoSampleEntry video_entry;
    video_entry.format = FOURCC_vp09;
    video_entry.width = 64;
    video_entry.height = 48;
    video_entry.codec_configuration.box_type = FOURCC_vpcC;
    video_entry.codec_configuration.data.push_back(0);
    description.video_entries.push_back(video_entry);

    std::unique_ptr<LowLatencySegmentSegmenter> segmenter(
        new LowLatencySegmentSegmenter(options, std::move(ftyp),
                                       std::move(moov)));

    std::shared_ptr<const StreamInfo> stream_info(
        GetVideoStreamInfo(kTimeScale));
    RETURN_IF_ERROR(segmenter->Initialize(
        {stream_info}, /*muxer_listener=*/nullptr,
        /*progress_listener=*/nullptr));

    std::shared_ptr<MediaSample> sample =
        GetMediaSample(0, kDuration, kKeyFrame);
    RETURN_IF_ERROR(segmenter->AddSample(0, *sample));

    SegmentInfo segment_info;
    segment_info.is_chunk = true;
    segment_info.is_subsegment = false;
    segment_info.segment_number = segment_number;
    // LowLatencySegmentSegmenter declares a private FinalizeSegment(int64_t)
    // that hides the public Segmenter::FinalizeSegment overload; qualify the
    // call to reach the one this test needs to drive.
    RETURN_IF_ERROR(segmenter->Segmenter::FinalizeSegment(0, segment_info));

    *out_segmenter = segmenter.release();
    return Status::OK;
  }
};

TEST_F(LowLatencySegmentSegmenterTest, DefaultFilenamesUnchanged) {
  MuxerOptions options = CreateMuxerOptions();
  // epoch_anchored_segment_numbers left at its default (false): this is the
  // regression guard for the trap where num_segments_ (0-based) must NOT be
  // swapped for segment_number (which starts at 1, or higher here).
  LowLatencySegmentSegmenter* segmenter = nullptr;
  ASSERT_OK(WriteOneSegment(options, kSegmentNumber, &segmenter));
  std::unique_ptr<LowLatencySegmentSegmenter> owned_segmenter(segmenter);

  // Today's (pre-epoch) behaviour: filenames come from the internal chunk
  // counter, which starts at 0, not from the real segment number.
  const std::string counter_based_name =
      GetSegmentName(options.segment_template, 0, 0, 0);
  EXPECT_TRUE(FileExists(counter_based_name));

  const std::string epoch_based_name =
      GetSegmentName(options.segment_template, 0, kSegmentNumber, 0);
  EXPECT_FALSE(FileExists(epoch_based_name));
}

TEST_F(LowLatencySegmentSegmenterTest, EpochAnchoredUsesRealSegmentNumber) {
  MuxerOptions options = CreateMuxerOptions();
  options.epoch_anchored_segment_numbers = true;
  LowLatencySegmentSegmenter* segmenter = nullptr;
  ASSERT_OK(WriteOneSegment(options, kSegmentNumber, &segmenter));
  std::unique_ptr<LowLatencySegmentSegmenter> owned_segmenter(segmenter);

  const std::string epoch_based_name =
      GetSegmentName(options.segment_template, 0, kSegmentNumber, 0);
  EXPECT_TRUE(FileExists(epoch_based_name));

  const std::string counter_based_name =
      GetSegmentName(options.segment_template, 0, 0, 0);
  EXPECT_FALSE(FileExists(counter_based_name));
}

}  // namespace mp4
}  // namespace media
}  // namespace shaka

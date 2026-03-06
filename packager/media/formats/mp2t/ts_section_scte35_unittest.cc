// Copyright 2024 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/formats/mp2t/ts_section_scte35.h>

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <packager/media/base/bit_reader.h>
#include <packager/media/base/media_handler.h>

namespace shaka {
namespace media {
namespace mp2t {

class TsSectionScte35Test : public ::testing::Test {
 protected:
  TsSectionScte35Test() {}

  void SetUp() override {
    events_.clear();
  }

  TsSectionScte35::Scte35EventCB MakeCallback() {
    return [this](std::shared_ptr<Scte35Event> event) {
      events_.push_back(std::move(event));
    };
  }

  std::vector<std::shared_ptr<Scte35Event>> events_;
};

// Constructs a minimal splice_insert (immediate splice out) SCTE-35 section.
//
// splice_insert:
//   splice_event_id: 1
//   cancel: 0
//   out_of_network: 1
//   program_splice: 1
//   duration_flag: 1
//   splice_immediate: 0
//   pts_time: 2700000 (30 seconds at 90kHz)
//   break_duration: 2700000 (30 seconds)
//   unique_program_id: 1
//   avail_num: 0
//   avails_expected: 0
TEST_F(TsSectionScte35Test, SpliceInsertCueOut) {
  TsSectionScte35 parser(MakeCallback());

  // Manually constructed splice_info_section with splice_insert.
  // section_length = 1(proto) + 5(enc/pts_adj) + 1(cw) + 3(tier/cmd_len)
  //                + 1(cmd_type) + 20(splice_insert) + 2(desc_len) + 4(CRC)
  //                = 37 = 0x25
  const uint8_t kSpliceInsert[] = {
      0xFC,                          // table_id
      0x30, 0x25,                    // section_syntax=0, section_length=37
      0x00,                          // protocol_version=0
      0x00, 0x00, 0x00, 0x00, 0x00,  // encrypted=0, pts_adjustment=0
      0xFF,                          // cw_index
      0xFF, 0xF0, 0x14,              // tier=0xFFF, splice_command_length=20
      0x05,                          // splice_command_type=splice_insert
      // splice_insert body (20 bytes):
      0x00, 0x00, 0x00, 0x01,        // splice_event_id=1
      0x7F,                          // cancel=0 | reserved
      0xEF,                          // out_of_network=1, program=1,
                                     // duration=1, immediate=0, reserved
      // splice_time: time_specified=1, reserved, pts_time=2700000
      0xFE,                          // time_specified=1 | reserved=0x3F |
                                     // pts_bit32=0
      0x00, 0x29, 0x32, 0xE0,        // pts_time bits 31-0
      // break_duration: auto_return=1, duration=2700000
      0xFE,                          // auto_return=1 | reserved | dur_bit32=0
      0x00, 0x29, 0x32, 0xE0,        // duration bits 31-0
      0x00, 0x01,                    // unique_program_id=1
      0x00,                          // avail_num=0
      0x00,                          // avails_expected=0
      // Descriptor loop
      0x00, 0x00,                    // descriptor_loop_length=0
      // CRC (not validated in ParsePsiSection)
      0xDE, 0xAD, 0xBE, 0xEF,
  };

  BitReader bit_reader(kSpliceInsert, sizeof(kSpliceInsert));
  EXPECT_TRUE(parser.ParsePsiSection(&bit_reader));

  ASSERT_EQ(1u, events_.size());
  const auto& event = events_[0];
  EXPECT_EQ("1", event->id);
  // out_of_network=1 maps to type 0x34 (Provider Placement Opportunity Start)
  EXPECT_EQ(0x34, event->type);
  // PTS 2700000 / 90000 = 30.0 seconds
  EXPECT_DOUBLE_EQ(30.0, event->start_time_in_seconds);
  // Duration 2700000 / 90000 = 30.0 seconds
  EXPECT_DOUBLE_EQ(30.0, event->duration_in_seconds);
  // cue_data should contain the raw section bytes.
  EXPECT_EQ(sizeof(kSpliceInsert), event->cue_data.size());
}

// splice_insert with out_of_network=0 (cue in / ad end).
TEST_F(TsSectionScte35Test, SpliceInsertCueIn) {
  TsSectionScte35 parser(MakeCallback());

  // Same as above but out_of_network=0, no duration, splice_immediate=1.
  // splice_insert body:
  //   event_id=2, cancel=0, out_of_network=0, program=1,
  //   duration=0, immediate=1
  //   unique_program_id=1, avail_num=0, avails_expected=0
  // splice_insert size = 4+1+1+2+1+1 = 10 bytes
  // section_length = 1+5+1+3+1+10+2+4 = 27 = 0x1B
  const uint8_t kSpliceInsertCueIn[] = {
      0xFC,                          // table_id
      0x30, 0x1B,                    // section_length=27
      0x00,                          // protocol_version=0
      0x00, 0x00, 0x00, 0x00, 0x00,  // encrypted=0, pts_adjustment=0
      0xFF,                          // cw_index
      0xFF, 0xF0, 0x0A,              // tier=0xFFF, splice_command_length=10
      0x05,                          // splice_command_type=splice_insert
      // splice_insert body (10 bytes):
      0x00, 0x00, 0x00, 0x02,        // splice_event_id=2
      0x7F,                          // cancel=0 | reserved
      0x5F,                          // out_of_network=0, program=1,
                                     // duration=0, immediate=1, reserved=0xF
      // No splice_time (immediate=1), no break_duration (duration_flag=0)
      0x00, 0x01,                    // unique_program_id=1
      0x00,                          // avail_num
      0x00,                          // avails_expected
      // Descriptor loop
      0x00, 0x00,                    // descriptor_loop_length=0
      // CRC
      0x00, 0x00, 0x00, 0x00,
  };

  BitReader bit_reader(kSpliceInsertCueIn, sizeof(kSpliceInsertCueIn));
  EXPECT_TRUE(parser.ParsePsiSection(&bit_reader));

  ASSERT_EQ(1u, events_.size());
  const auto& event = events_[0];
  EXPECT_EQ("2", event->id);
  // out_of_network=0 maps to type 0x35 (Provider Placement Opportunity End)
  EXPECT_EQ(0x35, event->type);
  EXPECT_DOUBLE_EQ(0.0, event->start_time_in_seconds);
  EXPECT_DOUBLE_EQ(0.0, event->duration_in_seconds);
}

// splice_null (heartbeat) should not emit any events.
TEST_F(TsSectionScte35Test, SpliceNullIgnored) {
  TsSectionScte35 parser(MakeCallback());

  // Minimal splice_null section.
  // splice_command_length=0, splice_command_type=0x00
  // section_length = 1+5+1+3+1+0+2+4 = 17 = 0x11
  const uint8_t kSpliceNull[] = {
      0xFC,                          // table_id
      0x30, 0x11,                    // section_length=17
      0x00,                          // protocol_version=0
      0x00, 0x00, 0x00, 0x00, 0x00,  // encrypted=0, pts_adjustment=0
      0xFF,                          // cw_index
      0xFF, 0xF0, 0x00,              // tier=0xFFF, splice_command_length=0
      0x00,                          // splice_command_type=splice_null
      // Descriptor loop
      0x00, 0x00,                    // descriptor_loop_length=0
      // CRC
      0x00, 0x00, 0x00, 0x00,
  };

  BitReader bit_reader(kSpliceNull, sizeof(kSpliceNull));
  EXPECT_TRUE(parser.ParsePsiSection(&bit_reader));

  // splice_null should not produce any events.
  EXPECT_EQ(0u, events_.size());
}

// time_signal with a segmentation descriptor.
TEST_F(TsSectionScte35Test, TimeSignalWithSegmentationDescriptor) {
  TsSectionScte35 parser(MakeCallback());

  // time_signal with splice_time PTS=2700000 (30s)
  // Followed by segmentation_descriptor:
  //   segmentation_event_id=100
  //   segmentation_type_id=0x30 (Provider Ad Start)
  //   segmentation_duration=2700000 (30s)
  //
  // time_signal body = 5 bytes (splice_time)
  // segmentation_descriptor:
  //   tag(1) + length(1) + identifier(4) + event_id(4) + cancel_byte(1)
  //   + flags_byte(1) + duration(5) + upid_type(1) + upid_len(1)
  //   + seg_type(1) + seg_num(1) + segs_expected(1) = 22 bytes total
  //
  // section_length = 1+5+1+3+1+5+2+22+4 = 44 = 0x2C
  const uint8_t kTimeSignal[] = {
      0xFC,                          // table_id
      0x30, 0x2C,                    // section_length=44
      0x00,                          // protocol_version=0
      0x00, 0x00, 0x00, 0x00, 0x00,  // encrypted=0, pts_adjustment=0
      0xFF,                          // cw_index
      0xFF, 0xF0, 0x05,              // tier=0xFFF, splice_command_length=5
      0x06,                          // splice_command_type=time_signal
      // time_signal body (5 bytes): splice_time
      0xFE,                          // time_specified=1 | reserved | pts_b32=0
      0x00, 0x29, 0x32, 0xE0,        // pts_time=2700000
      // Descriptor loop
      0x00, 0x16,                    // descriptor_loop_length=22
      // Segmentation descriptor (22 bytes):
      0x02,                          // splice_descriptor_tag=0x02
      0x14,                          // descriptor_length=20
      0x43, 0x55, 0x45, 0x49,        // identifier="CUEI"
      0x00, 0x00, 0x00, 0x64,        // segmentation_event_id=100
      0x7F,                          // cancel=0 | reserved
      0xFF,                          // program_seg=1, dur_flag=1,
                                     // delivery_not_restricted=1, reserved
      // segmentation_duration (40 bits) = 2700000
      0x00, 0x00, 0x29, 0x32, 0xE0,
      0x00,                          // segmentation_upid_type=0
      0x00,                          // segmentation_upid_length=0
      0x30,                          // segmentation_type_id=0x30
      0x00,                          // segment_num=0
      0x00,                          // segments_expected=0
      // CRC
      0x00, 0x00, 0x00, 0x00,
  };

  BitReader bit_reader(kTimeSignal, sizeof(kTimeSignal));
  EXPECT_TRUE(parser.ParsePsiSection(&bit_reader));

  ASSERT_EQ(1u, events_.size());
  const auto& event = events_[0];
  EXPECT_EQ("100", event->id);
  // segmentation_type_id=0x30 (Provider Ad Start)
  EXPECT_EQ(0x30, event->type);
  EXPECT_DOUBLE_EQ(30.0, event->start_time_in_seconds);
  EXPECT_DOUBLE_EQ(30.0, event->duration_in_seconds);
  EXPECT_EQ(sizeof(kTimeSignal), event->cue_data.size());
}

// Test pts_adjustment is correctly applied.
TEST_F(TsSectionScte35Test, PtsAdjustment) {
  TsSectionScte35 parser(MakeCallback());

  // splice_insert with pts_time=0, but pts_adjustment=900000 (10 seconds).
  // Effective splice time = 0 + 900000 = 900000 / 90000 = 10.0s
  //
  // pts_adjustment = 900000 = 0x0DBBA0
  // As 33 bits: 0x000DBBA0
  // encrypted(0)|encryption(000000)|pts_adj_bit32(0) = 0x00
  // pts_adj bits 31-24 = 0x00
  // pts_adj bits 23-16 = 0x0D
  // pts_adj bits 15-8  = 0xBB
  // pts_adj bits 7-0   = 0xA0
  //
  // section_length = 1+5+1+3+1+20+2+4 = 37 = 0x25
  const uint8_t kSpliceWithAdj[] = {
      0xFC,                          // table_id
      0x30, 0x25,                    // section_length=37
      0x00,                          // protocol_version=0
      // encrypted=0, pts_adjustment=900000
      0x00,                          // encrypted=0 | enc_algo=0 | pts_b32=0
      0x00, 0x0D, 0xBB, 0xA0,        // pts_adjustment bits 31-0
      0xFF,                          // cw_index
      0xFF, 0xF0, 0x14,              // tier=0xFFF, splice_command_length=20
      0x05,                          // splice_command_type=splice_insert
      // splice_insert body (20 bytes):
      0x00, 0x00, 0x00, 0x03,        // splice_event_id=3
      0x7F,                          // cancel=0 | reserved
      0xEF,                          // out_of_network=1, program=1,
                                     // duration=1, immediate=0, reserved
      // splice_time: time_specified=1, pts_time=0
      0xFE,                          // time_specified=1 | reserved | pts_b32=0
      0x00, 0x00, 0x00, 0x00,        // pts_time=0
      // break_duration: auto_return=1, duration=900000
      0xFE,                          // auto_return=1 | reserved | dur_b32=0
      0x00, 0x0D, 0xBB, 0xA0,        // duration=900000
      0x00, 0x01,                    // unique_program_id=1
      0x00, 0x00,                    // avail_num, avails_expected
      // Descriptor loop
      0x00, 0x00,                    // descriptor_loop_length=0
      // CRC
      0x00, 0x00, 0x00, 0x00,
  };

  BitReader bit_reader(kSpliceWithAdj, sizeof(kSpliceWithAdj));
  EXPECT_TRUE(parser.ParsePsiSection(&bit_reader));

  ASSERT_EQ(1u, events_.size());
  const auto& event = events_[0];
  EXPECT_EQ("3", event->id);
  // pts_time(0) + pts_adjustment(900000) = 900000 / 90000 = 10.0s
  EXPECT_DOUBLE_EQ(10.0, event->start_time_in_seconds);
  // Duration 900000 / 90000 = 10.0s
  EXPECT_DOUBLE_EQ(10.0, event->duration_in_seconds);
}

// Encrypted section should be silently skipped.
TEST_F(TsSectionScte35Test, EncryptedSectionSkipped) {
  TsSectionScte35 parser(MakeCallback());

  // section_length = 1+5+1+3+1+0+2+4 = 17 = 0x11
  const uint8_t kEncrypted[] = {
      0xFC,                          // table_id
      0x30, 0x11,                    // section_length=17
      0x00,                          // protocol_version=0
      // encrypted=1, encryption_algorithm=1, pts_adjustment=0
      0x82,                          // encrypted=1 | enc_algo=000001 | p=0
      0x00, 0x00, 0x00, 0x00,        // pts_adjustment=0
      0xFF,                          // cw_index
      0xFF, 0xF0, 0x00,              // tier=0xFFF, splice_command_length=0
      0x05,                          // splice_command_type
      0x00, 0x00,                    // descriptor_loop_length=0
      0x00, 0x00, 0x00, 0x00,        // CRC
  };

  BitReader bit_reader(kEncrypted, sizeof(kEncrypted));
  EXPECT_TRUE(parser.ParsePsiSection(&bit_reader));

  // Encrypted section should not produce any events.
  EXPECT_EQ(0u, events_.size());
}

// splice_insert with cancel indicator should not emit an event with timing.
TEST_F(TsSectionScte35Test, SpliceInsertCancelled) {
  TsSectionScte35 parser(MakeCallback());

  // splice_insert with cancel=1
  // splice_insert body: event_id(4) + cancel_byte(1) = 5 bytes
  // section_length = 1+5+1+3+1+5+2+4 = 22 = 0x16
  const uint8_t kCancelled[] = {
      0xFC,                          // table_id
      0x30, 0x16,                    // section_length=22
      0x00,                          // protocol_version=0
      0x00, 0x00, 0x00, 0x00, 0x00,  // encrypted=0, pts_adjustment=0
      0xFF,                          // cw_index
      0xFF, 0xF0, 0x05,              // tier=0xFFF, splice_command_length=5
      0x05,                          // splice_command_type=splice_insert
      // splice_insert body (5 bytes):
      0x00, 0x00, 0x00, 0x04,        // splice_event_id=4
      0xFF,                          // cancel=1 | reserved
      // Descriptor loop
      0x00, 0x00,                    // descriptor_loop_length=0
      // CRC
      0x00, 0x00, 0x00, 0x00,
  };

  BitReader bit_reader(kCancelled, sizeof(kCancelled));
  EXPECT_TRUE(parser.ParsePsiSection(&bit_reader));

  // Cancelled splice_insert should still emit (with id set, but no timing).
  ASSERT_EQ(1u, events_.size());
  EXPECT_EQ("4", events_[0]->id);
  EXPECT_DOUBLE_EQ(0.0, events_[0]->start_time_in_seconds);
  EXPECT_DOUBLE_EQ(0.0, events_[0]->duration_in_seconds);
}

// Raw cue_data should preserve the full section binary.
TEST_F(TsSectionScte35Test, CueDataPreservesRawBinary) {
  TsSectionScte35 parser(MakeCallback());

  // Reuse the simple cue-in test case.
  const uint8_t kSection[] = {
      0xFC,
      0x30, 0x1B,
      0x00,
      0x00, 0x00, 0x00, 0x00, 0x00,
      0xFF,
      0xFF, 0xF0, 0x0A,
      0x05,
      0x00, 0x00, 0x00, 0x05,
      0x7F,
      0x5F,
      0x00, 0x01,
      0x00, 0x00,
      0x00, 0x00,
      0xCA, 0xFE, 0xBA, 0xBE,        // Distinct CRC pattern
  };

  BitReader bit_reader(kSection, sizeof(kSection));
  EXPECT_TRUE(parser.ParsePsiSection(&bit_reader));

  ASSERT_EQ(1u, events_.size());
  // Verify the cue_data contains the exact raw bytes.
  ASSERT_EQ(sizeof(kSection), events_[0]->cue_data.size());
  EXPECT_EQ(0, memcmp(kSection, events_[0]->cue_data.data(),
                       sizeof(kSection)));
}

}  // namespace mp2t
}  // namespace media
}  // namespace shaka

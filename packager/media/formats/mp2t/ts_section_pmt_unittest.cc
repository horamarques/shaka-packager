// Copyright 2024 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/formats/mp2t/ts_section_pmt.h>

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <packager/media/base/bit_reader.h>
#include <packager/media/formats/mp2t/ts_audio_type.h>
#include <packager/media/formats/mp2t/ts_stream_type.h>

namespace shaka {
namespace media {
namespace mp2t {

namespace {

// Stores information received by the RegisterPesCb callback.
struct RegisteredPes {
  int pid;
  TsStreamType stream_type;
  uint32_t max_bitrate;
  std::string lang;
  TsAudioType audio_type;
  bool called;
};

}  // namespace

class TsSectionPmtTest : public ::testing::Test {
 protected:
  TsSectionPmtTest() : registered_() {}

  void SetUp() override {
    registered_.clear();
  }

  TsSectionPmt::RegisterPesCb MakeCallback() {
    return [this](int pid, TsStreamType stream_type, uint32_t max_bitrate,
                  const std::string& lang, TsAudioType audio_type,
                  const uint8_t* /* descriptor */,
                  size_t /* descriptor_length */) {
      RegisteredPes info;
      info.pid = pid;
      info.stream_type = stream_type;
      info.max_bitrate = max_bitrate;
      info.lang = lang;
      info.audio_type = audio_type;
      info.called = true;
      registered_.push_back(info);
    };
  }

  std::vector<RegisteredPes> registered_;
};

// Test: PMT with CUEI registration descriptor at program level and
// stream_type 0x86 should be detected as SCTE-35.
TEST_F(TsSectionPmtTest, Scte35CueiAtProgramLevel) {
  TsSectionPmt pmt(MakeCallback());

  // PMT section:
  //   program_info: registration descriptor with "CUEI"
  //   ES entry: stream_type=0x86 (normally kDtsHd), PID=0x61
  // section_length = 5 (fixed header) + 4 (PCR+prog_info_len) + 6 (CUEI desc)
  //                + 5 (ES entry) + 4 (CRC) = 24 = 0x18
  const uint8_t kPmtData[] = {
      0x02,                          // table_id
      0xB0, 0x18,                    // section_syntax_indicator=1, length=24
      0x00, 0x01,                    // program_number=1
      0xC1,                          // version=0, current_next=1
      0x00,                          // section_number
      0x00,                          // last_section_number
      0xE0, 0x50,                    // reserved + PCR_PID=0x50
      0xF0, 0x06,                    // reserved + program_info_length=6
      // Registration descriptor: tag=0x05, length=4, "CUEI"
      0x05, 0x04, 0x43, 0x55, 0x45, 0x49,
      // ES entry: stream_type=0x86, PID=0x61, ES_info_length=0
      0x86, 0xE0, 0x61, 0xF0, 0x00,
      // CRC32 (not validated by parser)
      0x00, 0x00, 0x00, 0x00,
  };

  BitReader bit_reader(kPmtData, sizeof(kPmtData));
  EXPECT_TRUE(pmt.ParsePsiSection(&bit_reader));

  ASSERT_EQ(1u, registered_.size());
  EXPECT_EQ(0x61, registered_[0].pid);
  EXPECT_EQ(TsStreamType::kScte35, registered_[0].stream_type);
}

// Test: PMT with stream_type 0x86 but NO CUEI descriptor should remain
// as kDtsHd (not converted to kScte35).
TEST_F(TsSectionPmtTest, StreamType0x86WithoutCueiRemainsDtsHd) {
  TsSectionPmt pmt(MakeCallback());

  // PMT section:
  //   program_info: empty (no descriptors)
  //   ES entry: stream_type=0x86, PID=0x61
  // section_length = 5 + 4 + 0 + 5 + 4 = 18 = 0x12
  const uint8_t kPmtData[] = {
      0x02,                          // table_id
      0xB0, 0x12,                    // section_syntax_indicator=1, length=18
      0x00, 0x01,                    // program_number=1
      0xC1,                          // version=0, current_next=1
      0x00,                          // section_number
      0x00,                          // last_section_number
      0xE0, 0x50,                    // reserved + PCR_PID=0x50
      0xF0, 0x00,                    // reserved + program_info_length=0
      // ES entry: stream_type=0x86, PID=0x61, ES_info_length=0
      0x86, 0xE0, 0x61, 0xF0, 0x00,
      // CRC32
      0x00, 0x00, 0x00, 0x00,
  };

  BitReader bit_reader(kPmtData, sizeof(kPmtData));
  EXPECT_TRUE(pmt.ParsePsiSection(&bit_reader));

  ASSERT_EQ(1u, registered_.size());
  EXPECT_EQ(0x61, registered_[0].pid);
  EXPECT_EQ(TsStreamType::kDtsHd, registered_[0].stream_type);
}

// Test: PMT with CUEI registration descriptor at ES level (not program level)
// should also detect SCTE-35.
TEST_F(TsSectionPmtTest, Scte35CueiAtEsLevel) {
  TsSectionPmt pmt(MakeCallback());

  // PMT section:
  //   program_info: empty
  //   ES entry: stream_type=0x86, PID=0x61, ES descriptors with CUEI
  // section_length = 5 + 4 + 0 + 11 (ES entry with 6-byte descriptor) + 4 = 24
  const uint8_t kPmtData[] = {
      0x02,                          // table_id
      0xB0, 0x18,                    // section_syntax_indicator=1, length=24
      0x00, 0x01,                    // program_number=1
      0xC1,                          // version=0, current_next=1
      0x00,                          // section_number
      0x00,                          // last_section_number
      0xE0, 0x50,                    // reserved + PCR_PID=0x50
      0xF0, 0x00,                    // reserved + program_info_length=0
      // ES entry: stream_type=0x86, PID=0x61, ES_info_length=6
      0x86, 0xE0, 0x61, 0xF0, 0x06,
      // Registration descriptor at ES level: tag=0x05, length=4, "CUEI"
      0x05, 0x04, 0x43, 0x55, 0x45, 0x49,
      // CRC32
      0x00, 0x00, 0x00, 0x00,
  };

  BitReader bit_reader(kPmtData, sizeof(kPmtData));
  EXPECT_TRUE(pmt.ParsePsiSection(&bit_reader));

  ASSERT_EQ(1u, registered_.size());
  EXPECT_EQ(0x61, registered_[0].pid);
  EXPECT_EQ(TsStreamType::kScte35, registered_[0].stream_type);
}

// Test: PMT with CUEI at program level and multiple ES entries.
// Only stream_type 0x86 should be converted; other streams remain unchanged.
TEST_F(TsSectionPmtTest, Scte35CueiOnlyAffects0x86Streams) {
  TsSectionPmt pmt(MakeCallback());

  // PMT section:
  //   program_info: CUEI registration descriptor (6 bytes)
  //   ES entry 1: stream_type=0x1B (H.264), PID=0x50
  //   ES entry 2: stream_type=0x86, PID=0x61
  // section_length = 5 + 4 + 6 + 5 + 5 + 4 = 29 = 0x1D
  const uint8_t kPmtData[] = {
      0x02,                          // table_id
      0xB0, 0x1D,                    // section_syntax_indicator=1, length=29
      0x00, 0x01,                    // program_number=1
      0xC1,                          // version=0, current_next=1
      0x00,                          // section_number
      0x00,                          // last_section_number
      0xE0, 0x50,                    // reserved + PCR_PID=0x50
      0xF0, 0x06,                    // reserved + program_info_length=6
      // Registration descriptor: tag=0x05, length=4, "CUEI"
      0x05, 0x04, 0x43, 0x55, 0x45, 0x49,
      // ES entry 1: H.264 video, PID=0x50, ES_info_length=0
      0x1B, 0xE0, 0x50, 0xF0, 0x00,
      // ES entry 2: stream_type=0x86, PID=0x61, ES_info_length=0
      0x86, 0xE0, 0x61, 0xF0, 0x00,
      // CRC32
      0x00, 0x00, 0x00, 0x00,
  };

  BitReader bit_reader(kPmtData, sizeof(kPmtData));
  EXPECT_TRUE(pmt.ParsePsiSection(&bit_reader));

  ASSERT_EQ(2u, registered_.size());
  // First ES: H.264 should remain unchanged.
  EXPECT_EQ(0x50, registered_[0].pid);
  EXPECT_EQ(TsStreamType::kAvc, registered_[0].stream_type);
  // Second ES: 0x86 should be converted to kScte35.
  EXPECT_EQ(0x61, registered_[1].pid);
  EXPECT_EQ(TsStreamType::kScte35, registered_[1].stream_type);
}

// Test: PMT with a non-CUEI registration descriptor at program level.
// stream_type 0x86 should NOT be converted.
TEST_F(TsSectionPmtTest, NonCueiRegistrationDescriptorIgnored) {
  TsSectionPmt pmt(MakeCallback());

  // PMT section:
  //   program_info: registration descriptor with "HDMV" (not CUEI)
  //   ES entry: stream_type=0x86, PID=0x61
  // section_length = 5 + 4 + 6 + 5 + 4 = 24 = 0x18
  const uint8_t kPmtData[] = {
      0x02,                          // table_id
      0xB0, 0x18,                    // section_syntax_indicator=1, length=24
      0x00, 0x01,                    // program_number=1
      0xC1,                          // version=0, current_next=1
      0x00,                          // section_number
      0x00,                          // last_section_number
      0xE0, 0x50,                    // reserved + PCR_PID=0x50
      0xF0, 0x06,                    // reserved + program_info_length=6
      // Registration descriptor: tag=0x05, length=4, "HDMV"
      0x05, 0x04, 0x48, 0x44, 0x4D, 0x56,
      // ES entry: stream_type=0x86, PID=0x61, ES_info_length=0
      0x86, 0xE0, 0x61, 0xF0, 0x00,
      // CRC32
      0x00, 0x00, 0x00, 0x00,
  };

  BitReader bit_reader(kPmtData, sizeof(kPmtData));
  EXPECT_TRUE(pmt.ParsePsiSection(&bit_reader));

  ASSERT_EQ(1u, registered_.size());
  EXPECT_EQ(0x61, registered_[0].pid);
  // Should remain as DTS-HD since the registration descriptor is not CUEI.
  EXPECT_EQ(TsStreamType::kDtsHd, registered_[0].stream_type);
}

// Test: Normal PMT with H.264 video and AAC audio (no SCTE-35).
// Verifies that existing functionality is not broken.
TEST_F(TsSectionPmtTest, NormalPmtWithVideoAndAudio) {
  TsSectionPmt pmt(MakeCallback());

  // PMT section:
  //   program_info: empty
  //   ES entry 1: H.264 video, PID=0x50
  //   ES entry 2: AAC audio, PID=0x51
  // section_length = 5 + 4 + 0 + 5 + 5 + 4 = 23 = 0x17
  const uint8_t kPmtData[] = {
      0x02,                          // table_id
      0xB0, 0x17,                    // section_syntax_indicator=1, length=23
      0x00, 0x01,                    // program_number=1
      0xC1,                          // version=0, current_next=1
      0x00,                          // section_number
      0x00,                          // last_section_number
      0xE0, 0x50,                    // reserved + PCR_PID=0x50
      0xF0, 0x00,                    // reserved + program_info_length=0
      // ES entry 1: H.264, PID=0x50, ES_info_length=0
      0x1B, 0xE0, 0x50, 0xF0, 0x00,
      // ES entry 2: AAC, PID=0x51, ES_info_length=0
      0x0F, 0xE0, 0x51, 0xF0, 0x00,
      // CRC32
      0x00, 0x00, 0x00, 0x00,
  };

  BitReader bit_reader(kPmtData, sizeof(kPmtData));
  EXPECT_TRUE(pmt.ParsePsiSection(&bit_reader));

  ASSERT_EQ(2u, registered_.size());
  EXPECT_EQ(0x50, registered_[0].pid);
  EXPECT_EQ(TsStreamType::kAvc, registered_[0].stream_type);
  EXPECT_EQ(0x51, registered_[1].pid);
  EXPECT_EQ(TsStreamType::kAdtsAac, registered_[1].stream_type);
}

}  // namespace mp2t
}  // namespace media
}  // namespace shaka

// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/webapi/service/event_spec.h>

#include <gtest/gtest.h>

namespace shaka {
namespace webapi {
namespace {

EventCreateRequest ValidLiveRequest() {
  EventCreateRequest request;
  request.event_id = "sport1";
  StreamSpec video;
  video.input = "udp://239.1.1.1:5000";
  video.stream = "video";
  video.init_segment = "/out/video_init.mp4";
  video.segment_template = "/out/video_$Number$.m4s";
  request.streams.push_back(video);
  request.mpd_output = "/out/output.mpd";
  request.segment_duration = 2;
  request.time_shift_buffer_depth = 30;
  return request;
}

TEST(ValidateEventRequestTest, AcceptsValidAndNamesBrokenField) {
  EXPECT_EQ("", ValidateEventRequest(ValidLiveRequest()));

  EventCreateRequest no_streams = ValidLiveRequest();
  no_streams.streams.clear();
  EXPECT_NE(std::string::npos,
            ValidateEventRequest(no_streams).find("streams"));

  EventCreateRequest no_output = ValidLiveRequest();
  no_output.mpd_output.clear();
  EXPECT_NE(std::string::npos,
            ValidateEventRequest(no_output).find("output"));

  EventCreateRequest bad_duration = ValidLiveRequest();
  bad_duration.segment_duration = 0;
  EXPECT_NE(std::string::npos,
            ValidateEventRequest(bad_duration).find("segment_duration"));

  EventCreateRequest bad_key = ValidLiveRequest();
  EncryptionSpec enc;
  enc.scheme = "cenc";
  enc.keys.push_back({"", "notahexkey", "00112233445566778899aabbccddeeff"});
  bad_key.encryption = enc;
  EXPECT_NE(std::string::npos, ValidateEventRequest(bad_key).find("key_id"));

  EventCreateRequest bad_scheme = ValidLiveRequest();
  EncryptionSpec enc2;
  enc2.scheme = "aes-9000";
  enc2.keys.push_back({"", "00112233445566778899aabbccddeeff",
                       "00112233445566778899aabbccddeeff"});
  bad_scheme.encryption = enc2;
  EXPECT_NE(std::string::npos,
            ValidateEventRequest(bad_scheme).find("scheme"));
}

TEST(BuildEventArgvTest, GoldenLiveDashWithEncryption) {
  EventCreateRequest request = ValidLiveRequest();
  EncryptionSpec enc;
  enc.scheme = "cbcs";
  enc.keys.push_back({"", "11111111111111111111111111111111",
                      "22222222222222222222222222222222"});
  enc.iv = "33333333333333333333333333333333";
  enc.clear_lead = 0;
  request.encryption = enc;
  request.extra_args = {"--dump_stream_info"};

  const std::vector<std::string> argv = BuildEventArgv(request);
  const std::vector<std::string> expected = {
      "input=udp://239.1.1.1:5000,stream=video,"
      "init_segment=/out/video_init.mp4,"
      "segment_template=/out/video_$Number$.m4s",
      "--mpd_output", "/out/output.mpd",
      "--segment_duration", "2",
      "--time_shift_buffer_depth", "30",
      "--enable_raw_key_encryption",
      "--protection_scheme", "cbcs",
      "--keys",
      "label=:key_id=11111111111111111111111111111111:"
      "key=22222222222222222222222222222222",
      "--iv", "33333333333333333333333333333333",
      "--clear_lead", "0",
      "--dump_stream_info",
  };
  EXPECT_EQ(expected, argv);
}

TEST(BuildEventArgvTest, HlsOnlyOmitsUnsetFlags) {
  EventCreateRequest request = ValidLiveRequest();
  request.mpd_output.clear();
  request.hls_master_playlist_output = "/out/master.m3u8";
  request.hls_playlist_type = "LIVE";
  request.time_shift_buffer_depth = 0;

  const std::vector<std::string> argv = BuildEventArgv(request);
  const std::vector<std::string> expected = {
      "input=udp://239.1.1.1:5000,stream=video,"
      "init_segment=/out/video_init.mp4,"
      "segment_template=/out/video_$Number$.m4s",
      "--hls_master_playlist_output", "/out/master.m3u8",
      "--hls_playlist_type", "LIVE",
      "--segment_duration", "2",
  };
  EXPECT_EQ(expected, argv);
}

TEST(GenerateEventIdTest, ShapeAndUniqueness) {
  const std::string a = GenerateEventId();
  const std::string b = GenerateEventId();
  EXPECT_EQ(32u, a.size());
  EXPECT_NE(a, b);
  for (char c : a)
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
}

}  // namespace
}  // namespace webapi
}  // namespace shaka

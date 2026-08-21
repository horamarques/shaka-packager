// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/webapi/service/event_spec.h>

#include <random>

#include <absl/strings/str_cat.h>

namespace shaka {
namespace webapi {
namespace {

bool IsHex(const std::string& value, size_t length) {
  if (value.size() != length)
    return false;
  for (char c : value) {
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F');
    if (!hex)
      return false;
  }
  return true;
}

}  // namespace

std::string ValidateEventRequest(const EventCreateRequest& request) {
  if (request.streams.empty())
    return "streams: at least one stream is required";
  for (size_t i = 0; i < request.streams.size(); ++i) {
    const StreamSpec& stream = request.streams[i];
    if (stream.input.empty())
      return absl::StrCat("streams[", i, "].input: required");
    if (stream.stream.empty())
      return absl::StrCat("streams[", i, "].stream: required");
    if (stream.segment_template.empty() && stream.output.empty())
      return absl::StrCat("streams[", i,
                          "].segment_template: required (or output)");
  }
  if (request.mpd_output.empty() && request.hls_master_playlist_output.empty())
    return "output: mpd_output or hls_master_playlist_output is required";
  if (!request.hls_master_playlist_output.empty() &&
      request.hls_playlist_type.empty())
    return "hls_playlist_type: required with hls_master_playlist_output";
  if (request.segment_duration <= 0)
    return "segment_duration: must be positive";
  if (request.time_shift_buffer_depth < 0)
    return "time_shift_buffer_depth: must be >= 0";
  if (request.stop_timeout_seconds <= 0)
    return "stop_timeout_seconds: must be positive";
  if (request.encryption.has_value()) {
    const EncryptionSpec& enc = request.encryption.value();
    if (enc.scheme != "cenc" && enc.scheme != "cbcs")
      return "encryption.scheme: must be cenc or cbcs";
    if (enc.keys.empty())
      return "encryption.keys: at least one key is required";
    for (size_t i = 0; i < enc.keys.size(); ++i) {
      if (!IsHex(enc.keys[i].key_id, 32))
        return absl::StrCat("encryption.keys[", i,
                            "].key_id: must be 32 hex chars");
      if (!IsHex(enc.keys[i].key, 32))
        return absl::StrCat("encryption.keys[", i,
                            "].key: must be 32 hex chars");
    }
    if (!enc.iv.empty() && !IsHex(enc.iv, 16) && !IsHex(enc.iv, 32))
      return "encryption.iv: must be 16 or 32 hex chars";
  }
  return "";
}

std::vector<std::string> BuildEventArgv(const EventCreateRequest& request) {
  std::vector<std::string> argv;
  for (const StreamSpec& stream : request.streams) {
    std::string descriptor =
        absl::StrCat("input=", stream.input, ",stream=", stream.stream);
    if (!stream.init_segment.empty())
      absl::StrAppend(&descriptor, ",init_segment=", stream.init_segment);
    if (!stream.segment_template.empty())
      absl::StrAppend(&descriptor,
                      ",segment_template=", stream.segment_template);
    if (!stream.output.empty())
      absl::StrAppend(&descriptor, ",output=", stream.output);
    argv.push_back(descriptor);
  }
  if (!request.mpd_output.empty()) {
    argv.push_back("--mpd_output");
    argv.push_back(request.mpd_output);
  }
  if (!request.hls_master_playlist_output.empty()) {
    argv.push_back("--hls_master_playlist_output");
    argv.push_back(request.hls_master_playlist_output);
    argv.push_back("--hls_playlist_type");
    argv.push_back(request.hls_playlist_type);
  }
  argv.push_back("--segment_duration");
  argv.push_back(absl::StrCat(request.segment_duration));
  if (request.time_shift_buffer_depth > 0) {
    argv.push_back("--time_shift_buffer_depth");
    argv.push_back(absl::StrCat(request.time_shift_buffer_depth));
  }
  if (request.encryption.has_value()) {
    const EncryptionSpec& enc = request.encryption.value();
    argv.push_back("--enable_raw_key_encryption");
    argv.push_back("--protection_scheme");
    argv.push_back(enc.scheme);
    std::string keys;
    for (size_t i = 0; i < enc.keys.size(); ++i) {
      if (i > 0)
        absl::StrAppend(&keys, ",");
      absl::StrAppend(&keys, "label=", enc.keys[i].label,
                      ":key_id=", enc.keys[i].key_id,
                      ":key=", enc.keys[i].key);
    }
    argv.push_back("--keys");
    argv.push_back(keys);
    if (!enc.iv.empty()) {
      argv.push_back("--iv");
      argv.push_back(enc.iv);
    }
    argv.push_back("--clear_lead");
    argv.push_back(absl::StrCat(enc.clear_lead));
  }
  for (const std::string& arg : request.extra_args)
    argv.push_back(arg);
  return argv;
}

std::string GenerateEventId() {
  static const char kHex[] = "0123456789abcdef";
  std::random_device device;
  std::mt19937_64 rng(device());
  std::uniform_int_distribution<int> dist(0, 15);
  std::string id;
  id.reserve(32);
  for (int i = 0; i < 32; ++i)
    id.push_back(kHex[dist(rng)]);
  return id;
}

}  // namespace webapi
}  // namespace shaka

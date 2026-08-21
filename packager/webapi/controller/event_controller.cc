// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/webapi/controller/event_controller.hpp>

#include <deque>

namespace shaka {
namespace webapi {
namespace {

std::string ToStd(const oatpp::String& value) {
  return value ? (*value) : std::string();
}

}  // namespace

// static
EventCreateRequest EventController::FromDto(
    const oatpp::Object<EventCreateDto>& dto) {
  EventCreateRequest request;
  request.event_id = ToStd(dto->event_id);
  if (dto->streams) {
    for (const auto& stream_dto : *dto->streams) {
      StreamSpec stream;
      stream.input = ToStd(stream_dto->input);
      stream.stream = ToStd(stream_dto->stream);
      stream.init_segment = ToStd(stream_dto->init_segment);
      stream.segment_template = ToStd(stream_dto->segment_template);
      stream.output = ToStd(stream_dto->output);
      request.streams.push_back(stream);
    }
  }
  request.mpd_output = ToStd(dto->mpd_output);
  request.hls_master_playlist_output =
      ToStd(dto->hls_master_playlist_output);
  request.hls_playlist_type = ToStd(dto->hls_playlist_type);
  if (dto->segment_duration)
    request.segment_duration = *dto->segment_duration;
  if (dto->time_shift_buffer_depth)
    request.time_shift_buffer_depth = *dto->time_shift_buffer_depth;
  if (dto->encryption) {
    EncryptionSpec enc;
    enc.scheme = ToStd(dto->encryption->scheme);
    if (dto->encryption->keys) {
      for (const auto& key_dto : *dto->encryption->keys) {
        enc.keys.push_back({ToStd(key_dto->label), ToStd(key_dto->key_id),
                            ToStd(key_dto->key)});
      }
    }
    enc.iv = ToStd(dto->encryption->iv);
    if (dto->encryption->clear_lead)
      enc.clear_lead = *dto->encryption->clear_lead;
    request.encryption = enc;
  }
  if (dto->extra_args) {
    for (const auto& arg : *dto->extra_args)
      request.extra_args.push_back(ToStd(arg));
  }
  if (dto->stop_timeout_seconds)
    request.stop_timeout_seconds = *dto->stop_timeout_seconds;
  return request;
}

// static
std::string EventController::TailFile(const std::string& path, int lines) {
  std::ifstream in(path);
  std::deque<std::string> tail;
  std::string line;
  while (std::getline(in, line)) {
    tail.push_back(line);
    if (static_cast<int>(tail.size()) > lines)
      tail.pop_front();
  }
  std::string result;
  for (const std::string& kept : tail) {
    result += kept;
    result += '\n';
  }
  return result;
}

}  // namespace webapi
}  // namespace shaka

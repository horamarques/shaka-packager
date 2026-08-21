// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

namespace shaka {
namespace webapi {

#include OATPP_CODEGEN_BEGIN(DTO)

class StreamSpecDto : public oatpp::DTO {
  DTO_INIT(StreamSpecDto, DTO)
  DTO_FIELD(String, input);
  DTO_FIELD(String, stream);
  DTO_FIELD(String, init_segment);
  DTO_FIELD(String, segment_template);
  DTO_FIELD(String, output);
};

class EncryptionKeyDto : public oatpp::DTO {
  DTO_INIT(EncryptionKeyDto, DTO)
  DTO_FIELD(String, label) = "";
  DTO_FIELD(String, key_id);
  DTO_FIELD(String, key);
};

class EncryptionDto : public oatpp::DTO {
  DTO_INIT(EncryptionDto, DTO)
  DTO_FIELD(String, scheme) = "cenc";
  DTO_FIELD(List<Object<EncryptionKeyDto>>, keys);
  DTO_FIELD(String, iv);
  DTO_FIELD(Int32, clear_lead) = 0;
};

class EventCreateDto : public oatpp::DTO {
  DTO_INIT(EventCreateDto, DTO)
  DTO_FIELD(String, event_id);
  DTO_FIELD(List<Object<StreamSpecDto>>, streams);
  DTO_FIELD(String, mpd_output);
  DTO_FIELD(String, hls_master_playlist_output);
  DTO_FIELD(String, hls_playlist_type);
  DTO_FIELD(Int32, segment_duration) = 6;
  DTO_FIELD(Int32, time_shift_buffer_depth) = 0;
  DTO_FIELD(Object<EncryptionDto>, encryption);
  DTO_FIELD(List<String>, extra_args);
  DTO_FIELD(Int32, stop_timeout_seconds) = 10;
};

class EventStatusDto : public oatpp::DTO {
  DTO_INIT(EventStatusDto, DTO)
  DTO_FIELD(String, event_id);
  DTO_FIELD(String, state);
  DTO_FIELD(Int32, pid);
  DTO_FIELD(Int32, exit_code);          // present only when terminal
  DTO_FIELD(Int32, metrics_port);
  DTO_FIELD(String, log_path);
  DTO_FIELD(List<String>, argv);
  DTO_FIELD(Int64, created_unix);
  DTO_FIELD(Int64, started_unix);
  DTO_FIELD(Int64, stopped_unix);
  DTO_FIELD(Int64, uptime_seconds);
};

class EventListDto : public oatpp::DTO {
  DTO_INIT(EventListDto, DTO)
  DTO_FIELD(List<Object<EventStatusDto>>, events);
};

class ErrorDetailDto : public oatpp::DTO {
  DTO_INIT(ErrorDetailDto, DTO)
  DTO_FIELD(String, code);
  DTO_FIELD(String, message);
  DTO_FIELD(String, detail);
};

class ErrorDto : public oatpp::DTO {
  DTO_INIT(ErrorDto, DTO)
  DTO_FIELD(Object<ErrorDetailDto>, error);
};

class HealthDto : public oatpp::DTO {
  DTO_INIT(HealthDto, DTO)
  DTO_FIELD(String, status) = "ok";
  DTO_FIELD(String, packager_version);
};

#include OATPP_CODEGEN_END(DTO)

}  // namespace webapi
}  // namespace shaka

// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_WEBAPI_SERVICE_EVENT_SPEC_H_
#define PACKAGER_WEBAPI_SERVICE_EVENT_SPEC_H_

#include <optional>
#include <string>
#include <vector>

namespace shaka {
namespace webapi {

struct StreamSpec {
  std::string input;             // required
  std::string stream;            // required (selector: video/audio/text/0..)
  std::string init_segment;      // optional
  std::string segment_template;  // required for live
  std::string output;            // optional (VOD single-file)
};

struct EncryptionKeySpec {
  std::string label;   // may be empty
  std::string key_id;  // 32 hex chars
  std::string key;     // 32 hex chars
};

struct EncryptionSpec {
  std::string scheme;  // "cenc" | "cbcs"
  std::vector<EncryptionKeySpec> keys;
  std::string iv;      // optional; 16 or 32 hex chars
  int clear_lead = 0;
};

struct EventCreateRequest {
  std::string event_id;                       // optional; empty = generate
  std::vector<StreamSpec> streams;            // required, >= 1
  std::string mpd_output;                     // at least one output required
  std::string hls_master_playlist_output;
  std::string hls_playlist_type;              // LIVE|EVENT|VOD when HLS set
  int segment_duration = 6;
  int time_shift_buffer_depth = 0;            // 0 = omit flag
  std::optional<EncryptionSpec> encryption;
  std::vector<std::string> extra_args;
  int stop_timeout_seconds = 10;
};

/// Returns "" when valid, else "<field>: <problem>".
std::string ValidateEventRequest(const EventCreateRequest& request);

/// argv AFTER the binary path, WITHOUT --metrics_port (EventManager appends
/// that). Stream descriptors first, then flags.
std::vector<std::string> BuildEventArgv(const EventCreateRequest& request);

/// 32-hex-char lowercase random id, for requests without event_id.
std::string GenerateEventId();

}  // namespace webapi
}  // namespace shaka

#endif  // PACKAGER_WEBAPI_SERVICE_EVENT_SPEC_H_

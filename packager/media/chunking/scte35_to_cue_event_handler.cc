// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/chunking/scte35_to_cue_event_handler.h>

#include <absl/log/log.h>

#include <packager/media/chunking/sync_point_queue.h>

namespace shaka {
namespace media {

Scte35ToCueEventHandler::Scte35ToCueEventHandler(SyncPointQueue* sync_points)
    : sync_points_(sync_points) {}

void Scte35ToCueEventHandler::OnScte35Event(
    std::shared_ptr<const Scte35Event> event) {
  if (!event || !sync_points_)
    return;

  CueEventType cue_type = MapSegmentationTypeToCueType(event->type);

  LOG(INFO) << "SCTE-35 event: id=" << event->id
            << " type=0x" << std::hex << event->type << std::dec
            << " time=" << event->start_time_in_seconds << "s"
            << " -> " << (cue_type == CueEventType::kCueOut ? "CUE-OUT"
                          : cue_type == CueEventType::kCueIn ? "CUE-IN"
                          : "CUE-POINT");

  sync_points_->AddDynamicCuePoint(event->start_time_in_seconds, cue_type,
                                   event->duration_in_seconds,
                                   event->cue_data);
}

CueEventType Scte35ToCueEventHandler::MapSegmentationTypeToCueType(
    int type_id) {
  // SCTE-35 segmentation_type_id (Table 23):
  // Even types in 0x30-0x46 are "start" (cue out).
  // Odd types in 0x31-0x47 are "end" (cue in).
  if (type_id >= 0x30 && type_id <= 0x47) {
    return (type_id % 2 == 0) ? CueEventType::kCueOut : CueEventType::kCueIn;
  }
  return CueEventType::kCuePoint;
}

}  // namespace media
}  // namespace shaka

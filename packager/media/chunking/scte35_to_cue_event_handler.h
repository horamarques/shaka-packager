// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_MEDIA_CHUNKING_SCTE35_TO_CUE_EVENT_HANDLER_H_
#define PACKAGER_MEDIA_CHUNKING_SCTE35_TO_CUE_EVENT_HANDLER_H_

#include <memory>
#include <string>

#include <packager/media/base/media_handler.h>

namespace shaka {
namespace media {

class SyncPointQueue;

class Scte35ToCueEventHandler {
 public:
  explicit Scte35ToCueEventHandler(SyncPointQueue* sync_points);

  void OnScte35Event(std::shared_ptr<const Scte35Event> event);

  static CueEventType MapSegmentationTypeToCueType(int type_id);

 private:
  SyncPointQueue* sync_points_;
};

}  // namespace media
}  // namespace shaka

#endif  // PACKAGER_MEDIA_CHUNKING_SCTE35_TO_CUE_EVENT_HANDLER_H_

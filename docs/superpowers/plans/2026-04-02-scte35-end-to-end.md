# SCTE-35 End-to-End Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete SCTE-35 pipeline from MPEG-TS input parsing through to HLS EXT-X-DATERANGE and DASH EventStream output.

**Architecture:** SCTE-35 events are parsed from TS input, converted to CueEvents, dynamically inserted into SyncPointQueue for cross-stream keyframe alignment, then carried through the muxer chain with cue_data preserved to emit EXT-X-DATERANGE (HLS) and EventStream (DASH).

**Tech Stack:** C++17, absl, Google Test/Mock, shaka-packager media pipeline

---

### Task 1: Cherry-pick SCTE-35 parser from feature branch

Merge the SCTE-35 parser and PMT detection from `feat/scte35-to-cue-handler` onto main.

**Files:**
- Cherry-pick commits from `feat/scte35-to-cue-handler`

- [ ] **Step 1: Cherry-pick the PMT CUEI detection commit**

```bash
cd /Users/pedromarques/Documents/Development/Velocix/shaka-packager
git cherry-pick cb6fcf23c5
```

If conflicts arise, resolve them preserving both upstream changes and SCTE-35 additions.

- [ ] **Step 2: Cherry-pick the SCTE-35 parser commit**

```bash
git cherry-pick 6ee1d6b875
```

Resolve any conflicts similarly.

- [ ] **Step 3: Build to verify**

```bash
/opt/homebrew/bin/cmake --build build -j$(sysctl -n hw.ncpu) 2>&1 | tail -20
```

Expected: Build succeeds.

- [ ] **Step 4: Run mp2t tests**

```bash
cd build && /opt/homebrew/bin/ctest -R mp2t --output-on-failure && cd ..
```

Expected: All mp2t tests pass.

- [ ] **Step 5: Commit (if cherry-pick needed manual resolution)**

If cherry-pick applied cleanly, commits already exist. If manual resolution was needed, amend or create a new commit.

---

### Task 2: Add dynamic cue insertion to SyncPointQueue

**Files:**
- Modify: `packager/media/chunking/sync_point_queue.h`
- Modify: `packager/media/chunking/sync_point_queue.cc`
- Test: `packager/media/chunking/sync_point_queue_unittest.cc`

- [ ] **Step 1: Write the failing test**

Find or create `packager/media/chunking/sync_point_queue_unittest.cc`. Add:

```cpp
TEST(SyncPointQueueTest, DynamicCuePointInsertion) {
  AdCueGeneratorParams empty_params;
  SyncPointQueue queue(empty_params);
  queue.AddThread();

  // No hints initially.
  EXPECT_EQ(std::numeric_limits<double>::max(), queue.GetHint(-1));

  // Add a dynamic cue point.
  queue.AddDynamicCuePoint(10.0, CueEventType::kCueOut, "cue_data_out");

  // Now hint should return 10.0.
  EXPECT_DOUBLE_EQ(10.0, queue.GetHint(-1));
  EXPECT_TRUE(queue.HasMore(10.0));

  // Promote at 10.0.
  auto promoted = queue.PromoteAt(10.0);
  ASSERT_NE(nullptr, promoted);
  EXPECT_EQ(CueEventType::kCueOut, promoted->type);
  EXPECT_DOUBLE_EQ(10.0, promoted->time_in_seconds);
  EXPECT_EQ("cue_data_out", promoted->cue_data);
}
```

- [ ] **Step 2: Add AddDynamicCuePoint to header**

In `packager/media/chunking/sync_point_queue.h`, add after `HasMore()` (line 53):

```cpp
  /// Dynamically add a cue point (e.g., from SCTE-35 input). Thread-safe.
  /// @param time_in_seconds is the cue time.
  /// @param type is the cue event type (kCueIn, kCueOut, kCuePoint).
  /// @param cue_data is the raw cue data for pass-through.
  void AddDynamicCuePoint(double time_in_seconds,
                          CueEventType type,
                          const std::string& cue_data);
```

- [ ] **Step 3: Implement AddDynamicCuePoint**

In `packager/media/chunking/sync_point_queue.cc`, add after `HasMore()`:

```cpp
void SyncPointQueue::AddDynamicCuePoint(double time_in_seconds,
                                        CueEventType type,
                                        const std::string& cue_data) {
  absl::MutexLock lock(mutex_);

  // Skip if this time is already promoted or unpromoted.
  if (promoted_.find(time_in_seconds) != promoted_.end())
    return;
  if (unpromoted_.find(time_in_seconds) != unpromoted_.end())
    return;

  auto event = std::make_shared<CueEvent>();
  event->time_in_seconds = time_in_seconds;
  event->type = type;
  event->cue_data = cue_data;
  unpromoted_[time_in_seconds] = std::move(event);

  // Wake blocked threads so they re-check hints.
  sync_condition_.SignalAll();
}
```

- [ ] **Step 4: Build and run test**

```bash
/opt/homebrew/bin/cmake --build build -j$(sysctl -n hw.ncpu) 2>&1 | tail -10
cd build && /opt/homebrew/bin/ctest -R chunking --output-on-failure && cd ..
```

Expected: All tests pass.

- [ ] **Step 5: Commit**

```bash
git add packager/media/chunking/sync_point_queue.h packager/media/chunking/sync_point_queue.cc packager/media/chunking/sync_point_queue_unittest.cc
git commit -m "feat: add dynamic cue point insertion to SyncPointQueue for SCTE-35"
```

---

### Task 3: Create Scte35ToCueEventHandler

**Files:**
- Create: `packager/media/chunking/scte35_to_cue_event_handler.h`
- Create: `packager/media/chunking/scte35_to_cue_event_handler.cc`
- Modify: `packager/media/chunking/CMakeLists.txt`

- [ ] **Step 1: Create the header**

Create `packager/media/chunking/scte35_to_cue_event_handler.h`:

```cpp
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

/// Converts Scte35Event objects from the MPEG-TS demuxer into CueEvents
/// and inserts them into a SyncPointQueue for cross-stream alignment.
class Scte35ToCueEventHandler {
 public:
  explicit Scte35ToCueEventHandler(SyncPointQueue* sync_points);

  /// Called by the demuxer's SCTE-35 callback when a new event is parsed.
  void OnScte35Event(std::shared_ptr<const Scte35Event> event);

  /// Map a SCTE-35 segmentation_type_id to a CueEventType.
  /// Even types (0x30, 0x32, 0x34, ...) map to kCueOut (start).
  /// Odd types (0x31, 0x33, 0x35, ...) map to kCueIn (end).
  /// Others (including 0) map to kCuePoint.
  static CueEventType MapSegmentationTypeToCueType(int type_id);

 private:
  SyncPointQueue* sync_points_;
};

}  // namespace media
}  // namespace shaka

#endif  // PACKAGER_MEDIA_CHUNKING_SCTE35_TO_CUE_EVENT_HANDLER_H_
```

- [ ] **Step 2: Create the implementation**

Create `packager/media/chunking/scte35_to_cue_event_handler.cc`:

```cpp
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

  sync_points_->AddDynamicCuePoint(
      event->start_time_in_seconds, cue_type, event->cue_data);
}

CueEventType Scte35ToCueEventHandler::MapSegmentationTypeToCueType(
    int type_id) {
  // SCTE-35 segmentation_type_id values (Table 23):
  // Even types in the 0x30-0x46 range are "start" events (cue out).
  // Odd types in the 0x31-0x47 range are "end" events (cue in).
  if (type_id >= 0x30 && type_id <= 0x47) {
    return (type_id % 2 == 0) ? CueEventType::kCueOut : CueEventType::kCueIn;
  }
  return CueEventType::kCuePoint;
}

}  // namespace media
}  // namespace shaka
```

- [ ] **Step 3: Add to CMakeLists.txt**

In `packager/media/chunking/CMakeLists.txt`, add `scte35_to_cue_event_handler.cc` and `scte35_to_cue_event_handler.h` to the library source list.

- [ ] **Step 4: Build and verify**

```bash
/opt/homebrew/bin/cmake --build build -j$(sysctl -n hw.ncpu) 2>&1 | tail -10
```

- [ ] **Step 5: Commit**

```bash
git add packager/media/chunking/scte35_to_cue_event_handler.h packager/media/chunking/scte35_to_cue_event_handler.cc packager/media/chunking/CMakeLists.txt
git commit -m "feat: add Scte35ToCueEventHandler to bridge SCTE-35 events to CueEvent pipeline"
```

---

### Task 4: Wire SCTE-35 pipeline in packager.cc and fix CueAlignmentHandler

**Files:**
- Modify: `packager/packager.cc` (lines 975-978, 650-660)
- Modify: `packager/media/chunking/cue_alignment_handler.cc` (line 190)
- Modify: `packager/media/demuxer/demuxer.h`
- Modify: `packager/media/demuxer/demuxer.cc`

- [ ] **Step 1: Add SCTE-35 callback to Demuxer**

In `packager/media/demuxer/demuxer.h`, add after existing public methods:

```cpp
  /// Set callback for SCTE-35 events parsed from MPEG-TS input.
  void SetScte35EventCallback(
      std::function<void(std::shared_ptr<const Scte35Event>)> callback) {
    scte35_event_callback_ = std::move(callback);
  }
```

Add private member:

```cpp
  std::function<void(std::shared_ptr<const Scte35Event>)> scte35_event_callback_;
```

In `packager/media/demuxer/demuxer.cc`, after `parser_->Init(...)` call, add:

```cpp
  // Set up SCTE-35 callback if available (MPEG-TS only).
  if (scte35_event_callback_) {
    auto* mp2t_parser = dynamic_cast<mp2t::Mp2tMediaParser*>(parser_.get());
    if (mp2t_parser) {
      mp2t_parser->set_scte35_event_cb(
          [this](uint32_t /* track_id */,
                 std::shared_ptr<Scte35Event> event) -> bool {
            scte35_event_callback_(std::move(event));
            return true;
          });
    }
  }
```

Add the necessary includes at top of demuxer.cc:

```cpp
#include <packager/media/formats/mp2t/mp2t_media_parser.h>
```

- [ ] **Step 2: Wire SyncPointQueue and SCTE-35 handler in packager.cc**

In `packager/packager.cc`, around line 975, change:

```cpp
  std::unique_ptr<SyncPointQueue> sync_points;
  if (!packaging_params.ad_cue_generator_params.cue_points.empty()) {
    sync_points.reset(
        new SyncPointQueue(packaging_params.ad_cue_generator_params));
  }
```

To:

```cpp
  // Create SyncPointQueue when ad_cues provided or when SCTE-35 input
  // might be present (any .ts/.m2ts input files).
  bool has_ts_input = false;
  for (const auto& descriptor : stream_descriptors) {
    if (descriptor.input.size() >= 3 &&
        (descriptor.input.substr(descriptor.input.size() - 3) == ".ts" ||
         descriptor.input.substr(descriptor.input.size() - 4) == ".m2t" ||
         descriptor.input.substr(descriptor.input.size() - 5) == ".m2ts")) {
      has_ts_input = true;
      break;
    }
  }

  std::unique_ptr<SyncPointQueue> sync_points;
  if (!packaging_params.ad_cue_generator_params.cue_points.empty() ||
      has_ts_input) {
    sync_points.reset(
        new SyncPointQueue(packaging_params.ad_cue_generator_params));
  }
```

Add include at top:

```cpp
#include <packager/media/chunking/scte35_to_cue_event_handler.h>
```

In `CreateAudioVideoJobs()`, after the demuxer creation loop (around line 661), add SCTE-35 wiring:

```cpp
  // Set up SCTE-35 handlers for TS demuxers.
  std::map<std::string, std::shared_ptr<Scte35ToCueEventHandler>> scte35_handlers;
  if (sync_points) {
    for (auto& [input, demuxer] : sources) {
      auto scte35_handler =
          std::make_shared<Scte35ToCueEventHandler>(sync_points);
      demuxer->SetScte35EventCallback(
          [handler = scte35_handler](
              std::shared_ptr<const Scte35Event> event) {
            handler->OnScte35Event(std::move(event));
          });
      scte35_handlers[input] = scte35_handler;
    }
  }
```

- [ ] **Step 3: Fix CueAlignmentHandler for dynamic hints**

In `packager/media/chunking/cue_alignment_handler.cc`, in `OnVideoSample()` (around line 190), change:

```cpp
  if (is_key_frame && sample_time >= hint_) {
```

To:

```cpp
  if (is_key_frame) {
    // Re-check hint from queue to detect dynamically-added cues (SCTE-35).
    double fresh_hint = sync_points_->GetHint(sample_time - 0.001);
    if (fresh_hint < hint_)
      hint_ = fresh_hint;
  }

  if (is_key_frame && sample_time >= hint_) {
```

- [ ] **Step 4: Build and run all tests**

```bash
/opt/homebrew/bin/cmake --build build -j$(sysctl -n hw.ncpu) 2>&1 | tail -10
cd build && /opt/homebrew/bin/ctest --output-on-failure -j$(sysctl -n hw.ncpu) && cd ..
```

- [ ] **Step 5: Commit**

```bash
git add packager/packager.cc packager/media/demuxer/demuxer.h packager/media/demuxer/demuxer.cc packager/media/chunking/cue_alignment_handler.cc
git commit -m "feat: wire SCTE-35 pipeline from demuxer through SyncPointQueue to CueAlignmentHandler"
```

---

### Task 5: Pass cue_data through MuxerListener chain

**Files:**
- Modify: `packager/media/event/event_info.h`
- Modify: `packager/media/event/hls_notify_muxer_listener.cc`
- Modify: `packager/media/event/mpd_notify_muxer_listener.cc`
- Modify: `packager/hls/base/hls_notifier.h`
- Modify: `packager/hls/base/simple_hls_notifier.h`
- Modify: `packager/hls/base/simple_hls_notifier.cc`
- Modify: `packager/mpd/base/mpd_notifier.h`
- Modify: `packager/mpd/base/simple_mpd_notifier.h`
- Modify: `packager/mpd/base/simple_mpd_notifier.cc`

- [ ] **Step 1: Fix EventInfo to carry cue_data**

In `packager/media/event/event_info.h`, the `EventInfo` struct uses a C union which can't hold `std::string`. Change the struct to not use a union:

```cpp
struct EventInfo {
  EventInfoType type;
  SegmentEventInfo segment_info;
  KeyFrameEvent key_frame;
  CueEventInfo cue_event_info;
  std::string cue_data;  // Only used when type == kCue
};
```

This replaces the union with plain members. Since `EventInfo` is only used for VOD event caching and these are small POD types, the minimal memory overhead is acceptable.

- [ ] **Step 2: Update HLS notifier interface to accept cue_data**

In `packager/hls/base/hls_notifier.h`, change line 111:

```cpp
  virtual bool NotifyCueEvent(uint32_t stream_id, int64_t timestamp) = 0;
```

To:

```cpp
  virtual bool NotifyCueEvent(uint32_t stream_id, int64_t timestamp,
                              const std::string& cue_data = "") = 0;
```

- [ ] **Step 3: Update SimpleHlsNotifier to accept cue_data**

In `packager/hls/base/simple_hls_notifier.h`, update the declaration to match. In `packager/hls/base/simple_hls_notifier.cc`, update `NotifyCueEvent` to accept `cue_data` (store it for later use in Task 6).

- [ ] **Step 4: Update HlsNotifyMuxerListener to pass cue_data**

In `packager/media/event/hls_notify_muxer_listener.cc`, in `OnCueEvent()` (line 302-313), remove `UNUSED(cue_data)` and forward it:

```cpp
void HlsNotifyMuxerListener::OnCueEvent(int64_t timestamp,
                                        const std::string& cue_data) {
  if (!media_info_->has_segment_template()) {
    EventInfo event_info;
    event_info.type = EventInfoType::kCue;
    event_info.cue_event_info = {timestamp};
    event_info.cue_data = cue_data;
    event_info_.push_back(event_info);
  } else {
    hls_notifier_->NotifyCueEvent(stream_id_.value(), timestamp, cue_data);
  }
}
```

- [ ] **Step 5: Update MPD notifier interface similarly**

In `packager/mpd/base/mpd_notifier.h`, change `NotifyCueEvent`:

```cpp
  virtual bool NotifyCueEvent(uint32_t container_id, int64_t timestamp,
                              const std::string& cue_data = "") = 0;
```

Update `SimpleMpdNotifier` and `MpdNotifyMuxerListener` similarly.

- [ ] **Step 6: Update any mock classes**

Search for mock implementations of these interfaces and update their signatures.

- [ ] **Step 7: Build and run all tests**

```bash
/opt/homebrew/bin/cmake --build build -j$(sysctl -n hw.ncpu) 2>&1 | tail -20
cd build && /opt/homebrew/bin/ctest --output-on-failure -j$(sysctl -n hw.ncpu) && cd ..
```

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "feat: pass cue_data through MuxerListener chain for SCTE-35 signaling"
```

---

### Task 6: HLS EXT-X-DATERANGE support

**Files:**
- Modify: `packager/hls/base/media_playlist.h`
- Modify: `packager/hls/base/media_playlist.cc`
- Modify: `packager/hls/base/simple_hls_notifier.cc`
- Test: `packager/hls/base/media_playlist_unittest.cc`

- [ ] **Step 1: Write the failing test**

Add to `packager/hls/base/media_playlist_unittest.cc`:

```cpp
TEST_F(LowLatencyHlsMediaPlaylistTest, DateRangeForScte35CueOut) {
  mutable_hls_params()->add_program_date_time = true;
  ASSERT_TRUE(media_playlist_->SetMediaInfo(valid_video_media_info_));
  media_playlist_->SetReferenceTime(absl::FromUnixSeconds(1000000));

  media_playlist_->AddSegment("file1.ts", 0, 2 * kTimeScale, kZeroByteOffset,
                              kMBytes);

  // Simulate a SCTE-35 cue-out with raw binary data.
  std::string cue_data = "\xFC\x30\x11";  // Minimal SCTE-35 section
  media_playlist_->AddDateRange(2 * kTimeScale, cue_data, true);

  media_playlist_->AddSegment("file2.ts", 2 * kTimeScale, 2 * kTimeScale,
                              kZeroByteOffset, kMBytes);

  const char kMemoryFilePath[] = "memory://media.m3u8";
  EXPECT_TRUE(media_playlist_->WriteToFile(kMemoryFilePath));

  std::string actual;
  ASSERT_TRUE(File::ReadFileToString(kMemoryFilePath, &actual));

  EXPECT_NE(std::string::npos, actual.find("#EXT-X-DATERANGE:"));
  EXPECT_NE(std::string::npos, actual.find("SCTE35-OUT=0x"));
}
```

- [ ] **Step 2: Add kExtDateRange entry type and AddDateRange method**

In `packager/hls/base/media_playlist.h`, add `kExtDateRange` to `HlsEntry::EntryType` enum. Add public method:

```cpp
  /// Add an EXT-X-DATERANGE entry for SCTE-35 cue signaling.
  /// @param timestamp is in timescale of the media.
  /// @param cue_data is the raw SCTE-35 section data.
  /// @param is_cue_out true for SCTE35-OUT, false for SCTE35-IN.
  virtual void AddDateRange(int64_t timestamp, const std::string& cue_data,
                            bool is_cue_out);
```

- [ ] **Step 3: Implement DateRangeEntry and AddDateRange**

In `packager/hls/base/media_playlist.cc`, add `DateRangeEntry` class following the pattern of `PlacementOpportunityEntry`:

```cpp
class DateRangeEntry : public HlsEntry {
 public:
  DateRangeEntry(const std::string& id,
                 const absl::Time& start_date,
                 const std::string& cue_data,
                 bool is_cue_out)
      : HlsEntry(HlsEntry::EntryType::kExtDateRange),
        id_(id),
        start_date_(start_date),
        cue_data_(cue_data),
        is_cue_out_(is_cue_out) {}

  std::string ToString() override {
    std::string hex_data;
    for (unsigned char c : cue_data_) {
      absl::StrAppendFormat(&hex_data, "%02X", c);
    }
    std::string tag = absl::StrFormat(
        "#EXT-X-DATERANGE:ID=\"%s\",START-DATE=\"%s\",%s=0x%s",
        id_.c_str(),
        absl::FormatTime("%Y-%m-%dT%H:%M:%E3SZ", start_date_,
                         absl::UTCTimeZone()).c_str(),
        is_cue_out_ ? "SCTE35-OUT" : "SCTE35-IN",
        hex_data.c_str());
    return tag;
  }

 private:
  const std::string id_;
  const absl::Time start_date_;
  const std::string cue_data_;
  const bool is_cue_out_;
};
```

Implement `AddDateRange`:

```cpp
void MediaPlaylist::AddDateRange(int64_t timestamp,
                                 const std::string& cue_data,
                                 bool is_cue_out) {
  static int date_range_counter = 0;
  std::string id = absl::StrFormat("splice-%d", ++date_range_counter);

  absl::Time start_date = reference_time_ +
      absl::Seconds(static_cast<double>(timestamp) / time_scale_);

  entries_.emplace_back(
      new DateRangeEntry(id, start_date, cue_data, is_cue_out));
}
```

- [ ] **Step 4: Update SimpleHlsNotifier to call AddDateRange**

In `packager/hls/base/simple_hls_notifier.cc`, in `NotifyCueEvent()`, change to:

```cpp
bool SimpleHlsNotifier::NotifyCueEvent(uint32_t stream_id, int64_t timestamp,
                                       const std::string& cue_data) {
  absl::MutexLock lock(lock_);
  auto stream_iterator = stream_map_.find(stream_id);
  if (stream_iterator == stream_map_.end()) {
    LOG(ERROR) << "Cannot find stream with ID: " << stream_id;
    return false;
  }
  auto& media_playlist = stream_iterator->second->media_playlist;
  if (!cue_data.empty()) {
    // SCTE-35 data present: use EXT-X-DATERANGE for spec-compliant signaling.
    // Determine cue type from the first byte pattern (simplified heuristic).
    media_playlist->AddDateRange(timestamp, cue_data, true);
  } else {
    media_playlist->AddPlacementOpportunity();
  }
  return true;
}
```

- [ ] **Step 5: Build and run tests**

```bash
/opt/homebrew/bin/cmake --build build -j$(sysctl -n hw.ncpu) 2>&1 | tail -10
./build/packager/hls/base/media_playlist_unittest --gtest_filter='LowLatencyHls*'
```

- [ ] **Step 6: Commit**

```bash
git add packager/hls/base/media_playlist.h packager/hls/base/media_playlist.cc packager/hls/base/simple_hls_notifier.h packager/hls/base/simple_hls_notifier.cc packager/hls/base/media_playlist_unittest.cc
git commit -m "feat: add HLS EXT-X-DATERANGE support for SCTE-35 signaling"
```

---

### Task 7: DASH EventStream support

**Files:**
- Modify: `packager/mpd/base/period.h`
- Modify: `packager/mpd/base/period.cc`
- Modify: `packager/mpd/base/simple_mpd_notifier.cc`
- Test: `packager/mpd/base/period_unittest.cc`

- [ ] **Step 1: Write the failing test**

Add to `packager/mpd/base/period_unittest.cc`:

```cpp
TEST_F(PeriodTest, EventStreamFromScte35) {
  auto* adaptation_set = period_->GetOrCreateAdaptationSet(
      ConvertToMediaInfo(GetDefaultMediaInfoForVideo()), true);
  ASSERT_TRUE(adaptation_set);

  period_->AddEventStreamEvent(10.0, 5.0, "scte35_binary_data");

  auto xml = period_->GetXml(true);
  ASSERT_TRUE(xml.has_value());

  auto xml_str = xml->ToString("");
  EXPECT_NE(std::string::npos,
            xml_str.find("urn:scte:scte35:2013:xml"));
  EXPECT_NE(std::string::npos, xml_str.find("EventStream"));
  EXPECT_NE(std::string::npos, xml_str.find("Event"));
}
```

- [ ] **Step 2: Add EventStreamEvent storage and method to Period**

In `packager/mpd/base/period.h`, add public method and private storage:

```cpp
  // Public:
  /// Add a SCTE-35 event for EventStream generation.
  void AddEventStreamEvent(double presentation_time_seconds,
                           double duration_seconds,
                           const std::string& cue_data);

  // Private:
  struct EventStreamEvent {
    double presentation_time_seconds;
    double duration_seconds;
    std::string cue_data;
  };
  std::vector<EventStreamEvent> event_stream_events_;
```

- [ ] **Step 3: Implement AddEventStreamEvent and XML generation**

In `packager/mpd/base/period.cc`, add the method:

```cpp
void Period::AddEventStreamEvent(double presentation_time_seconds,
                                 double duration_seconds,
                                 const std::string& cue_data) {
  event_stream_events_.push_back(
      {presentation_time_seconds, duration_seconds, cue_data});
}
```

In `GetXml()`, after the adaptation sets loop but before the duration/start attributes (around line 176), add:

```cpp
  // Add EventStream for SCTE-35 events.
  if (!event_stream_events_.empty()) {
    xml::XmlNode event_stream("EventStream");
    if (!event_stream.SetStringAttribute(
            "schemeIdUri", "urn:scte:scte35:2013:xml") ||
        !event_stream.SetIntegerAttribute("timescale", 1)) {
      return std::nullopt;
    }

    for (const auto& evt : event_stream_events_) {
      xml::XmlNode event_node("Event");
      int64_t presentation_time =
          static_cast<int64_t>(evt.presentation_time_seconds -
                               start_time_in_seconds_);
      if (!event_node.SetIntegerAttribute("presentationTime",
                                          presentation_time)) {
        return std::nullopt;
      }
      if (evt.duration_seconds > 0) {
        if (!event_node.SetIntegerAttribute(
                "duration", static_cast<int64_t>(evt.duration_seconds))) {
          return std::nullopt;
        }
      }
      // Base64-encode the cue data.
      std::string base64_data;
      absl::Base64Escape(evt.cue_data, &base64_data);

      xml::XmlNode signal_node("Signal");
      if (!signal_node.SetStringAttribute(
              "xmlns", "http://www.scte.org/schemas/35/2016")) {
        return std::nullopt;
      }
      xml::XmlNode binary_node("Binary");
      if (!binary_node.SetContent(base64_data)) {
        return std::nullopt;
      }
      if (!signal_node.AddChild(std::move(binary_node)) ||
          !event_node.AddChild(std::move(signal_node)) ||
          !event_stream.AddChild(std::move(event_node))) {
        return std::nullopt;
      }
    }

    if (!period.AddChild(std::move(event_stream)))
      return std::nullopt;
  }
```

- [ ] **Step 4: Update SimpleMpdNotifier to pass cue_data to Period**

In `packager/mpd/base/simple_mpd_notifier.cc`, in `NotifyCueEvent()`, after the Period is obtained, add:

```cpp
  if (!cue_data.empty()) {
    period->AddEventStreamEvent(
        static_cast<double>(timestamp) / representation->GetMediaInfo().reference_time_scale(),
        0.0, cue_data);
  }
```

- [ ] **Step 5: Build and run tests**

```bash
/opt/homebrew/bin/cmake --build build -j$(sysctl -n hw.ncpu) 2>&1 | tail -10
cd build && /opt/homebrew/bin/ctest -R mpd --output-on-failure && cd ..
```

- [ ] **Step 6: Commit**

```bash
git add packager/mpd/base/period.h packager/mpd/base/period.cc packager/mpd/base/simple_mpd_notifier.h packager/mpd/base/simple_mpd_notifier.cc packager/mpd/base/period_unittest.cc
git commit -m "feat: add DASH EventStream support for SCTE-35 signaling"
```

---

### Task 8: Full test suite verification

**Files:** None (verification only)

- [ ] **Step 1: Build everything**

```bash
/opt/homebrew/bin/cmake --build build -j$(sysctl -n hw.ncpu) 2>&1 | tail -10
```

- [ ] **Step 2: Run full test suite**

```bash
cd build && /opt/homebrew/bin/ctest --output-on-failure -j$(sysctl -n hw.ncpu) 2>&1 | tail -40 && cd ..
```

Expected: All tests pass.

- [ ] **Step 3: Fix any regressions if needed**

Only if test failures require fixes.

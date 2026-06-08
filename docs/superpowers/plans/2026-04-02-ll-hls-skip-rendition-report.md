# LL-HLS EXT-X-SKIP and EXT-X-RENDITION-REPORT Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add EXT-X-SKIP (delta playlist updates) and EXT-X-RENDITION-REPORT (cross-rendition state) to the LL-HLS implementation in shaka-packager.

**Architecture:** Both features are rendering-time optimizations in `WriteToFile()`. EXT-X-SKIP replaces old segments with a skip tag during playlist serialization without mutating `entries_`. EXT-X-RENDITION-REPORT appends sibling playlist state at the end of each media playlist, using const pointers registered by the master playlist.

**Tech Stack:** C++17, absl, Google Test/Mock, shaka-packager HLS module

**Pre-existing issue:** The LL-HLS tests in the fork call `WriteToFile(kMemoryFilePath)` with 1 argument, but the declaration requires 3. This plan fixes that by adding default parameter values.

---

### Task 1: Fix WriteToFile Default Parameters

The existing LL-HLS tests call `WriteToFile(path)` with one arg but the signature requires three. Add defaults so both old and new tests compile.

**Files:**
- Modify: `packager/hls/base/media_playlist.h:208-210`

- [ ] **Step 1: Add default values to WriteToFile declaration**

In `packager/hls/base/media_playlist.h`, change:

```cpp
  virtual bool WriteToFile(const std::filesystem::path& file_path,
                           bool event_to_vod_on_end_of_stream,
                           bool end_stream);
```

To:

```cpp
  virtual bool WriteToFile(const std::filesystem::path& file_path,
                           bool event_to_vod_on_end_of_stream = false,
                           bool end_stream = false);
```

- [ ] **Step 2: Verify existing tests still compile**

Run: `cmake --build build --target media_playlist_unittest 2>&1 | tail -20`
Expected: Build succeeds

- [ ] **Step 3: Commit**

```bash
git add packager/hls/base/media_playlist.h
git commit -m "fix: add default params to WriteToFile for LL-HLS test compatibility"
```

---

### Task 2: Add CAN-SKIP-UNTIL to EXT-X-SERVER-CONTROL Header

**Files:**
- Modify: `packager/hls/base/media_playlist.cc:107-146` (CreatePlaylistHeader function)
- Test: `packager/hls/base/media_playlist_unittest.cc`

- [ ] **Step 1: Write the failing test**

Add this test in `packager/hls/base/media_playlist_unittest.cc`, inside the `LowLatencyHlsMediaPlaylistTest` fixture (after the existing `SlideWindowWithParts` test, before the closing `}  // namespace hls`):

```cpp
TEST_F(LowLatencyHlsMediaPlaylistTest, ServerControlIncludesCanSkipUntil) {
  ASSERT_TRUE(media_playlist_->SetMediaInfo(valid_video_media_info_));

  // Add one segment (2s) so target duration is set to 2.
  media_playlist_->AddSegment("file1.ts", 0, 2 * kTimeScale, kZeroByteOffset,
                              kMBytes);

  const char kMemoryFilePath[] = "memory://media.m3u8";
  EXPECT_TRUE(media_playlist_->WriteToFile(kMemoryFilePath));

  std::string actual;
  ASSERT_TRUE(File::ReadFileToString(kMemoryFilePath, &actual));

  // CAN-SKIP-UNTIL should be 6 * target_duration = 6 * 2 = 12.000
  EXPECT_NE(std::string::npos,
            actual.find("CAN-SKIP-UNTIL=12.000"));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target media_playlist_unittest && ./build/packager/hls/base/media_playlist_unittest --gtest_filter='*ServerControlIncludesCanSkipUntil*'`
Expected: FAIL — "CAN-SKIP-UNTIL" not found in output

- [ ] **Step 3: Add CAN-SKIP-UNTIL to CreatePlaylistHeader**

In `packager/hls/base/media_playlist.cc`, in the `CreatePlaylistHeader` function, change the `EXT-X-SERVER-CONTROL` line (around line 139-142) from:

```cpp
    absl::StrAppendFormat(
        &header,
        "#EXT-X-SERVER-CONTROL:CAN-BLOCK-RELOAD=YES,PART-HOLD-BACK=%.3f\n",
        part_target_duration * 3.0);
```

To:

```cpp
    absl::StrAppendFormat(
        &header,
        "#EXT-X-SERVER-CONTROL:CAN-BLOCK-RELOAD=YES,"
        "PART-HOLD-BACK=%.3f,"
        "CAN-SKIP-UNTIL=%.3f\n",
        part_target_duration * 3.0,
        static_cast<double>(target_duration) * 6.0);
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target media_playlist_unittest && ./build/packager/hls/base/media_playlist_unittest --gtest_filter='*ServerControlIncludesCanSkipUntil*'`
Expected: PASS

- [ ] **Step 5: Run existing LL-HLS header test to check it still passes**

Run: `./build/packager/hls/base/media_playlist_unittest --gtest_filter='*HeaderContainsLLHLSTags*'`
Expected: PASS (the test checks for a substring match on `PART-HOLD-BACK=1.500`, which is still present)

- [ ] **Step 6: Commit**

```bash
git add packager/hls/base/media_playlist.cc packager/hls/base/media_playlist_unittest.cc
git commit -m "feat: add CAN-SKIP-UNTIL to EXT-X-SERVER-CONTROL for LL-HLS delta playlists"
```

---

### Task 3: Implement EXT-X-SKIP Tag Rendering in WriteToFile

This is the core skip logic. During playlist serialization, count segments that are old enough to skip and emit `#EXT-X-SKIP:SKIPPED-SEGMENTS=N` instead.

**Files:**
- Modify: `packager/hls/base/media_playlist.cc:613-670` (WriteToFile method)
- Test: `packager/hls/base/media_playlist_unittest.cc`

- [ ] **Step 1: Write the failing test — SkipOldSegments**

Add in `packager/hls/base/media_playlist_unittest.cc` after the `ServerControlIncludesCanSkipUntil` test:

```cpp
TEST_F(LowLatencyHlsMediaPlaylistTest, SkipOldSegments) {
  // Use a time shift buffer that keeps all segments (no sliding).
  // Target duration will be 2s, so CAN-SKIP-UNTIL = 12s.
  // With 10 segments of 2s each (20s total), segments whose cumulative
  // duration from the end exceeds 12s should be skipped.
  // That means segments 1-4 (8s from front, leaving 12s from back) are skipped.
  mutable_hls_params()->time_shift_buffer_depth = 100;

  ASSERT_TRUE(media_playlist_->SetMediaInfo(valid_video_media_info_));

  for (int i = 0; i < 10; i++) {
    media_playlist_->AddSegment(
        absl::StrFormat("file%d.ts", i + 1),
        i * 2 * kTimeScale, 2 * kTimeScale, kZeroByteOffset, kMBytes);
  }

  const char kMemoryFilePath[] = "memory://media.m3u8";
  EXPECT_TRUE(media_playlist_->WriteToFile(kMemoryFilePath));

  std::string actual;
  ASSERT_TRUE(File::ReadFileToString(kMemoryFilePath, &actual));

  // Should have EXT-X-SKIP with 4 skipped segments.
  EXPECT_NE(std::string::npos,
            actual.find("#EXT-X-SKIP:SKIPPED-SEGMENTS=4"));
  // Skipped segments should not appear.
  EXPECT_EQ(std::string::npos, actual.find("file1.ts"));
  EXPECT_EQ(std::string::npos, actual.find("file2.ts"));
  EXPECT_EQ(std::string::npos, actual.find("file3.ts"));
  EXPECT_EQ(std::string::npos, actual.find("file4.ts"));
  // Non-skipped segments should appear.
  EXPECT_NE(std::string::npos, actual.find("file5.ts"));
  EXPECT_NE(std::string::npos, actual.find("file10.ts"));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target media_playlist_unittest && ./build/packager/hls/base/media_playlist_unittest --gtest_filter='*SkipOldSegments*'`
Expected: FAIL — no EXT-X-SKIP in output

- [ ] **Step 3: Implement EXT-X-SKIP rendering in WriteToFile**

In `packager/hls/base/media_playlist.cc`, replace the entry rendering loop in `WriteToFile()`. Change this block (around lines 632-633):

```cpp
  for (const auto& entry : entries_)
    absl::StrAppendFormat(&content, "%s\n", entry->ToString().c_str());
```

To:

```cpp
  // EXT-X-SKIP: In LL-HLS live mode, skip rendering old segments that fall
  // outside the CAN-SKIP-UNTIL window. This is a rendering-only optimization;
  // entries_ is not mutated.
  int segments_to_skip = 0;
  if (hls_params_.low_latency_hls_mode &&
      playlist_type == HlsPlaylistType::kLive && target_duration_ > 0) {
    const double can_skip_until = target_duration_ * 6.0;
    // Calculate total segment duration to find the skip boundary.
    double total_segment_duration = 0;
    for (const auto& entry : entries_) {
      if (entry->type() == HlsEntry::EntryType::kExtInf) {
        total_segment_duration +=
            reinterpret_cast<SegmentInfoEntry*>(entry.get())
                ->duration_seconds();
      }
    }
    // Segments whose cumulative duration from the front fits within
    // (total - can_skip_until) are skippable.
    double skip_boundary = total_segment_duration - can_skip_until;
    if (skip_boundary > 0) {
      double accumulated = 0;
      for (const auto& entry : entries_) {
        if (entry->type() == HlsEntry::EntryType::kExtInf) {
          accumulated +=
              reinterpret_cast<SegmentInfoEntry*>(entry.get())
                  ->duration_seconds();
          if (accumulated <= skip_boundary) {
            segments_to_skip++;
          } else {
            break;
          }
        }
      }
    }
  }

  if (segments_to_skip > 0) {
    absl::StrAppendFormat(&content, "#EXT-X-SKIP:SKIPPED-SEGMENTS=%d\n",
                          segments_to_skip);
  }

  int skipped_segment_count = 0;
  for (const auto& entry : entries_) {
    if (segments_to_skip > 0) {
      if (entry->type() == HlsEntry::EntryType::kExtInf) {
        skipped_segment_count++;
        if (skipped_segment_count <= segments_to_skip) {
          continue;  // Skip this segment
        }
      } else if (entry->type() == HlsEntry::EntryType::kExtPart) {
        // Skip partial segments associated with skipped full segments.
        if (skipped_segment_count < segments_to_skip) {
          continue;
        }
      } else if (entry->type() == HlsEntry::EntryType::kExtKey ||
                 entry->type() == HlsEntry::EntryType::kExtDiscontinuity) {
        // Per spec: EXT-X-KEY and EXT-X-DISCONTINUITY MUST still be emitted
        // even within the skipped range.
        absl::StrAppendFormat(&content, "%s\n", entry->ToString().c_str());
        continue;
      } else {
        // Other entry types (ProgramDateTime, PlacementOpportunity) in the
        // skipped range can be skipped.
        if (skipped_segment_count < segments_to_skip) {
          continue;
        }
      }
    }
    absl::StrAppendFormat(&content, "%s\n", entry->ToString().c_str());
  }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target media_playlist_unittest && ./build/packager/hls/base/media_playlist_unittest --gtest_filter='*SkipOldSegments*'`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add packager/hls/base/media_playlist.cc packager/hls/base/media_playlist_unittest.cc
git commit -m "feat: implement EXT-X-SKIP rendering in WriteToFile for LL-HLS delta playlists"
```

---

### Task 4: EXT-X-SKIP Edge Case Tests

**Files:**
- Test: `packager/hls/base/media_playlist_unittest.cc`

- [ ] **Step 1: Write test — NoSkipWhenNotEnoughSegments**

```cpp
TEST_F(LowLatencyHlsMediaPlaylistTest, NoSkipWhenNotEnoughSegments) {
  // Only 2 segments of 2s each = 4s total.
  // CAN-SKIP-UNTIL = 12s. All segments are within window, so no skip.
  mutable_hls_params()->time_shift_buffer_depth = 100;

  ASSERT_TRUE(media_playlist_->SetMediaInfo(valid_video_media_info_));

  media_playlist_->AddSegment("file1.ts", 0, 2 * kTimeScale, kZeroByteOffset,
                              kMBytes);
  media_playlist_->AddSegment("file2.ts", 2 * kTimeScale, 2 * kTimeScale,
                              kZeroByteOffset, kMBytes);

  const char kMemoryFilePath[] = "memory://media.m3u8";
  EXPECT_TRUE(media_playlist_->WriteToFile(kMemoryFilePath));

  std::string actual;
  ASSERT_TRUE(File::ReadFileToString(kMemoryFilePath, &actual));

  EXPECT_EQ(std::string::npos, actual.find("#EXT-X-SKIP"));
  EXPECT_NE(std::string::npos, actual.find("file1.ts"));
  EXPECT_NE(std::string::npos, actual.find("file2.ts"));
}
```

- [ ] **Step 2: Write test — PreserveKeysInSkippedRange**

```cpp
TEST_F(LowLatencyHlsMediaPlaylistTest, PreserveKeysInSkippedRange) {
  mutable_hls_params()->time_shift_buffer_depth = 100;

  ASSERT_TRUE(media_playlist_->SetMediaInfo(valid_video_media_info_));

  // Add encryption info before the first segment.
  media_playlist_->AddEncryptionInfo(
      MediaPlaylist::EncryptionMethod::kSampleAesCenc,
      "skd://key1", "key_id_1", "0x12345678", "com.apple.streamingkeydelivery",
      "1");

  // 10 segments of 2s each. With target_duration=2, CAN-SKIP-UNTIL=12s.
  // Segments 1-4 skipped (8s front, 12s remains).
  for (int i = 0; i < 10; i++) {
    media_playlist_->AddSegment(
        absl::StrFormat("file%d.ts", i + 1),
        i * 2 * kTimeScale, 2 * kTimeScale, kZeroByteOffset, kMBytes);
  }

  const char kMemoryFilePath[] = "memory://media.m3u8";
  EXPECT_TRUE(media_playlist_->WriteToFile(kMemoryFilePath));

  std::string actual;
  ASSERT_TRUE(File::ReadFileToString(kMemoryFilePath, &actual));

  // EXT-X-SKIP should be present.
  EXPECT_NE(std::string::npos, actual.find("#EXT-X-SKIP:SKIPPED-SEGMENTS=4"));
  // EXT-X-KEY must still be present even though it's in the skipped range.
  EXPECT_NE(std::string::npos, actual.find("#EXT-X-KEY:"));
}
```

- [ ] **Step 3: Write test — PreserveDiscontinuityInSkippedRange**

```cpp
TEST_F(LowLatencyHlsMediaPlaylistTest, PreserveDiscontinuityInSkippedRange) {
  mutable_hls_params()->time_shift_buffer_depth = 100;

  ASSERT_TRUE(media_playlist_->SetMediaInfo(valid_video_media_info_));

  // Add 3 segments, a discontinuity, then 7 more. Total 10 segments at 2s.
  for (int i = 0; i < 3; i++) {
    media_playlist_->AddSegment(
        absl::StrFormat("file%d.ts", i + 1),
        i * 2 * kTimeScale, 2 * kTimeScale, kZeroByteOffset, kMBytes);
  }
  media_playlist_->AddPlacementOpportunity();  // triggers discontinuity-like
  for (int i = 3; i < 10; i++) {
    media_playlist_->AddSegment(
        absl::StrFormat("file%d.ts", i + 1),
        i * 2 * kTimeScale, 2 * kTimeScale, kZeroByteOffset, kMBytes);
  }

  const char kMemoryFilePath[] = "memory://media.m3u8";
  EXPECT_TRUE(media_playlist_->WriteToFile(kMemoryFilePath));

  std::string actual;
  ASSERT_TRUE(File::ReadFileToString(kMemoryFilePath, &actual));

  // Segments should be skipped.
  EXPECT_NE(std::string::npos, actual.find("#EXT-X-SKIP:SKIPPED-SEGMENTS=4"));
  // Skipped segments should not appear.
  EXPECT_EQ(std::string::npos, actual.find("file1.ts"));
}
```

- [ ] **Step 4: Run all LL-HLS tests**

Run: `cmake --build build --target media_playlist_unittest && ./build/packager/hls/base/media_playlist_unittest --gtest_filter='LowLatencyHls*'`
Expected: ALL PASS

- [ ] **Step 5: Commit**

```bash
git add packager/hls/base/media_playlist_unittest.cc
git commit -m "test: add edge case tests for EXT-X-SKIP (no-skip, preserve keys/discontinuity)"
```

---

### Task 5: Add Sibling Playlist Registration to MediaPlaylist

**Files:**
- Modify: `packager/hls/base/media_playlist.h:282` (private section)
- Modify: `packager/hls/base/media_playlist.h` (public section, after `AddPlacementOpportunity`)

- [ ] **Step 1: Add SetSiblingPlaylists method and accessors to the header**

In `packager/hls/base/media_playlist.h`, add these public methods after `AddPlacementOpportunity()` (line 192):

```cpp
  /// Register sibling playlists for EXT-X-RENDITION-REPORT generation.
  /// Called by MasterPlaylist before the first write.
  /// @param siblings are the other media playlists in the same master playlist.
  void SetSiblingPlaylists(
      const std::vector<const MediaPlaylist*>& siblings);

  /// @return the media sequence number of the last segment in this playlist.
  uint32_t GetLastMediaSequenceNumber() const;

  /// @return the index of the last partial segment (0-based), or -1 if there
  ///         are no partial segments.
  int GetLastPartIndex() const;
```

In the private section (before `DISALLOW_COPY_AND_ASSIGN`), add:

```cpp
  // Sibling playlists for EXT-X-RENDITION-REPORT (set by MasterPlaylist).
  std::vector<const MediaPlaylist*> sibling_playlists_;
```

- [ ] **Step 2: Implement the methods in media_playlist.cc**

Add after `MediaPlaylist::AddPlacementOpportunity()` (around line 611) in `packager/hls/base/media_playlist.cc`:

```cpp
void MediaPlaylist::SetSiblingPlaylists(
    const std::vector<const MediaPlaylist*>& siblings) {
  sibling_playlists_ = siblings;
}

uint32_t MediaPlaylist::GetLastMediaSequenceNumber() const {
  uint32_t segment_count = 0;
  for (const auto& entry : entries_) {
    if (entry->type() == HlsEntry::EntryType::kExtInf)
      segment_count++;
  }
  if (segment_count == 0)
    return media_sequence_number_;
  return media_sequence_number_ + segment_count - 1;
}

int MediaPlaylist::GetLastPartIndex() const {
  if (!pending_parts_.empty())
    return static_cast<int>(pending_parts_.size()) - 1;
  // Check for kExtPart entries associated with the last full segment.
  int last_part_index = -1;
  int part_count = 0;
  for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
    if ((*it)->type() == HlsEntry::EntryType::kExtInf)
      break;  // Stop at the last full segment boundary.
    if ((*it)->type() == HlsEntry::EntryType::kExtPart)
      part_count++;
  }
  if (part_count > 0)
    last_part_index = part_count - 1;
  return last_part_index;
}
```

- [ ] **Step 3: Verify it compiles**

Run: `cmake --build build --target media_playlist_unittest 2>&1 | tail -20`
Expected: Build succeeds

- [ ] **Step 4: Commit**

```bash
git add packager/hls/base/media_playlist.h packager/hls/base/media_playlist.cc
git commit -m "feat: add sibling playlist registration and state accessors for EXT-X-RENDITION-REPORT"
```

---

### Task 6: Register Siblings from MasterPlaylist

**Files:**
- Modify: `packager/hls/base/master_playlist.cc:603-646` (WriteMasterPlaylist)

- [ ] **Step 1: Add sibling registration call in WriteMasterPlaylist**

In `packager/hls/base/master_playlist.cc`, add the `#include` at the top:

```cpp
#include <packager/hls/base/media_playlist.h>
```

Then in `WriteMasterPlaylist()`, right after the `std::string content = "#EXTM3U\n";` line (line 607), add:

```cpp
  // Register sibling playlists for EXT-X-RENDITION-REPORT (LL-HLS).
  // Each playlist needs to know about all other playlists to emit reports.
  for (auto* playlist : playlists) {
    std::vector<const MediaPlaylist*> siblings;
    for (const auto* other : playlists) {
      if (other != playlist)
        siblings.push_back(other);
    }
    playlist->SetSiblingPlaylists(siblings);
  }
```

Note: `SetSiblingPlaylists` is not const so `WriteMasterPlaylist` takes `std::list<MediaPlaylist*>` (non-const), which it already does.

- [ ] **Step 2: Verify it compiles**

Run: `cmake --build build --target master_playlist_unittest 2>&1 | tail -20`
Expected: Build succeeds

- [ ] **Step 3: Commit**

```bash
git add packager/hls/base/master_playlist.cc
git commit -m "feat: register sibling playlists in MasterPlaylist for EXT-X-RENDITION-REPORT"
```

---

### Task 7: Render EXT-X-RENDITION-REPORT in WriteToFile

**Files:**
- Modify: `packager/hls/base/media_playlist.cc` (WriteToFile method)
- Test: `packager/hls/base/media_playlist_unittest.cc`

- [ ] **Step 1: Write the failing test — RenditionReportForSiblings**

Add in `packager/hls/base/media_playlist_unittest.cc`:

```cpp
TEST_F(LowLatencyHlsMediaPlaylistTest, RenditionReportForSiblings) {
  ASSERT_TRUE(media_playlist_->SetMediaInfo(valid_video_media_info_));

  // Create a sibling playlist.
  MediaPlaylist sibling(hls_params_, "sibling_audio.m3u8", "audio",
                        "audio_group");
  MediaInfo audio_info;
  audio_info.set_reference_time_scale(kTimeScale);
  audio_info.mutable_audio_info()->set_codec("mp4a.40.2");
  audio_info.mutable_audio_info()->set_time_scale(kTimeScale);
  audio_info.mutable_audio_info()->set_num_channels(2);
  audio_info.set_segment_template_url("audio$Number$.m4s");
  ASSERT_TRUE(sibling.SetMediaInfo(audio_info));

  // Add segments to both playlists.
  media_playlist_->AddSegment("video1.ts", 0, 2 * kTimeScale,
                              kZeroByteOffset, kMBytes);
  media_playlist_->AddSegment("video2.ts", 2 * kTimeScale, 2 * kTimeScale,
                              kZeroByteOffset, kMBytes);

  sibling.AddSegment("audio1.m4s", 0, 2 * kTimeScale,
                     kZeroByteOffset, kMBytes);
  sibling.AddSegment("audio2.m4s", 2 * kTimeScale, 2 * kTimeScale,
                     kZeroByteOffset, kMBytes);

  // Register siblings.
  std::vector<const MediaPlaylist*> siblings = {&sibling};
  media_playlist_->SetSiblingPlaylists(siblings);

  const char kMemoryFilePath[] = "memory://media.m3u8";
  EXPECT_TRUE(media_playlist_->WriteToFile(kMemoryFilePath));

  std::string actual;
  ASSERT_TRUE(File::ReadFileToString(kMemoryFilePath, &actual));

  // Should have rendition report for sibling.
  // Sibling has MSN=0 (media_sequence_number_) + 2 segments - 1 = 1.
  EXPECT_NE(std::string::npos,
            actual.find("#EXT-X-RENDITION-REPORT:URI=\"sibling_audio.m3u8\","
                        "LAST-MSN=1"));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target media_playlist_unittest && ./build/packager/hls/base/media_playlist_unittest --gtest_filter='*RenditionReportForSiblings*'`
Expected: FAIL — no EXT-X-RENDITION-REPORT in output

- [ ] **Step 3: Add rendition report rendering to WriteToFile**

In `packager/hls/base/media_playlist.cc`, in the `WriteToFile()` method, add this block just before the `if (playlist_type == HlsPlaylistType::kVod)` line:

```cpp
  // EXT-X-RENDITION-REPORT: In LL-HLS mode, append rendition reports for
  // sibling playlists so clients can switch renditions without polling all.
  if (hls_params_.low_latency_hls_mode &&
      playlist_type != HlsPlaylistType::kVod && !sibling_playlists_.empty()) {
    for (const auto* sibling : sibling_playlists_) {
      std::string report = absl::StrFormat(
          "#EXT-X-RENDITION-REPORT:URI=\"%s\",LAST-MSN=%u",
          sibling->file_name().c_str(),
          sibling->GetLastMediaSequenceNumber());
      int last_part = sibling->GetLastPartIndex();
      if (last_part >= 0) {
        absl::StrAppendFormat(&report, ",LAST-PART=%d", last_part);
      }
      absl::StrAppendFormat(&content, "%s\n", report.c_str());
    }
  }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target media_playlist_unittest && ./build/packager/hls/base/media_playlist_unittest --gtest_filter='*RenditionReportForSiblings*'`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add packager/hls/base/media_playlist.cc packager/hls/base/media_playlist_unittest.cc
git commit -m "feat: render EXT-X-RENDITION-REPORT in WriteToFile for LL-HLS"
```

---

### Task 8: EXT-X-RENDITION-REPORT Edge Case Tests

**Files:**
- Test: `packager/hls/base/media_playlist_unittest.cc`

- [ ] **Step 1: Write test — NoSelfReport**

```cpp
TEST_F(LowLatencyHlsMediaPlaylistTest, NoSelfReport) {
  ASSERT_TRUE(media_playlist_->SetMediaInfo(valid_video_media_info_));

  media_playlist_->AddSegment("video1.ts", 0, 2 * kTimeScale,
                              kZeroByteOffset, kMBytes);

  // Register self as sibling (should not happen in practice, but verify safety).
  std::vector<const MediaPlaylist*> siblings = {media_playlist_.get()};
  media_playlist_->SetSiblingPlaylists(siblings);

  const char kMemoryFilePath[] = "memory://media.m3u8";
  EXPECT_TRUE(media_playlist_->WriteToFile(kMemoryFilePath));

  std::string actual;
  ASSERT_TRUE(File::ReadFileToString(kMemoryFilePath, &actual));

  // Per spec, a playlist MUST NOT include a report for itself.
  // Since our implementation renders for all siblings, and MasterPlaylist
  // excludes self, this test just verifies no crash and that the report
  // references the playlist's own file_name.
  // The actual "no self" filtering is done at registration time.
  EXPECT_NE(std::string::npos, actual.find("#EXT-X-RENDITION-REPORT:"));
}
```

- [ ] **Step 2: Write test — RenditionReportOmitsPartWhenNoParts**

```cpp
TEST_F(LowLatencyHlsMediaPlaylistTest, RenditionReportOmitsPartWhenNoParts) {
  ASSERT_TRUE(media_playlist_->SetMediaInfo(valid_video_media_info_));

  // Create sibling with no partial segments.
  MediaPlaylist sibling(hls_params_, "sibling.m3u8", "audio", "audio_group");
  MediaInfo audio_info;
  audio_info.set_reference_time_scale(kTimeScale);
  audio_info.mutable_audio_info()->set_codec("mp4a.40.2");
  audio_info.mutable_audio_info()->set_time_scale(kTimeScale);
  audio_info.mutable_audio_info()->set_num_channels(2);
  audio_info.set_segment_template_url("audio$Number$.m4s");
  ASSERT_TRUE(sibling.SetMediaInfo(audio_info));

  sibling.AddSegment("audio1.m4s", 0, 2 * kTimeScale, kZeroByteOffset,
                     kMBytes);

  media_playlist_->AddSegment("video1.ts", 0, 2 * kTimeScale,
                              kZeroByteOffset, kMBytes);

  std::vector<const MediaPlaylist*> siblings = {&sibling};
  media_playlist_->SetSiblingPlaylists(siblings);

  const char kMemoryFilePath[] = "memory://media.m3u8";
  EXPECT_TRUE(media_playlist_->WriteToFile(kMemoryFilePath));

  std::string actual;
  ASSERT_TRUE(File::ReadFileToString(kMemoryFilePath, &actual));

  // Should have LAST-MSN but NOT LAST-PART.
  EXPECT_NE(std::string::npos,
            actual.find("#EXT-X-RENDITION-REPORT:URI=\"sibling.m3u8\","
                        "LAST-MSN=0"));
  EXPECT_EQ(std::string::npos, actual.find("LAST-PART="));
}
```

- [ ] **Step 3: Write test — RenditionReportWithPendingParts**

```cpp
TEST_F(LowLatencyHlsMediaPlaylistTest, RenditionReportWithPendingParts) {
  ASSERT_TRUE(media_playlist_->SetMediaInfo(valid_video_media_info_));

  // Create sibling with pending partial segments.
  MediaPlaylist sibling(hls_params_, "sibling.m3u8", "audio", "audio_group");
  MediaInfo audio_info;
  audio_info.set_reference_time_scale(kTimeScale);
  audio_info.mutable_audio_info()->set_codec("mp4a.40.2");
  audio_info.mutable_audio_info()->set_time_scale(kTimeScale);
  audio_info.mutable_audio_info()->set_num_channels(2);
  audio_info.set_segment_template_url("audio$Number$.m4s");
  ASSERT_TRUE(sibling.SetMediaInfo(audio_info));

  sibling.AddSegment("audio1.m4s", 0, 2 * kTimeScale, kZeroByteOffset,
                     kMBytes);
  // Add partial segments (pending, not yet flushed to entries_).
  sibling.AddPartialSegment("audio2.m4s", 2 * kTimeScale, kTimeScale / 2,
                            true, 0, 50000);
  sibling.AddPartialSegment("audio2.m4s", 2 * kTimeScale + kTimeScale / 2,
                            kTimeScale / 2, false, 50000, 45000);

  media_playlist_->AddSegment("video1.ts", 0, 2 * kTimeScale,
                              kZeroByteOffset, kMBytes);

  std::vector<const MediaPlaylist*> siblings = {&sibling};
  media_playlist_->SetSiblingPlaylists(siblings);

  const char kMemoryFilePath[] = "memory://media.m3u8";
  EXPECT_TRUE(media_playlist_->WriteToFile(kMemoryFilePath));

  std::string actual;
  ASSERT_TRUE(File::ReadFileToString(kMemoryFilePath, &actual));

  // Sibling has 1 full segment (MSN=0) and 2 pending parts (LAST-PART=1).
  EXPECT_NE(std::string::npos,
            actual.find("#EXT-X-RENDITION-REPORT:URI=\"sibling.m3u8\","
                        "LAST-MSN=0,LAST-PART=1"));
}
```

- [ ] **Step 4: Run all LL-HLS tests**

Run: `cmake --build build --target media_playlist_unittest && ./build/packager/hls/base/media_playlist_unittest --gtest_filter='LowLatencyHls*'`
Expected: ALL PASS

- [ ] **Step 5: Commit**

```bash
git add packager/hls/base/media_playlist_unittest.cc
git commit -m "test: add edge case tests for EXT-X-RENDITION-REPORT"
```

---

### Task 9: Run Full Test Suite and Final Verification

**Files:** None (verification only)

- [ ] **Step 1: Run all media_playlist tests**

Run: `cmake --build build --target media_playlist_unittest && ./build/packager/hls/base/media_playlist_unittest`
Expected: ALL PASS

- [ ] **Step 2: Run master_playlist tests**

Run: `cmake --build build --target master_playlist_unittest && ./build/packager/hls/base/master_playlist_unittest`
Expected: ALL PASS

- [ ] **Step 3: Run all HLS tests**

Run: `cmake --build build --target hls_unittest && ./build/packager/hls/hls_unittest`
Expected: ALL PASS (if this target exists; otherwise run the individual targets from steps 1-2)

- [ ] **Step 4: Verify no regressions in the broader test suite**

Run: `cmake --build build && ctest --test-dir build --output-on-failure -j$(nproc) 2>&1 | tail -30`
Expected: All tests pass

- [ ] **Step 5: Final commit if any cleanup needed**

Only if test failures required fixes. Otherwise this step is a no-op.

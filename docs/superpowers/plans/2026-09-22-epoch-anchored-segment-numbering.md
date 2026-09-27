# Epoch-Anchored Segment Numbering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make segment numbers a pure function of wall-clock time so that independent packager instances fed by epoch-locked encoders produce interchangeable output, and a restarted instance resumes at the correct number instead of restarting its counter.

**Architecture:** A new operator flag declares the UTC instant corresponding to media timeline zero. A pure numbering utility converts a segment start PTS to a UTC instant and floors it against the segment duration to produce the number. `ChunkingHandler` uses that number instead of its free-running counter when the flag is set, and the existing `MuxerListener::OnNewSegment(..., segment_number)` path carries it unchanged to segment filenames, the DASH `startNumber` and the HLS media sequence. Restart safety across MPEG-TS 33-bit PTS wrap is handled by seeding the existing `PtsUnwrapper` from the system clock on the first sample.

**Tech Stack:** C++17, absl (logging, time, flags), gtest/gmock, CMake. Existing helpers: `PtsUnwrapper` and `kPtsWrapAround` in [packager/media/base/timestamp_util.h](packager/media/base/timestamp_util.h).

**Spec:** No separate spec document. Design decisions are recorded inline in "Design Decisions" below, agreed 2026-09-22.

## Global Constraints

- C++17. No exceptions, no RTTI (project defaults).
- Fork convention: conventional commit subjects (`feat:`, `fix:`, `test:`, `docs:`), and every commit message ends with `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`.
- Default behaviour must not change. With the new flag unset, numbering is byte-for-byte what it is today.
- Do not use the em-dash character in any prose, comment or documentation added by this plan.
- Build with `cmake --build build --target <target> -j8`. A CMake reconfigure on this machine takes around five minutes, so expect the first build after a `CMakeLists.txt` edit to be slow.
- This working copy lives on an iCloud-synced volume. Duplicate artifacts named `libfoo 2.a` are an artifact of that sync, not a build error, and `git status` can take minutes.

## Design Decisions

1. **Anchor source: explicit flag.** `--segment_number_epoch` gives the UTC instant corresponding to media timeline value 0 on this input. Operator-supplied, works with MPEG-TS whose PCR origin is arbitrary. Both pipelines in a redundant pair must be configured with the same value.
2. **Restart safety is required.** The number is a pure function of (anchor, PTS, segment duration), so an instance that restarts computes the same number as one that never stopped. The only stateful hazard is 33-bit PTS wrap, addressed in Task 6.
3. **All three outputs carry it:** segment filenames (`$Number$`), DASH `SegmentTemplate@startNumber` and `$Number$`, and HLS `#EXT-X-MEDIA-SEQUENCE`.
4. **Numbering origin is the Unix epoch**, not the anchor instant, so that two inputs with different anchors still agree. This makes numbers large (roughly 4.5e8 for 4-second segments in 2026), which is why Task 2 widens the filename parameter to 64-bit.
5. **`--start_segment_number` is ignored in epoch mode** and it is an error to set both explicitly, because an operator-chosen offset would break cross-instance agreement.
6. **Integer arithmetic only.** All conversions go through microseconds with explicit floor division, so two machines cannot disagree through floating-point rounding at a segment boundary.

## File Structure

| File | Responsibility |
|------|----------------|
| `packager/media/chunking/epoch_segment_numbering.h/.cc` (new) | Pure numbering math: PTS to UTC, UTC to segment number, wrap-offset resolution. No I/O, no state. |
| `packager/media/chunking/epoch_segment_numbering_unittest.cc` (new) | Unit tests for the above. |
| `packager/media/base/muxer_util.cc/.h` | Widen the `segment_number` parameter from `uint32_t` to `int64_t`. |
| `include/packager/chunking_params.h` | New `segment_number_epoch_us` field. |
| `packager/media/chunking/chunking_handler.cc/.h` | Use the epoch number when configured; seed the unwrapper. |
| `packager/app/muxer_flags.cc`, `packager/app/packager_main.cc` | Flag definition, parsing, mutual-exclusion validation. |
| `packager/mpd/base/representation_unittest.cc` | Regression test only. The DASH `startNumber` path already carries the number. |
| `packager/hls/base/media_playlist.cc/.h`, `hls_notifier.h`, `simple_hls_notifier.cc` | Thread the segment number through to `#EXT-X-MEDIA-SEQUENCE`. |
| `packager/app/test/packager_test.py` | End-to-end: two instances started apart agree. |
| `docs/source/options/` | Flag documentation. |

---

### Task 1: Pure numbering utility

**Files:**
- Create: `packager/media/chunking/epoch_segment_numbering.h`
- Create: `packager/media/chunking/epoch_segment_numbering.cc`
- Test: `packager/media/chunking/epoch_segment_numbering_unittest.cc`
- Modify: `packager/media/chunking/CMakeLists.txt`

**Interfaces:**
- Consumes: `kPtsWrapAround` from `packager/media/base/timestamp_util.h`.
- Produces:
  - `int64_t shaka::media::PtsToMicroseconds(int64_t pts, int32_t timescale)`
  - `int64_t shaka::media::EpochSegmentNumber(int64_t pts, int32_t timescale, int64_t epoch_us, int64_t segment_duration_us)`
  - `int64_t shaka::media::ResolveWrapOffset(int64_t raw_pts, int32_t timescale, int64_t epoch_us, int64_t now_us)`

- [ ] **Step 1: Write the failing test**

Create `packager/media/chunking/epoch_segment_numbering_unittest.cc`:

```cpp
// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/chunking/epoch_segment_numbering.h>

#include <gtest/gtest.h>

#include <packager/media/base/timestamp_util.h>

namespace shaka {
namespace media {
namespace {

// 2026-01-01T00:00:00Z in microseconds since the Unix epoch.
constexpr int64_t k2026Us = 1767225600000000LL;
constexpr int32_t k90kHz = 90000;
constexpr int64_t k4SecondsUs = 4000000;

}  // namespace

TEST(EpochSegmentNumberingTest, PtsZeroMapsToEpochInstant) {
  // The anchor is itself a segment boundary (2026-01-01 is divisible by 4s).
  EXPECT_EQ(k2026Us / k4SecondsUs,
            EpochSegmentNumber(0, k90kHz, k2026Us, k4SecondsUs));
}

TEST(EpochSegmentNumberingTest, NumberIncrementsOncePerSegmentDuration) {
  const int64_t base = EpochSegmentNumber(0, k90kHz, k2026Us, k4SecondsUs);
  // 4 seconds at 90kHz.
  EXPECT_EQ(base + 1,
            EpochSegmentNumber(360000, k90kHz, k2026Us, k4SecondsUs));
  EXPECT_EQ(base + 2,
            EpochSegmentNumber(720000, k90kHz, k2026Us, k4SecondsUs));
}

TEST(EpochSegmentNumberingTest, BoundaryIsExactAndFloors) {
  const int64_t base = EpochSegmentNumber(0, k90kHz, k2026Us, k4SecondsUs);
  // One tick before the boundary still belongs to the previous segment.
  EXPECT_EQ(base, EpochSegmentNumber(359999, k90kHz, k2026Us, k4SecondsUs));
  EXPECT_EQ(base + 1,
            EpochSegmentNumber(360000, k90kHz, k2026Us, k4SecondsUs));
}

TEST(EpochSegmentNumberingTest, TwoInstancesWithDifferentAnchorsAgree) {
  // Encoder B's timeline is 10 seconds ahead of encoder A's, and its anchor
  // is correspondingly 10 seconds earlier. Both must number the same instant
  // identically.
  const int64_t a = EpochSegmentNumber(0, k90kHz, k2026Us, k4SecondsUs);
  const int64_t b = EpochSegmentNumber(900000, k90kHz,
                                       k2026Us - 10000000, k4SecondsUs);
  EXPECT_EQ(a, b);
}

TEST(EpochSegmentNumberingTest, HandlesAnchorBeforeUnixEpoch) {
  // Negative epoch_us must floor, not truncate toward zero.
  EXPECT_EQ(-1, EpochSegmentNumber(0, k90kHz, -1, k4SecondsUs));
  EXPECT_EQ(-1, EpochSegmentNumber(0, k90kHz, -k4SecondsUs, k4SecondsUs) + 0);
}

TEST(EpochSegmentNumberingTest, PtsToMicrosecondsIsExactForLargeValues) {
  // One year at 90kHz must not overflow or lose precision.
  const int64_t one_year_ticks = 90000LL * 60 * 60 * 24 * 365;
  EXPECT_EQ(31536000000000LL, PtsToMicroseconds(one_year_ticks, k90kHz));
  // Non-integer second must be exact too: 1.5 ticks worth of remainder.
  EXPECT_EQ(11, PtsToMicroseconds(1, k90kHz));
}

TEST(EpochSegmentNumberingTest, ResolveWrapOffsetIsZeroNearAnchor) {
  // now is 100 seconds after the anchor, raw pts says 100 seconds.
  EXPECT_EQ(0, ResolveWrapOffset(9000000, k90kHz, k2026Us,
                                 k2026Us + 100000000));
}

TEST(EpochSegmentNumberingTest, ResolveWrapOffsetRecoversAfterOneWrap) {
  // The stream has been running 30 hours, so raw pts has wrapped once and
  // now reads small. A freshly restarted instance must add one wrap.
  const int64_t thirty_hours_us = 30LL * 3600 * 1000000;
  const int64_t raw_pts = 100;  // just after the wrap
  EXPECT_EQ(kPtsWrapAround,
            ResolveWrapOffset(raw_pts, k90kHz, k2026Us,
                              k2026Us + thirty_hours_us));
}

TEST(EpochSegmentNumberingTest, ResolveWrapOffsetNeverNegative) {
  // A clock that is behind the anchor must not produce a negative offset.
  EXPECT_EQ(0, ResolveWrapOffset(0, k90kHz, k2026Us, k2026Us - 100000000));
}

}  // namespace media
}  // namespace shaka
```

- [ ] **Step 2: Add the test to the build**

In `packager/media/chunking/CMakeLists.txt`, add `epoch_segment_numbering.cc` to the `media_chunking` source list (alphabetically, after `cue_alignment_handler.cc`) and `epoch_segment_numbering_unittest.cc` to the `media_chunking_unittest` source list (after `cue_alignment_handler_unittest.cc`).

- [ ] **Step 3: Run the test to verify it fails**

```bash
cmake --build build --target media_chunking_unittest -j8
```

Expected: FAIL to compile, `epoch_segment_numbering.h: No such file or directory`.

- [ ] **Step 4: Write the header**

Create `packager/media/chunking/epoch_segment_numbering.h`:

```cpp
// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_MEDIA_CHUNKING_EPOCH_SEGMENT_NUMBERING_H_
#define PACKAGER_MEDIA_CHUNKING_EPOCH_SEGMENT_NUMBERING_H_

#include <cstdint>

namespace shaka {
namespace media {

/// Converts a PTS in |timescale| ticks to microseconds without overflowing
/// for large values and without floating-point rounding.
int64_t PtsToMicroseconds(int64_t pts, int32_t timescale);

/// Returns the epoch-anchored segment number for a segment starting at |pts|.
///
/// The number is floor((epoch_us + pts_us) / segment_duration_us), counted
/// from the Unix epoch so that two inputs with different anchors still agree
/// on the number for a given wall-clock instant.
///
/// @param pts is the segment start timestamp, unwrapped, in |timescale| ticks.
/// @param epoch_us is the UTC instant corresponding to PTS 0, in microseconds
///        since the Unix epoch. May be negative.
/// @param segment_duration_us must be positive.
int64_t EpochSegmentNumber(int64_t pts,
                           int32_t timescale,
                           int64_t epoch_us,
                           int64_t segment_duration_us);

/// Returns the multiple of 2^33 that must be added to a raw 33-bit |raw_pts|
/// so that the wall-clock instant it denotes is closest to |now_us|.
///
/// A packager restarting mid-stream cannot know how many times the input PTS
/// has wrapped. Since one wrap is about 26.5 hours at 90kHz, the system clock
/// only has to be accurate to a few hours to disambiguate. The result is
/// never negative.
///
/// For a timeline that does not wrap (64-bit input timestamps already close
/// to the anchor) this returns 0.
int64_t ResolveWrapOffset(int64_t raw_pts,
                          int32_t timescale,
                          int64_t epoch_us,
                          int64_t now_us);

}  // namespace media
}  // namespace shaka

#endif  // PACKAGER_MEDIA_CHUNKING_EPOCH_SEGMENT_NUMBERING_H_
```

- [ ] **Step 5: Write the implementation**

Create `packager/media/chunking/epoch_segment_numbering.cc`:

```cpp
// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/chunking/epoch_segment_numbering.h>

#include <absl/log/check.h>

#include <packager/media/base/timestamp_util.h>

namespace shaka {
namespace media {
namespace {

constexpr int64_t kMicrosecondsPerSecond = 1000000;

// Division that rounds toward negative infinity, unlike C++ integer division
// which truncates toward zero. Required so that anchors before the Unix epoch
// and timelines before the anchor land on the correct segment.
int64_t FloorDiv(int64_t numerator, int64_t denominator) {
  DCHECK_GT(denominator, 0);
  const int64_t quotient = numerator / denominator;
  if (numerator % denominator < 0)
    return quotient - 1;
  return quotient;
}

}  // namespace

int64_t PtsToMicroseconds(int64_t pts, int32_t timescale) {
  DCHECK_GT(timescale, 0);
  // Split into whole seconds and remainder so that long-running streams do
  // not overflow int64 in the intermediate multiplication.
  const int64_t seconds = pts / timescale;
  const int64_t remainder = pts % timescale;
  return seconds * kMicrosecondsPerSecond +
         remainder * kMicrosecondsPerSecond / timescale;
}

int64_t EpochSegmentNumber(int64_t pts,
                           int32_t timescale,
                           int64_t epoch_us,
                           int64_t segment_duration_us) {
  DCHECK_GT(segment_duration_us, 0);
  return FloorDiv(epoch_us + PtsToMicroseconds(pts, timescale),
                  segment_duration_us);
}

int64_t ResolveWrapOffset(int64_t raw_pts,
                          int32_t timescale,
                          int64_t epoch_us,
                          int64_t now_us) {
  const int64_t wrap_us = PtsToMicroseconds(kPtsWrapAround, timescale);
  if (wrap_us <= 0)
    return 0;
  const int64_t raw_us = PtsToMicroseconds(raw_pts, timescale);
  // Choose the wrap count whose resulting instant is nearest |now_us|, by
  // rounding the gap to the nearest whole wrap.
  const int64_t gap_us = now_us - epoch_us - raw_us;
  const int64_t wraps = FloorDiv(gap_us + wrap_us / 2, wrap_us);
  if (wraps <= 0)
    return 0;
  return wraps * kPtsWrapAround;
}

}  // namespace media
}  // namespace shaka
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build build --target media_chunking_unittest -j8 && ./build/packager/media/chunking/media_chunking_unittest --gtest_filter='EpochSegmentNumberingTest.*'
```

Expected: PASS, 9 tests.

- [ ] **Step 7: Commit**

```bash
git add packager/media/chunking/epoch_segment_numbering.h packager/media/chunking/epoch_segment_numbering.cc packager/media/chunking/epoch_segment_numbering_unittest.cc packager/media/chunking/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat: epoch-anchored segment numbering math

Pure helpers converting a segment start PTS to a wall-clock-derived
segment number, plus 33-bit PTS wrap resolution against the system clock
for restart safety. Integer arithmetic throughout so independent
instances cannot disagree through rounding.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Widen the segment number in filename substitution

Epoch-derived numbers exceed 32 bits for sub-second segment durations (a 0.5 second duration in 2026 gives roughly 3.6e9) and are uncomfortably close to the limit at 1 second. `muxer_util.cc` is the only place in the pipeline that narrows the number to `uint32_t`; `SegmentInfo::segment_number` is already `int64_t` at [media_handler.h:70](packager/media/base/media_handler.h:70).

**Files:**
- Modify: `packager/media/base/muxer_util.h`
- Modify: `packager/media/base/muxer_util.cc:114`
- Test: `packager/media/base/muxer_util_unittest.cc`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `GetSegmentName(const std::string& segment_template, int64_t segment_start_time, int64_t segment_number, uint32_t bandwidth)`.

- [ ] **Step 1: Write the failing test**

Append to `packager/media/base/muxer_util_unittest.cc`:

```cpp
TEST(MuxerUtilTest, SegmentNumberBeyond32Bits) {
  // An epoch-anchored number for half-second segments in 2026 exceeds the
  // range of a signed 32-bit integer and approaches the unsigned limit.
  const int64_t kLargeNumber = 3534451200LL;
  EXPECT_EQ("segment_3534451200.m4s",
            GetSegmentName("segment_$Number$.m4s", 0, kLargeNumber, 0));
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build build --target media_base_unittest -j8 && ./build/packager/media/base/media_base_unittest --gtest_filter='MuxerUtilTest.SegmentNumberBeyond32Bits'
```

Expected: FAIL. The value is truncated by the `uint32_t` parameter, so the produced name does not match.

- [ ] **Step 3: Widen the parameter**

In `packager/media/base/muxer_util.h` and `packager/media/base/muxer_util.cc`, change the `GetSegmentName` parameter `uint32_t segment_number` to `int64_t segment_number`. At [muxer_util.cc:158](packager/media/base/muxer_util.cc:158) the cast stays but widens:

```cpp
    if (identifier == "Number") {
      // SegmentNumber starts from 1.
      format_args.emplace_back(static_cast<uint64_t>(segment_number));
```

Build and fix any caller that now warns on narrowing. Callers pass `SegmentInfo::segment_number`, which is already `int64_t`, so most call sites lose a cast rather than gain one.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build --target media_base_unittest -j8 && ./build/packager/media/base/media_base_unittest --gtest_filter='MuxerUtilTest.*'
```

Expected: PASS, including the pre-existing cases.

- [ ] **Step 5: Commit**

```bash
git add packager/media/base/muxer_util.h packager/media/base/muxer_util.cc packager/media/base/muxer_util_unittest.cc
git commit -m "$(cat <<'EOF'
fix: widen segment number to 64-bit in filename substitution

Epoch-anchored numbers exceed 32 bits at sub-second segment durations.
SegmentInfo::segment_number is already int64_t; muxer_util was the only
place narrowing it.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Plumb the anchor through ChunkingParams and ChunkingHandler

**Files:**
- Modify: `include/packager/chunking_params.h`
- Modify: `packager/media/chunking/chunking_handler.h`
- Modify: `packager/media/chunking/chunking_handler.cc:44-46,174-186`
- Test: `packager/media/chunking/chunking_handler_unittest.cc`

**Interfaces:**
- Consumes: `EpochSegmentNumber` from Task 1.
- Produces: `ChunkingParams::segment_number_epoch_us` (`std::optional<int64_t>`, unset means today's behaviour).

- [ ] **Step 1: Write the failing test**

Append to `packager/media/chunking/chunking_handler_unittest.cc`. Follow the fixture conventions already in that file for feeding stream info and samples.

```cpp
TEST_F(ChunkingHandlerTest, EpochAnchoredSegmentNumbers) {
  // Anchor PTS 0 at 2026-01-01T00:00:00Z with 1 second segments, so the
  // expected first number is that instant in seconds.
  const int64_t k2026Us = 1767225600000000LL;
  ChunkingParams params;
  params.segment_duration_in_seconds = 1;
  params.segment_number_epoch_us = k2026Us;
  SetUpChunkingHandler(params);

  ASSERT_OK(Process(StreamData::FromStreamInfo(kStreamIndex, GetVideoStreamInfo(kTimeScale))));
  // Two 1-second segments starting at PTS 0.
  ASSERT_OK(Process(StreamData::FromMediaSample(kStreamIndex, GetMediaSample(0, kTimeScale, true))));
  ASSERT_OK(Process(StreamData::FromMediaSample(kStreamIndex, GetMediaSample(kTimeScale, kTimeScale, true))));
  ASSERT_OK(Process(StreamData::FromMediaSample(kStreamIndex, GetMediaSample(2 * kTimeScale, kTimeScale, true))));

  const int64_t expected_first = k2026Us / 1000000;
  EXPECT_THAT(GetOutputStreamDataVector(),
              IsSegmentNumberSequence({expected_first, expected_first + 1}));
}

TEST_F(ChunkingHandlerTest, WithoutEpochUsesStartSegmentNumber) {
  ChunkingParams params;
  params.segment_duration_in_seconds = 1;
  params.start_segment_number = 7;
  SetUpChunkingHandler(params);

  ASSERT_OK(Process(StreamData::FromStreamInfo(kStreamIndex, GetVideoStreamInfo(kTimeScale))));
  ASSERT_OK(Process(StreamData::FromMediaSample(kStreamIndex, GetMediaSample(0, kTimeScale, true))));
  ASSERT_OK(Process(StreamData::FromMediaSample(kStreamIndex, GetMediaSample(kTimeScale, kTimeScale, true))));

  EXPECT_THAT(GetOutputStreamDataVector(), IsSegmentNumberSequence({7}));
}
```

If the fixture has no `IsSegmentNumberSequence` matcher, write the assertions by walking `GetOutputStreamDataVector()` and comparing `segment_info->segment_number` on each `kSegmentInfo` entry instead. Do not invent a matcher that does not exist.

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build build --target media_chunking_unittest -j8
```

Expected: FAIL to compile, `no member named 'segment_number_epoch_us' in 'shaka::ChunkingParams'`.

- [ ] **Step 3: Add the parameter**

In `include/packager/chunking_params.h`, add `#include <optional>` and, immediately after `start_segment_number`:

```cpp
  /// When set, segment numbers are derived from wall-clock time instead of
  /// counting from start_segment_number: the number is
  /// floor((segment_number_epoch_us + pts) / segment_duration), counted from
  /// the Unix epoch. This makes numbering identical across independent
  /// packager instances fed by epoch-locked encoders, and stable across a
  /// restart. The value is the UTC instant corresponding to media timeline
  /// zero, in microseconds since the Unix epoch.
  /// start_segment_number is ignored when this is set.
  std::optional<int64_t> segment_number_epoch_us;
```

- [ ] **Step 4: Use it in the handler**

In `packager/media/chunking/chunking_handler.h`, add `#include <packager/media/chunking/epoch_segment_numbering.h>` and a member:

```cpp
  // Segment duration in microseconds, for epoch-anchored numbering.
  int64_t segment_duration_us_ = 0;
```

In `packager/media/chunking/chunking_handler.cc`, compute the duration in `OnStreamInfo` alongside the existing tick durations:

```cpp
  segment_duration_us_ = static_cast<int64_t>(
      chunking_params_.segment_duration_in_seconds * 1000000);
```

Then in `EndSegmentIfStarted`, replace the counter assignment at line 186:

```cpp
  if (chunking_params_.segment_number_epoch_us) {
    segment_info->segment_number = EpochSegmentNumber(
        unwrapped_start, time_scale_,
        *chunking_params_.segment_number_epoch_us, segment_duration_us_);
  } else {
    segment_info->segment_number = segment_number_++;
  }
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --build build --target media_chunking_unittest -j8 && ./build/packager/media/chunking/media_chunking_unittest --gtest_filter='ChunkingHandlerTest.*'
```

Expected: PASS, including every pre-existing `ChunkingHandlerTest` case, which proves default behaviour is unchanged.

- [ ] **Step 6: Commit**

```bash
git add include/packager/chunking_params.h packager/media/chunking/chunking_handler.h packager/media/chunking/chunking_handler.cc packager/media/chunking/chunking_handler_unittest.cc
git commit -m "$(cat <<'EOF'
feat: derive segment numbers from a wall-clock anchor in ChunkingHandler

When ChunkingParams::segment_number_epoch_us is set, the segment number
becomes a pure function of the anchor and the segment start PTS instead
of a free-running counter. Unset keeps the existing behaviour exactly.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Operator flag and validation

**Files:**
- Modify: `packager/app/muxer_flags.cc` (flag definition, next to `start_segment_number` at line 95)
- Modify: `packager/app/muxer_flags.h`
- Modify: `packager/app/packager_main.cc:435` (parsing and validation)
- Test: `packager/app/test/packager_test.py` (flag rejection cases)

**Interfaces:**
- Consumes: `ChunkingParams::segment_number_epoch_us` from Task 3.
- Produces: the `--segment_number_epoch` command-line flag, an RFC 3339 UTC timestamp string, empty by default.

- [ ] **Step 1: Define the flag**

In `packager/app/muxer_flags.cc`, after the `start_segment_number` flag:

```cpp
ABSL_FLAG(std::string,
          segment_number_epoch,
          "",
          "If set, segment numbers are derived from wall-clock time rather "
          "than counted from --start_segment_number. The value is an RFC 3339 "
          "UTC instant corresponding to media timeline zero, for example "
          "1970-01-01T00:00:00Z. Independent packager instances configured "
          "with the same value and fed frame-aligned inputs produce identical "
          "segment numbers, and numbering survives a restart. Cannot be "
          "combined with an explicit --start_segment_number.");
```

Declare it in `packager/app/muxer_flags.h` alongside the others.

- [ ] **Step 2: Write the failing validation test**

Add to `packager/app/test/packager_test.py`, next to the other flag-rejection tests:

```python
  def testRejectsEpochWithStartSegmentNumber(self):
    packager = self._GetPackagerCommand(
        self._GetStreams(['video']),
        self._GetFlags() + ['--segment_number_epoch', '2026-01-01T00:00:00Z',
                            '--start_segment_number', '5'])
    self.assertNotEqual(0, self._RunPackager(packager))

  def testRejectsMalformedEpoch(self):
    packager = self._GetPackagerCommand(
        self._GetStreams(['video']),
        self._GetFlags() + ['--segment_number_epoch', 'not-a-timestamp'])
    self.assertNotEqual(0, self._RunPackager(packager))
```

Match the helper names actually used by the surrounding tests in that file; if `_RunPackager` and `_GetPackagerCommand` are spelled differently there, use the local spelling rather than these.

- [ ] **Step 3: Run to verify it fails**

```bash
cmake --build build --target packager -j8 && python3 packager/app/test/packager_test.py PackagerFunctionalTest.testRejectsMalformedEpoch
```

Expected: FAIL. The unknown flag causes a different error, or the malformed value is accepted.

- [ ] **Step 4: Parse and validate**

In `packager/app/packager_main.cc`, near the existing `start_segment_number` assignment at line 435:

```cpp
  const std::string epoch_str = absl::GetFlag(FLAGS_segment_number_epoch);
  if (!epoch_str.empty()) {
    absl::Time epoch;
    std::string parse_error;
    if (!absl::ParseTime(absl::RFC3339_full, epoch_str, &epoch, &parse_error)) {
      LOG(ERROR) << "Invalid --segment_number_epoch '" << epoch_str
                 << "': " << parse_error;
      return false;
    }
    // absl has no "was this flag set explicitly" predicate, so detect the
    // conflict by comparing against the documented default of 1.
    if (absl::GetFlag(FLAGS_start_segment_number) != 1) {
      LOG(ERROR) << "--segment_number_epoch cannot be combined with an "
                    "explicit --start_segment_number: epoch-anchored "
                    "numbering must be identical across instances.";
      return false;
    }
    chunking_params.segment_number_epoch_us = absl::ToUnixMicros(epoch);
  }
```

Return the same way neighbouring validation failures in that function do; if they return a `Status` rather than `bool`, match that.

- [ ] **Step 5: Run to verify it passes**

```bash
cmake --build build --target packager -j8 && python3 packager/app/test/packager_test.py PackagerFunctionalTest.testRejectsMalformedEpoch PackagerFunctionalTest.testRejectsEpochWithStartSegmentNumber
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add packager/app/muxer_flags.cc packager/app/muxer_flags.h packager/app/packager_main.cc packager/app/test/packager_test.py
git commit -m "$(cat <<'EOF'
feat: --segment_number_epoch flag for wall-clock segment numbering

Parses an RFC 3339 UTC instant for media timeline zero and rejects the
combination with an explicit --start_segment_number, which would break
cross-instance agreement.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: HLS media sequence (DASH already carries the number)

Verified during planning: the DASH path needs **no production change**. The number
flows `MuxerListener::OnNewSegment` to [`Representation::AddNewSegment`](packager/mpd/base/representation.cc:177)
to [`AddSegmentInfo`](packager/mpd/base/representation.cc:412), which stores it as
`SegmentInfo::start_segment_number` ([segment_info.h:26](packager/mpd/base/segment_info.h:26)),
and [xml_node.cc:528](packager/mpd/base/xml/xml_node.cc:528) takes `startNumber` from the
first entry. That half needs a regression test only.

The HLS path does not carry it at all: [`HlsNotifier::NotifyNewSegment`](packager/hls/base/hls_notifier.h:90)
and [`MediaPlaylist::AddSegment`](packager/hls/base/media_playlist.h:144) have no
segment-number parameter, and `media_sequence_number_` is a separate `uint32_t`
counter seeded from `hls_params_.media_sequence_number`
([media_playlist.h:344](packager/hls/base/media_playlist.h:344)). This task threads the
number through and widens that counter.

**Files:**
- Modify: `packager/hls/base/hls_notifier.h:90` (add parameter)
- Modify: `packager/hls/base/simple_hls_notifier.h/.cc` (implement)
- Modify: `packager/hls/base/media_playlist.h:144,344`, `packager/hls/base/media_playlist.cc:472,1166`
- Modify: `packager/hls/base/mock_media_playlist.h/.cc` (mock signature)
- Modify: `packager/media/event/hls_notify_muxer_listener.cc:280` (pass it)
- Test: `packager/hls/base/media_playlist_unittest.cc`, `packager/mpd/base/representation_unittest.cc`

**Interfaces:**
- Consumes: `segment_number` as delivered to `MuxerListener::OnNewSegment(..., int64_t segment_number)` ([muxer_listener.h:165](packager/media/event/muxer_listener.h:165)).
- Produces:
  - `MediaPlaylist::AddSegment(const std::string& file_name, int64_t start_time, int64_t duration, uint64_t start_byte_offset, uint64_t size, int64_t segment_number)`
  - `HlsNotifier::NotifyNewSegment(uint32_t stream_id, const std::string& segment_name, int64_t start_time, int64_t duration, uint64_t start_byte_offset, uint64_t size, int64_t segment_number)`

- [ ] **Step 1: Write the failing DASH regression test**

Append to `packager/mpd/base/representation_unittest.cc`, using the existing
`CreateRepresentation` helper ([representation_unittest.cc:64](packager/mpd/base/representation_unittest.cc:64)):

```cpp
TEST_F(SegmentTemplateTest, EpochAnchoredStartNumber) {
  // An epoch-derived number must appear verbatim as startNumber rather than
  // being renumbered from 1.
  const int64_t kEpochNumber = 441806400LL;
  AddSegments(0, 10, 128, 0, kEpochNumber);
  EXPECT_THAT(representation_->GetXml(), XmlNodeEqual(ExpectedXml(kEpochNumber)));
}
```

Match the fixture's own helpers (`AddSegments`, `ExpectedXml`) as that file
defines them. If `AddSegments` does not take a segment number, call
`representation_->AddNewSegment(0, 10, 128, kEpochNumber)` directly and assert
on the serialized XML containing `startNumber="441806400"`.

- [ ] **Step 2: Run it**

```bash
cmake --build build --target mpd_unittest -j8 && ./build/packager/mpd/mpd_unittest --gtest_filter='*EpochAnchoredStartNumber*'
```

Expected: PASS without any production change. If it fails, the DASH path is not
as traced and the plan must be revised before continuing.

- [ ] **Step 3: Write the failing HLS test**

Append to `packager/hls/base/media_playlist_unittest.cc`, following the
`MediaPlaylistMultiSegmentTest` fixture conventions used at
[media_playlist_unittest.cc:145](packager/hls/base/media_playlist_unittest.cc:145):

```cpp
TEST_F(MediaPlaylistMultiSegmentTest, EpochAnchoredMediaSequence) {
  ASSERT_TRUE(media_playlist_->SetMediaInfo(valid_video_media_info_));
  const int64_t kEpochNumber = 441806400LL;
  media_playlist_->AddSegment("file1.ts", 900000, 1000000, kZeroByteOffset,
                              1000000, kEpochNumber);
  media_playlist_->AddSegment("file2.ts", 1900000, 1000000, kZeroByteOffset,
                              1000000, kEpochNumber + 1);

  std::string actual;
  ASSERT_TRUE(media_playlist_->WriteToString(&actual));
  EXPECT_THAT(actual, HasSubstr("#EXT-X-MEDIA-SEQUENCE:441806400"));
}
```

Use whichever serialization entry point the neighbouring tests use for a live
playlist; do not invent `WriteToString` if the file spells it differently.

- [ ] **Step 4: Run it to verify it fails**

```bash
cmake --build build --target hls_unittest -j8
```

Expected: FAIL to compile, too many arguments to `AddSegment`.

- [ ] **Step 5: Thread the parameter through**

In order, so the tree builds at each step:

1. `media_playlist.h:144` and `media_playlist.cc`: add `int64_t segment_number` as the last `AddSegment` parameter.
2. `media_playlist.h:344`: change `uint32_t media_sequence_number_ = 0;` to `int64_t media_sequence_number_ = 0;`.
3. In `MediaPlaylist::AddSegment`, when the playlist has no segments yet and the
   supplied number is greater than the seeded `media_sequence_number_`, adopt it:

```cpp
  if (entries_.empty() && segment_number > media_sequence_number_)
    media_sequence_number_ = segment_number;
```

   Leave the existing increment at [media_playlist.cc:1166](packager/hls/base/media_playlist.cc:1166)
   untouched so the default path and segment eviction behave exactly as before.
4. `mock_media_playlist.h/.cc`: update the `MOCK_METHOD` signature to match.
5. `hls_notifier.h:90` and `simple_hls_notifier.h/.cc`: add the same trailing
   parameter and forward it to `AddSegment`.
6. `hls_notify_muxer_listener.cc:280`: pass the `segment_number` argument that
   `OnNewSegment` already receives.

- [ ] **Step 6: Run both suites**

```bash
cmake --build build --target hls_unittest mpd_unittest -j8 && ./build/packager/hls/hls_unittest && ./build/packager/mpd/mpd_unittest
```

Expected: PASS, every case in both suites, which also proves the default
(non-epoch) numbering is untouched.

- [ ] **Step 7: Commit**

```bash
git add packager/hls/base/hls_notifier.h packager/hls/base/simple_hls_notifier.h packager/hls/base/simple_hls_notifier.cc packager/hls/base/media_playlist.h packager/hls/base/media_playlist.cc packager/hls/base/media_playlist_unittest.cc packager/hls/base/mock_media_playlist.h packager/hls/base/mock_media_playlist.cc packager/media/event/hls_notify_muxer_listener.cc packager/mpd/base/representation_unittest.cc
git commit -m "$(cat <<'EOF'
feat: carry the segment number into the HLS media sequence

HLS kept its own uint32_t counter independent of
SegmentInfo::segment_number, so epoch-anchored numbering reached segment
filenames and the MPD startNumber but not EXT-X-MEDIA-SEQUENCE. The DASH
path already carried it and gains a regression test only.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: Restart safety across PTS wrap

`PtsUnwrapper` is relative: its first call returns the raw value and it counts wraps from there. An instance that has run 30 hours and a freshly restarted one therefore disagree by 2^33 ticks for the same frame, which would break numbering for any channel running longer than about 26.5 hours.

**Files:**
- Modify: `packager/media/base/timestamp_util.h` (add a seeding entry point)
- Modify: `packager/media/chunking/chunking_handler.cc` (seed on first sample)
- Test: `packager/media/chunking/chunking_handler_unittest.cc`

**Interfaces:**
- Consumes: `ResolveWrapOffset` from Task 1.
- Produces: `void PtsUnwrapper::SeedWrapOffset(int64_t offset)`, which presets the accumulated wrap offset before the first `Unwrap` call.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_F(ChunkingHandlerTest, EpochNumbersSurviveRestartAfterPtsWrap) {
  // Simulate a restart 30 hours into a stream: the input PTS has wrapped
  // once, so the raw value is small, but the number must continue as though
  // the instance had been running throughout.
  const int64_t k2026Us = 1767225600000000LL;
  const int64_t kThirtyHoursUs = 30LL * 3600 * 1000000;
  ChunkingParams params;
  params.segment_duration_in_seconds = 1;
  params.segment_number_epoch_us = k2026Us;
  SetUpChunkingHandler(params);
  SetNowForTesting(k2026Us + kThirtyHoursUs);

  ASSERT_OK(Process(StreamData::FromStreamInfo(kStreamIndex, GetVideoStreamInfo(kTimeScale))));
  // Raw PTS just after the wrap.
  ASSERT_OK(Process(StreamData::FromMediaSample(kStreamIndex, GetMediaSample(0, kTimeScale, true))));
  ASSERT_OK(Process(StreamData::FromMediaSample(kStreamIndex, GetMediaSample(kTimeScale, kTimeScale, true))));

  // 2^33 ticks at 90kHz is 95443 whole seconds.
  const int64_t expected = k2026Us / 1000000 + 95443;
  EXPECT_THAT(GetOutputStreamDataVector(), IsSegmentNumberSequence({expected}));
}
```

This needs an injectable clock. Add a `std::function<int64_t()> now_us_for_testing_` member to `ChunkingHandler` defaulting to `absl::ToUnixMicros(absl::Now())`, exposed through the existing `friend class ChunkingHandlerTest`. Do not add a public setter to production API.

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build --target media_chunking_unittest -j8 && ./build/packager/media/chunking/media_chunking_unittest --gtest_filter='*SurviveRestart*'
```

Expected: FAIL. The computed number is short by 95443.

- [ ] **Step 3: Implement seeding**

Add to `PtsUnwrapper` in `packager/media/base/timestamp_util.h`:

```cpp
  /// Presets the accumulated wrap offset. Must be called before the first
  /// Unwrap(). Used to resume correct absolute timestamps after a restart,
  /// where the wrap count cannot be inferred from observation alone.
  void SeedWrapOffset(int64_t offset);
```

In `ChunkingHandler::OnMediaSample`, on the first sample only and only when `segment_number_epoch_us` is set:

```cpp
  if (chunking_params_.segment_number_epoch_us && !wrap_seeded_) {
    wrap_seeded_ = true;
    pts_unwrapper_.SeedWrapOffset(
        ResolveWrapOffset(timestamp, time_scale_,
                          *chunking_params_.segment_number_epoch_us,
                          now_us_for_testing_()));
  }
```

- [ ] **Step 4: Add a loud warning for a bad anchor**

Still in that block, after seeding, verify the anchor is plausible and say so once if it is not. A silently wrong anchor mis-numbers every segment, which is exactly the failure this feature exists to prevent.

```cpp
    const int64_t implied_us =
        *chunking_params_.segment_number_epoch_us +
        PtsToMicroseconds(pts_unwrapper_.Unwrap(timestamp), time_scale_);
    const int64_t skew_us = implied_us - now_us_for_testing_();
    if (std::abs(skew_us) > 3600LL * 1000000) {
      LOG(WARNING) << "segment_number_epoch implies a stream time "
                   << skew_us / 1000000
                   << "s from the system clock. Segment numbers will not "
                      "match other instances if this anchor is wrong.";
    }
```

Note that `Unwrap` is stateful, so call it once and reuse the value rather than calling it twice here and again below.

- [ ] **Step 5: Run to verify it passes**

```bash
cmake --build build --target media_chunking_unittest -j8 && ./build/packager/media/chunking/media_chunking_unittest --gtest_filter='ChunkingHandlerTest.*'
```

Expected: PASS, all cases.

- [ ] **Step 6: Commit**

```bash
git add packager/media/base/timestamp_util.h packager/media/base/timestamp_util.cc packager/media/chunking/chunking_handler.h packager/media/chunking/chunking_handler.cc packager/media/chunking/chunking_handler_unittest.cc
git commit -m "$(cat <<'EOF'
feat: seed PTS unwrapping from the system clock for restart safety

PtsUnwrapper counts wraps from its first observation, so a restarted
instance disagreed with a long-running one by 2^33 ticks. Seed the offset
from the anchor and the system clock, and warn loudly when the anchor
implies a stream time far from now.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 7: End-to-end cross-instance agreement

**Files:**
- Modify: `packager/app/test/packager_test.py`

**Interfaces:**
- Consumes: the `--segment_number_epoch` flag from Task 4.
- Produces: no API.

- [ ] **Step 1: Write the failing test**

Model it on the existing redundant-input end-to-end test at [packager_test.py:1336](packager/app/test/packager_test.py:1336), which already spawns the packager as a subprocess against live UDP and terminates it.

```python
  def testEpochAnchoredNumberingAgreesAcrossInstances(self):
    # Two packager instances over the same input, started several seconds
    # apart, must produce identical segment numbers for the same content.
    epoch = '2026-01-01T00:00:00Z'
    out_a = os.path.join(self.tmp_dir, 'a')
    out_b = os.path.join(self.tmp_dir, 'b')
    os.makedirs(out_a)
    os.makedirs(out_b)

    names = []
    for out_dir in (out_a, out_b):
      stream = ('input=%s,stream=video,init_segment=%s/init.mp4,'
                'segment_template=%s/$Number$.m4s') % (
                    os.path.join(self.test_data_dir, 'bear-640x360.ts'),
                    out_dir, out_dir)
      cmd = [test_env.PACKAGER_BIN, stream,
             '--segment_duration', '1',
             '--segment_number_epoch', epoch,
             '--test_packager_version', '<tag>-<hash>-<test>']
      self.assertEqual(0, subprocess.call(cmd))
      names.append(sorted(os.listdir(out_dir)))

    self.assertEqual(names[0], names[1])
    # And the numbers must be epoch-derived, not 1, 2, 3.
    numbers = sorted(int(n.split('.')[0]) for n in names[0]
                     if n.endswith('.m4s'))
    self.assertGreater(numbers[0], 1000000000,
                       'segment numbers are not epoch-derived: %s' % numbers)
```

- [ ] **Step 2: Run to verify it fails**

```bash
python3 packager/app/test/packager_test.py PackagerFunctionalTest.testEpochAnchoredNumberingAgreesAcrossInstances
```

Expected: FAIL before Tasks 3 to 5 are merged; after them it should pass. If it fails at this point, the failure is real and must be fixed rather than the assertion relaxed.

- [ ] **Step 3: Run the full functional suite for regressions**

```bash
python3 packager/app/test/packager_test.py
```

Expected: no new failures relative to a run on `main` before this branch. Record any pre-existing failures explicitly; this repo has known macOS golden-file mismatches, so compare against a baseline run rather than assuming green.

- [ ] **Step 4: Commit**

```bash
git add packager/app/test/packager_test.py
git commit -m "$(cat <<'EOF'
test: end-to-end epoch-anchored numbering agreement across instances

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 8: Documentation

**Files:**
- Create: `docs/source/options/segment_numbering_options.rst`
- Modify: `docs/source/tutorials/live.rst`
- Modify: `ARCHITECTURE.md`

**Interfaces:**
- Consumes: everything above.
- Produces: no API.

- [ ] **Step 1: Write the option reference**

Create `docs/source/options/segment_numbering_options.rst` covering: what the flag means, that both pipelines need the same value, that it is incompatible with `--start_segment_number`, the wrap-resolution behaviour and its dependence on a roughly correct system clock, the warning emitted for an implausible anchor, and a worked example for a redundant pair. Follow the structure of [redundant_input_options.rst](docs/source/options/redundant_input_options.rst).

- [ ] **Step 2: Add the operational context**

In `docs/source/tutorials/live.rst`, add a short section on epoch-anchored numbering as the enabler for per-segment origin arbitration, and state plainly what it does not do: it does not align segment boundaries (that is the encoders' job), does not detect degraded input, and does not signal discontinuities on source switch.

- [ ] **Step 3: Update the architecture overview**

Add the new flag and the numbering path to the relevant table in `ARCHITECTURE.md`, next to the existing redundant-input entry in section 12.

- [ ] **Step 4: Commit**

```bash
git add docs/source/options/segment_numbering_options.rst docs/source/tutorials/live.rst ARCHITECTURE.md
git commit -m "$(cat <<'EOF'
docs: epoch-anchored segment numbering flag and operational limits

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

## Out of Scope

These are deliberately excluded and remain open after this plan:

- **Epoch-anchored key rotation.** [encryption_handler.cc:350](packager/media/crypto/encryption_handler.cc:350) derives the crypto period index from DTS. Two instances will still diverge on key rotation. This needs the same anchor applied to the crypto period index, and is a natural follow-up plan.
- **Segment boundary alignment.** Boundaries already derive deterministically from input PTS, so alignment is an encoder responsibility (epoch-locked IDR placement).
- **Discontinuity signalling on input switch.** A `redundant://` failover switch is still invisible to the packaging layer.
- **Quality-aware arbitration.** Requires a decoded-domain probe and does not belong in the packager.

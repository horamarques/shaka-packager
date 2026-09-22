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

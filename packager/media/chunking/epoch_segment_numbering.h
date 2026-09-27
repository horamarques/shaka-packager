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

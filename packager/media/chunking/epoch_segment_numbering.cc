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

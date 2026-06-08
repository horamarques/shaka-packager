// Copyright 2024 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/chunking/sync_point_queue.h>

#include <limits>

#include <gtest/gtest.h>

#include <packager/media/base/media_handler.h>

namespace shaka {
namespace media {

TEST(SyncPointQueueTest, DynamicCuePointInsertion) {
  AdCueGeneratorParams empty_params;
  SyncPointQueue queue(empty_params);
  queue.AddThread();

  // No hints initially.
  EXPECT_EQ(std::numeric_limits<double>::max(), queue.GetHint(-1));

  // Add a dynamic cue point.
  queue.AddDynamicCuePoint(10.0, CueEventType::kCueOut, 0.0, "cue_data_out");

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

TEST(SyncPointQueueTest, DynamicCuePointSkipsDuplicateTime) {
  AdCueGeneratorParams empty_params;
  SyncPointQueue queue(empty_params);
  queue.AddThread();

  queue.AddDynamicCuePoint(10.0, CueEventType::kCueOut, 0.0, "first");
  queue.AddDynamicCuePoint(10.0, CueEventType::kCueIn, 0.0, "second");

  auto promoted = queue.PromoteAt(10.0);
  ASSERT_NE(nullptr, promoted);
  EXPECT_EQ(CueEventType::kCueOut, promoted->type);
  EXPECT_EQ("first", promoted->cue_data);
}

TEST(SyncPointQueueTest, DynamicCuePointMixedWithStatic) {
  AdCueGeneratorParams params;
  Cuepoint static_cue;
  static_cue.start_time_in_seconds = 5.0;
  params.cue_points.push_back(static_cue);

  SyncPointQueue queue(params);
  queue.AddThread();

  // Static hint at 5.0.
  EXPECT_DOUBLE_EQ(5.0, queue.GetHint(-1));

  // Add dynamic cue at 3.0 (before static).
  queue.AddDynamicCuePoint(3.0, CueEventType::kCueOut, 0.0, "dynamic");

  // Hint should now be 3.0 (earliest).
  EXPECT_DOUBLE_EQ(3.0, queue.GetHint(-1));

  // Promote dynamic, then static should be next.
  auto promoted = queue.PromoteAt(3.0);
  ASSERT_NE(nullptr, promoted);
  EXPECT_DOUBLE_EQ(3.0, promoted->time_in_seconds);
  EXPECT_EQ("dynamic", promoted->cue_data);
}

}  // namespace media
}  // namespace shaka

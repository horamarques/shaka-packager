# Copyright 2026 Google LLC. All rights reserved.
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file or at
# https://developers.google.com/open-source/licenses/bsd

add_library(metrics STATIC
    metrics_service.cc)
target_link_libraries(metrics
    absl::strings
    prometheus-cpp::core
    prometheus-cpp::pull
    status
    version)

add_executable(metrics_unittest
    metrics_service_unittest.cc)
target_link_libraries(metrics_unittest
    gtest
    gtest_main
    metrics)
add_gtest(metrics_unittest)

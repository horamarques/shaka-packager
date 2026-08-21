# Copyright 2026 Google LLC. All rights reserved.
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file or at
# https://developers.google.com/open-source/licenses/bsd

add_library(webapi_core STATIC
    service/event_manager.cc)
target_link_libraries(webapi_core
    absl::strings
    absl::synchronization
    status)

add_executable(webapi_core_unittest
    service/event_manager_unittest.cc)
target_link_libraries(webapi_core_unittest
    gtest
    gtest_main
    webapi_core)
add_gtest(webapi_core_unittest)

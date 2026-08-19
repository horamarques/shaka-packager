# Copyright 2026 Google LLC. All rights reserved.
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file or at
# https://developers.google.com/open-source/licenses/bsd

# CMake build file for the prometheus-cpp library, which provides the
# metrics registry and the /metrics HTTP exposer (bundled civetweb).

set(ENABLE_PUSH OFF)
set(ENABLE_COMPRESSION OFF)
set(ENABLE_TESTING OFF)

add_subdirectory(source EXCLUDE_FROM_ALL)

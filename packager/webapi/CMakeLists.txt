# Copyright 2026 Google LLC. All rights reserved.
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file or at
# https://developers.google.com/open-source/licenses/bsd

add_library(webapi_core STATIC
    service/event_manager.cc
    service/event_spec.cc)
target_link_libraries(webapi_core
    absl::strings
    absl::synchronization
    status)

add_executable(webapi_core_unittest
    service/event_manager_unittest.cc
    service/event_spec_unittest.cc)
target_link_libraries(webapi_core_unittest
    gtest
    gtest_main
    webapi_core)
add_gtest(webapi_core_unittest)

add_executable(packager-api
    controller/event_controller.cc
    main.cc)
target_compile_definitions(packager-api PRIVATE
    OATPP_SWAGGER_RES_PATH="${OATPP_SWAGGER_RES_PATH}")
# oatpp 1.3.0's own headers (Type.hpp, MemoryLabel.hpp, ApiController's
# ContentHints aggregate init, etc.) trip several warnings under this
# project's global -Wall -Wextra -Werror (packager/CMakeLists.txt). We don't
# control that third-party header code, so demote errors back to warnings
# for this target only, same spirit as third_party/oatpp/CMakeLists.txt's
# `-w` on the oatpp/oatpp-swagger targets themselves.
target_compile_options(packager-api PRIVATE -Wno-error)
target_link_libraries(packager-api
    absl::flags
    absl::flags_parse
    absl::log
    absl::log_initialize
    absl::strings
    oatpp::oatpp
    oatpp::oatpp-swagger
    version
    webapi_core)

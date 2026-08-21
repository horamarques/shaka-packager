# Copyright 2026 Google LLC. All rights reserved.
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file or at
# https://developers.google.com/open-source/licenses/bsd

# CMake build file for oatpp + oatpp-swagger, the HTTP/OpenAPI framework
# used by the packager-api web service.
#
# Uses FetchContent (pinned tags) instead of the submodule pattern:
# oatpp-swagger's CMake needs the OATPP_MODULES_LOCATION=CUSTOM discovery
# settings below to build against an in-tree oatpp, a recipe proven in
# VxPackager.

include(FetchContent)

FetchContent_Declare(
  oatpp
  GIT_REPOSITORY https://github.com/oatpp/oatpp.git
  GIT_TAG 1.3.0
)
FetchContent_Declare(
  oatpp_swagger
  GIT_REPOSITORY https://github.com/oatpp/oatpp-swagger.git
  GIT_TAG 1.3.0
)

set(OATPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)

# Let oatpp-swagger's sub-build find the fetched oatpp without a prebuilt
# package.
set(oatpp_DIR ${CMAKE_BINARY_DIR}/_deps/oatpp-build CACHE PATH "" FORCE)
set(OATPP_MODULES_LOCATION CUSTOM CACHE STRING "" FORCE)
set(OATPP_DIR_SRC ${CMAKE_BINARY_DIR}/_deps/oatpp-src/src CACHE PATH "" FORCE)
set(OATPP_DIR_LIB ${CMAKE_BINARY_DIR}/_deps/oatpp-build/src CACHE PATH "" FORCE)
list(APPEND CMAKE_PREFIX_PATH ${oatpp_DIR})

# oatpp and oatpp-swagger declare cmake_minimum_required(VERSION 3.1), which
# CMake >= 4.0 refuses outright (support for < 3.5 was removed). Raise the
# floor only for their subdirectory scope, not the rest of the build.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(oatpp oatpp_swagger)
unset(CMAKE_POLICY_VERSION_MINIMUM)

if(TARGET oatpp AND NOT TARGET oatpp::oatpp)
  add_library(oatpp::oatpp ALIAS oatpp)
endif()
if(TARGET oatpp-swagger AND NOT TARGET oatpp::oatpp-swagger)
  add_library(oatpp::oatpp-swagger ALIAS oatpp-swagger)
endif()

# Relax warnings on the external targets (third_party already sets -w /
# -Wno-error globally for this directory tree, but the fetched sources are
# compiled with their own flags; mirror VxPackager's relaxations).
foreach(_oatpp_target oatpp oatpp-swagger)
  if(TARGET ${_oatpp_target})
    target_compile_options(${_oatpp_target} PRIVATE -w)
  endif()
endforeach()

# Swagger UI static resources live in the fetched source tree.
set(OATPP_SWAGGER_RES_PATH "${oatpp_swagger_SOURCE_DIR}/res"
    CACHE PATH "oatpp-swagger UI resources" FORCE)

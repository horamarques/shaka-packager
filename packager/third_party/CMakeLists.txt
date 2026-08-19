# Copyright 2022 Google LLC. All rights reserved.
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file or at
# https://developers.google.com/open-source/licenses/bsd

# Packager CMake build file for third-party libraries.

# Tell third-party libraries generally not to build their own tests.
set(BUILD_TESTING OFF)

# Tell third-party libraries generally not to build shared library targets.
set(BUILD_SHARED_LIBS OFF)

# Tell CMake that we intend to override some libraries' options with set().
# By setting this default instead of using cmake_policy(SET CMP0077 NEW), we
# ensure that the defaults are reset when a library calls
# cmake_minimum_required.
# See https://gitlab.kitware.com/cmake/cmake/-/issues/20312
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

if(MSVC)
  # Use warning level 3 in third-party code, as opposed to level 4 used in our
  # own code.
  add_compile_options(/W3)

  # Do not treat warnings as errors in third-party code.
  add_compile_options(/WX-)
else()
  # Do not treat warnings as errors in third-party code.
  # Surprisingly, both of these are required to satisfy various platforms and
  # compilers at once.
  add_compile_options(-Wno-error -Wno-error=all)
  # Hide warnings in third-party code.  The noise isn't helpful, and this line
  # can be commented out if we ever want to see/audit third-party warnings.
  add_compile_options(-w)
endif()

# These all use EXCLUDE_FROM_ALL so that only the referenced targets get built.
add_subdirectory(abseil-cpp EXCLUDE_FROM_ALL SYSTEM)
add_subdirectory(c-ares EXCLUDE_FROM_ALL)
add_subdirectory(curl EXCLUDE_FROM_ALL)
add_subdirectory(googletest EXCLUDE_FROM_ALL)
add_subdirectory(json EXCLUDE_FROM_ALL)
# NOTE: libpng references zlib, so put zlib before libpng
add_subdirectory(zlib EXCLUDE_FROM_ALL)
add_subdirectory(libpng EXCLUDE_FROM_ALL)
add_subdirectory(libwebm EXCLUDE_FROM_ALL)
add_subdirectory(libxml2 EXCLUDE_FROM_ALL)
add_subdirectory(mbedtls EXCLUDE_FROM_ALL)
add_subdirectory(mimalloc EXCLUDE_FROM_ALL)
add_subdirectory(mongoose EXCLUDE_FROM_ALL)
add_subdirectory(prometheus-cpp EXCLUDE_FROM_ALL)
# Bundled civetweb (prometheus-cpp's HTTP exposer) and our vendored mongoose
# (used by test_web_server) both define mg_printf/mg_base64_(en|de)code/
# mg_url_(en|de)code with external linkage. Any binary that links both
# (e.g. file_unittest, which needs metrics -> civetweb and test_web_server ->
# mongoose) fails to link with duplicate symbol errors. Rename civetweb's
# copies at compile time via macro substitution so the two libraries no
# longer collide; this only affects the identifiers as seen by the
# preprocessor; behavior is unchanged.
if(TARGET civetweb AND TARGET pull)
  set(_civetweb_renamed_syms
      mg_printf=civetweb_mg_printf
      mg_base64_encode=civetweb_mg_base64_encode
      mg_base64_decode=civetweb_mg_base64_decode
      mg_url_encode=civetweb_mg_url_encode
      mg_url_decode=civetweb_mg_url_decode)
  target_compile_definitions(civetweb PRIVATE ${_civetweb_renamed_syms})
  target_compile_definitions(pull PRIVATE ${_civetweb_renamed_syms})
  unset(_civetweb_renamed_syms)
endif()
# Marking protobuf as SYSTEM enables "external headers" mode in code analysis
# for the protobuf-internal headers included by files generated through protoc.
# This is equivalent to how dependencies added through `find_package` are
# treated.
add_subdirectory(protobuf EXCLUDE_FROM_ALL SYSTEM)

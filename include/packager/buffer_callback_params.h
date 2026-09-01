// Copyright 2017 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_PUBLIC_BUFFER_CALLBACK_PARAMS_H_
#define PACKAGER_PUBLIC_BUFFER_CALLBACK_PARAMS_H_

#include <cstdint>
#include <functional>
#include <string>

namespace shaka {

/// Buffer callback params.
struct BufferCallbackParams {
  /// If this function is specified, packager treats @a StreamDescriptor.input
  /// as a label and call this function with @a name set to
  /// @a StreamDescriptor.input.
  std::function<int64_t(const std::string& name, void* buffer, uint64_t size)>
      read_func;
  /// If this function is specified, packager treats the output files specified
  /// in PackagingParams and StreamDescriptors as labels and calls this function
  /// with @a name set. This applies to @a
  /// PackagingParams.MpdParams.mpd_output,
  /// @a PackagingParams.HlsParams.master_playlist_output, @a
  /// StreamDescriptor.output, @a StreamDescriptor.segment_template, @a
  /// StreamDescriptor.hls_playlist_name.
  std::function<
      int64_t(const std::string& name, const void* buffer, uint64_t size)>
      write_func;
  /// Optional. If this function is specified, packager calls it every time a
  /// callback output is opened for writing, before any @a write_func call for
  /// that open, with @a name as passed to @a write_func and @a mode set to the
  /// mode the writer asked for: "w"/"wb" to REPLACE whatever was previously
  /// written under that name, "a"/"ab" to APPEND to it. Reads are not
  /// reported.
  ///
  /// A callback file has no file position and cannot truncate, so without this
  /// signal a consumer cannot distinguish a rewrite from an append and simply
  /// accumulates both. That matters: the mp4 segmenters write the init segment
  /// once from DoInitialize() and then rewrite it from DoFinalize() to add the
  /// media duration (mehd), both with mode "w". A consumer that ignores the
  /// mode ends up with two concatenated init segments, of which parsers read
  /// the first — the one without the duration.
  std::function<void(const std::string& name, const std::string& mode)>
      open_func;
};

}  // namespace shaka

#endif  // PACKAGER_PUBLIC_BUFFER_CALLBACK_PARAMS_H_

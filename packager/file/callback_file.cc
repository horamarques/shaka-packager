// Copyright 2017 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/file/callback_file.h>

#include <cstdint>

#include <absl/log/log.h>

#include <packager/file.h>
#include <packager/macros/compiler.h>

namespace shaka {

CallbackFile::CallbackFile(const char* file_name, const char* mode)
    : File(file_name), file_mode_(mode) {}

CallbackFile::~CallbackFile() {}

bool CallbackFile::Close() {
  delete this;
  return true;
}

int64_t CallbackFile::Read(void* buffer, uint64_t length) {
  if (!callback_params_->read_func) {
    LOG(ERROR) << "Read function not defined.";
    return -1;
  }
  return callback_params_->read_func(name_, buffer, length);
}

int64_t CallbackFile::Write(const void* buffer, uint64_t length) {
  if (!callback_params_->write_func) {
    LOG(ERROR) << "Write function not defined.";
    return -1;
  }
  return callback_params_->write_func(name_, buffer, length);
}

void CallbackFile::CloseForWriting() {}

int64_t CallbackFile::Size() {
  LOG(INFO) << "CallbackFile does not support Size().";
  return -1;
}

bool CallbackFile::Flush() {
  // Do nothing on Flush.
  return true;
}

bool CallbackFile::Seek(uint64_t position) {
  UNUSED(position);
  VLOG(1) << "CallbackFile does not support Seek().";
  return false;
}

bool CallbackFile::Tell(uint64_t* position) {
  UNUSED(position);
  VLOG(1) << "CallbackFile does not support Tell().";
  return false;
}

bool CallbackFile::Open() {
  // Modes "w" and "a" are both accepted. A callback file has no file position
  // and cannot truncate itself, so the distinction is forwarded to the caller
  // through open_func instead of acted on here: "w" means the writer is
  // REPLACING what it last wrote under this name, "a" means it is extending
  // it. Low-latency segmenters open in-progress segments with mode "a"; the
  // mp4 segmenters rewrite the init segment with mode "w".
  if (file_mode_ != "r" && file_mode_ != "w" && file_mode_ != "a" &&
      file_mode_ != "rb" && file_mode_ != "wb" && file_mode_ != "ab") {
    LOG(ERROR) << "CallbackFile does not support file mode " << file_mode_;
    return false;
  }
  if (!ParseCallbackFileName(file_name(), &callback_params_, &name_))
    return false;
  if (file_mode_ != "r" && file_mode_ != "rb" && callback_params_->open_func)
    callback_params_->open_func(name_, file_mode_);
  return true;
}

}  // namespace shaka

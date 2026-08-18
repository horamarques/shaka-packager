// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_FILE_REDUNDANT_UDP_FILE_H_
#define PACKAGER_FILE_REDUNDANT_UDP_FILE_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <packager/file.h>
#include <packager/file/file_closer.h>
#include <packager/file/io_cache.h>
#include <packager/file/redundant_input_merger.h>
#include <packager/macros/classes.h>

namespace shaka {

/// Implements RedundantUdpFile, which receives the same MPEG-TS multiplex
/// from multiple UDP legs (SMPTE 2022-7 style) and exposes a single merged
/// byte stream. See docs/superpowers/specs/2026-08-18-redundant-ts-input-
/// design.md.
///
/// URL syntax (the "redundant://" prefix is stripped by the File factory):
///   <udp-url>|<udp-url>[&mode=merge|failover][&dedup_window_ms=N]
///   [&dedup_window_pkts=N][&failover_timeout_ms=N]
/// where each <udp-url> is a full "udp://<ip>:<port>[?<udp-options>]".
/// Recognized merger parameters are stripped from the tail; anything else
/// stays with the last leg's UDP options.
///
/// Legs without an explicit timeout_us UDP option get a 100 ms receive
/// timeout so reader threads can drive health ticks and shut down promptly.
class RedundantUdpFile : public File {
 public:
  /// @param url contains the legs and options (without the scheme prefix).
  explicit RedundantUdpFile(const char* url);

  /// @name File implementation overrides.
  /// @{
  bool Close() override;
  int64_t Read(void* buffer, uint64_t length) override;
  int64_t Write(const void* buffer, uint64_t length) override;
  void CloseForWriting() override;
  int64_t Size() override;
  bool Flush() override;
  bool Seek(uint64_t position) override;
  bool Tell(uint64_t* position) override;
  /// @}

  /// Parses |url| into leg UDP urls (with "udp://" prefix) and merger config.
  /// Exposed for testing. @return false on malformed input.
  static bool ParseUrl(const std::string& url,
                       std::vector<std::string>* leg_urls,
                       RedundantInputMerger::Config* config);

 protected:
  ~RedundantUdpFile() override;

  bool Open() override;

 private:
  void ReaderThread(size_t leg_index);
  // Logs per-leg and global merger counters once per minute (mutex held).
  void MaybeLogStats(int64_t now_ms);

  std::vector<std::string> leg_urls_;
  RedundantInputMerger::Config config_;

  // Emitted packets ready for the demuxer.
  IoCache cache_;

  // Guards merger_ (reader threads call OnBytes/OnTick concurrently).
  std::mutex merger_mutex_;
  std::unique_ptr<RedundantInputMerger> merger_;

  std::vector<File*> legs_;  // Owned; closed in Close().
  // Last time the per-minute stats summary was logged (guarded by
  // merger_mutex_).
  int64_t last_stats_log_ms_ = 0;
  std::vector<std::thread> threads_;
  std::atomic<bool> stop_{false};

  DISALLOW_COPY_AND_ASSIGN(RedundantUdpFile);
};

}  // namespace shaka

#endif  // PACKAGER_FILE_REDUNDANT_UDP_FILE_H_

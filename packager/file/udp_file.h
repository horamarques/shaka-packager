// Copyright 2014 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef MEDIA_FILE_UDP_FILE_H_
#define MEDIA_FILE_UDP_FILE_H_

#include <cstdint>

#if defined(OS_WIN)
#include <windows.h>
#include <winsock2.h>
#else
typedef int SOCKET;
#endif  // defined(OS_WIN)

#include <packager/file.h>
#include <packager/macros/classes.h>

namespace prometheus {
class Counter;
class Gauge;
}  // namespace prometheus

namespace shaka {

/// Implements UdpFile, which receives UDP unicast and multicast streams.
class UdpFile : public File {
 public:
  /// @param file_name C string containing the address of the stream to receive.
  ///        It should be of the form "<ip_address>:<port>".
  explicit UdpFile(const char* address_and_port);

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

 protected:
  ~UdpFile() override;

  bool Open() override;

 private:
  SOCKET socket_;
#if defined(OS_WIN)
  // For Winsock in Windows.
  bool wsa_started_ = false;
#endif  // defined(OS_WIN)

  // Metrics handles; created in Open(), null until then. Never owned.
  prometheus::Counter* bytes_received_counter_ = nullptr;
  prometheus::Counter* datagrams_received_counter_ = nullptr;
  prometheus::Counter* recv_timeouts_counter_ = nullptr;
  prometheus::Counter* recv_errors_counter_ = nullptr;
  prometheus::Gauge* last_receive_timestamp_gauge_ = nullptr;
#if defined(__linux__)
  prometheus::Counter* kernel_drops_counter_ = nullptr;
  // SO_RXQ_OVFL reports a cumulative per-socket drop count; track the last
  // value so the counter advances by deltas.
  uint32_t last_kernel_drop_count_ = 0;
  bool kernel_drop_count_valid_ = false;
#endif

  DISALLOW_COPY_AND_ASSIGN(UdpFile);
};

}  // namespace shaka

#endif  // MEDIA_FILE_UDP_FILE_H_

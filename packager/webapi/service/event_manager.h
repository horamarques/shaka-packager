// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_WEBAPI_SERVICE_EVENT_MANAGER_H_
#define PACKAGER_WEBAPI_SERVICE_EVENT_MANAGER_H_

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <packager/status.h>

namespace shaka {
namespace webapi {

enum class EventState { kStarting, kRunning, kStopping, kStopped, kFailed };
// "STARTING"/"RUNNING"/"STOPPING"/"STOPPED"/"FAILED"
std::string EventStateName(EventState state);

struct EventSnapshot {
  std::string id;
  EventState state;
  int pid = -1;
  std::optional<int> exit_code;           // set in kStopped/kFailed
  int metrics_port = 0;
  std::string log_path;
  std::vector<std::string> argv;
  int64_t created_unix = 0;
  int64_t started_unix = 0;               // 0 until kRunning
  int64_t stopped_unix = 0;               // 0 until terminal
};

class EventManager {
 public:
  struct Config {
    std::string packager_bin;
    std::string log_dir;
    int metrics_port_min = 19100;
    int metrics_port_max = 19199;
    int readiness_timeout_ms = 5000;      // STARTING -> RUNNING fallback
    int max_terminal_events = 100;        // retention cap; oldest evicted
  };

  explicit EventManager(const Config& config);
  ~EventManager();                        // calls Shutdown()

  /// Spawns a packager subprocess. |argv_tail| is everything AFTER the
  /// binary path (streams + flags, WITHOUT --metrics_port, which this
  /// method allocates and appends). Fails with INVALID_ARGUMENT on
  /// duplicate id, RESOURCE_EXHAUSTED (error::UNKNOWN + code in message)
  /// when the port pool is empty, UNKNOWN on spawn failure.
  Status CreateEvent(const std::string& event_id,
                     const std::vector<std::string>& argv_tail,
                     int stop_timeout_seconds);

  /// drain: SIGTERM now, SIGKILL after the event's stop timeout.
  /// kill_now: SIGKILL immediately.
  Status StopEvent(const std::string& event_id, bool kill_now);

  std::optional<EventSnapshot> GetEvent(const std::string& event_id);
  std::vector<EventSnapshot> ListEvents();

  /// Drain-stops every live event and joins all monitor threads.
  void Shutdown();

 private:
  struct EventRecord {
    EventSnapshot snapshot;
    int stop_timeout_seconds = 10;
    std::thread monitor;
  };

  void MonitorEvent(EventRecord* record);   // waitpid loop, one thread/event
  int AllocatePortLocked();                 // -1 when exhausted

  const Config config_;
  std::mutex mutex_;
  std::map<std::string, std::unique_ptr<EventRecord>> events_;
  std::set<int> ports_in_use_;
  std::atomic<bool> shutting_down_{false};
};

/// One-shot HTTP GET http://127.0.0.1:port/path with |timeout_ms|;
/// returns body, or std::nullopt on connect/read failure. Used for
/// readiness probing and the metrics proxy.
std::optional<std::string> HttpGetLocal(int port, const std::string& path,
                                        int timeout_ms);

}  // namespace webapi
}  // namespace shaka

#endif  // PACKAGER_WEBAPI_SERVICE_EVENT_MANAGER_H_

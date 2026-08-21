// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/webapi/service/event_manager.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <ctime>

#include <absl/strings/str_cat.h>

namespace shaka {
namespace webapi {
namespace {

int64_t NowUnix() {
  return static_cast<int64_t>(time(nullptr));
}

}  // namespace

std::string EventStateName(EventState state) {
  switch (state) {
    case EventState::kStarting:
      return "STARTING";
    case EventState::kRunning:
      return "RUNNING";
    case EventState::kStopping:
      return "STOPPING";
    case EventState::kStopped:
      return "STOPPED";
    case EventState::kFailed:
      return "FAILED";
  }
  return "UNKNOWN";
}

std::optional<std::string> HttpGetLocal(int port, const std::string& path,
                                        int timeout_ms) {
  const int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    return std::nullopt;
  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(sock);
    return std::nullopt;
  }
  const std::string request = absl::StrCat(
      "GET ", path, " HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n");
  if (send(sock, request.data(), request.size(), 0) !=
      static_cast<ssize_t>(request.size())) {
    close(sock);
    return std::nullopt;
  }
  std::string response;
  char buffer[4096];
  ssize_t n;
  while ((n = recv(sock, buffer, sizeof(buffer), 0)) > 0)
    response.append(buffer, n);
  close(sock);
  if (response.empty())
    return std::nullopt;
  // Strip the header block; return the body.
  const size_t body = response.find("\r\n\r\n");
  return body == std::string::npos ? response : response.substr(body + 4);
}

EventManager::EventManager(const Config& config) : config_(config) {}

EventManager::~EventManager() {
  Shutdown();
}

int EventManager::AllocatePortLocked() {
  for (int port = config_.metrics_port_min; port <= config_.metrics_port_max;
       ++port) {
    if (ports_in_use_.insert(port).second)
      return port;
  }
  return -1;
}

Status EventManager::CreateEvent(const std::string& event_id,
                                 const std::vector<std::string>& argv_tail,
                                 int stop_timeout_seconds) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutting_down_)
    return Status(error::UNKNOWN, "shutting down");
  if (events_.count(event_id))
    return Status(error::INVALID_ARGUMENT,
                  absl::StrCat("duplicate event id: ", event_id));
  const int metrics_port = AllocatePortLocked();
  if (metrics_port < 0)
    return Status(error::UNKNOWN, "metrics port pool exhausted");

  auto record = std::make_unique<EventRecord>();
  record->stop_timeout_seconds = stop_timeout_seconds;
  EventSnapshot& snap = record->snapshot;
  snap.id = event_id;
  snap.state = EventState::kStarting;
  snap.metrics_port = metrics_port;
  snap.created_unix = NowUnix();
  snap.log_path = absl::StrCat(config_.log_dir, "/event-", event_id, ".log");
  snap.argv.push_back(config_.packager_bin);
  snap.argv.insert(snap.argv.end(), argv_tail.begin(), argv_tail.end());
  snap.argv.push_back("--metrics_port");
  snap.argv.push_back(absl::StrCat(metrics_port));

  const int log_fd =
      open(snap.log_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (log_fd < 0) {
    ports_in_use_.erase(metrics_port);
    return Status(error::UNKNOWN,
                  absl::StrCat("cannot open log file: ", snap.log_path));
  }

  std::vector<char*> c_argv;
  for (std::string& arg : snap.argv)
    c_argv.push_back(arg.data());
  c_argv.push_back(nullptr);

  const pid_t pid = fork();
  if (pid < 0) {
    close(log_fd);
    ports_in_use_.erase(metrics_port);
    return Status(error::UNKNOWN, "fork failed");
  }
  if (pid == 0) {
    // Child: stderr (and stdout) -> log file, then exec.
    dup2(log_fd, STDERR_FILENO);
    dup2(log_fd, STDOUT_FILENO);
    close(log_fd);
    execv(c_argv[0], c_argv.data());
    _exit(127);  // exec failed
  }
  close(log_fd);
  snap.pid = pid;

  EventRecord* raw = record.get();
  record->monitor = std::thread(&EventManager::MonitorEvent, this, raw);
  events_[event_id] = std::move(record);
  return Status::OK;
}

void EventManager::MonitorEvent(EventRecord* record) {
  // Readiness phase: RUNNING on first metrics response, or after the
  // readiness timeout if the child is still alive.
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(config_.readiness_timeout_ms);
  int metrics_port;
  pid_t pid;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_port = record->snapshot.metrics_port;
    pid = record->snapshot.pid;
  }
  bool child_exited = false;
  int wait_status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t done = waitpid(pid, &wait_status, WNOHANG);
    if (done == pid) {
      child_exited = true;
      break;
    }
    if (HttpGetLocal(metrics_port, "/metrics", 100).has_value())
      break;  // ready
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  if (!child_exited) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (record->snapshot.state == EventState::kStarting) {
        record->snapshot.state = EventState::kRunning;
        record->snapshot.started_unix = NowUnix();
      }
    }
    // Blocking wait for exit.
    waitpid(pid, &wait_status, 0);
  }

  std::lock_guard<std::mutex> lock(mutex_);
  EventSnapshot& snap = record->snapshot;
  snap.stopped_unix = NowUnix();
  if (WIFEXITED(wait_status))
    snap.exit_code = WEXITSTATUS(wait_status);
  else if (WIFSIGNALED(wait_status))
    snap.exit_code = 128 + WTERMSIG(wait_status);
  // A stop we initiated (state kStopping, including SIGKILL escalation)
  // lands in kStopped, as does a clean exit-0; anything else that dies on
  // its own is kFailed.
  if (snap.state == EventState::kStopping ||
      (snap.exit_code.has_value() && snap.exit_code.value() == 0)) {
    snap.state = EventState::kStopped;
  } else {
    snap.state = EventState::kFailed;
  }
  ports_in_use_.erase(snap.metrics_port);
}

Status EventManager::StopEvent(const std::string& event_id, bool kill_now) {
  pid_t pid = -1;
  int timeout_seconds = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = events_.find(event_id);
    if (it == events_.end())
      return Status(error::INVALID_ARGUMENT,
                    absl::StrCat("unknown event: ", event_id));
    EventSnapshot& snap = it->second->snapshot;
    if (snap.state == EventState::kStopped ||
        snap.state == EventState::kFailed) {
      return Status::OK;  // idempotent
    }
    snap.state = EventState::kStopping;
    pid = snap.pid;
    timeout_seconds = it->second->stop_timeout_seconds;
  }
  if (kill_now) {
    kill(pid, SIGKILL);
    return Status::OK;
  }
  kill(pid, SIGTERM);
  // Escalation watchdog: detached thread; harmless if the pid is gone.
  std::thread([pid, timeout_seconds] {
    std::this_thread::sleep_for(std::chrono::seconds(timeout_seconds));
    kill(pid, SIGKILL);  // no-op (ESRCH) if already reaped
  }).detach();
  return Status::OK;
}

std::optional<EventSnapshot> EventManager::GetEvent(
    const std::string& event_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = events_.find(event_id);
  if (it == events_.end())
    return std::nullopt;
  return it->second->snapshot;
}

std::vector<EventSnapshot> EventManager::ListEvents() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<EventSnapshot> result;
  result.reserve(events_.size());
  for (const auto& entry : events_)
    result.push_back(entry.second->snapshot);
  return result;
}

void EventManager::Shutdown() {
  shutting_down_ = true;
  std::vector<std::string> ids;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : events_) {
      const EventState state = entry.second->snapshot.state;
      if (state == EventState::kStarting || state == EventState::kRunning)
        ids.push_back(entry.first);
    }
  }
  for (const std::string& id : ids)
    StopEvent(id, /*kill_now=*/false);
  // Join OUTSIDE the lock: monitor threads take mutex_ in their final
  // block, so joining while holding it deadlocks (ShutdownStopsEverything
  // hangs if this is wrong). Move the threads out under the lock first.
  std::vector<std::thread> monitors;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : events_) {
      if (entry.second->monitor.joinable())
        monitors.push_back(std::move(entry.second->monitor));
    }
  }
  for (std::thread& monitor : monitors)
    monitor.join();
}

}  // namespace webapi
}  // namespace shaka

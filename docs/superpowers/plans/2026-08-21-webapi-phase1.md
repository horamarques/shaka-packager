# Packager WebAPI Phase 1 (Event Control) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A new `packager-api` executable serving an OpenAPI-documented REST API that creates, supervises, and stops packaging events as supervised `packager` subprocesses.

**Architecture:** oatpp 1.3.0 + oatpp-swagger provide HTTP, typed DTOs, and the generated OpenAPI/Swagger UI. A framework-free `webapi_core` library owns the domain logic (request validation, argv generation, subprocess supervision with a state machine, metrics-port pool) so it unit-tests without HTTP. Controllers are a thin translation layer.

**Tech Stack:** C++17, oatpp 1.3.0 (FetchContent, VxPackager's proven recipe), abseil, gtest, python integration test.

**Spec:** `docs/superpowers/specs/2026-08-21-webapi-design.md` (Phase 1 sections)

## Global Constraints

- oatpp and oatpp-swagger pinned to GIT_TAG `1.3.0`, both. Spec said submodules; this plan deliberately uses FetchContent inside the standard wrapper location `packager/third_party/oatpp/CMakeLists.txt` because oatpp-swagger's CMake requires the `OATPP_MODULES_LOCATION=CUSTOM` discovery dance that VxPackager already solved for FetchContent — recorded as an approved deviation.
- `webapi_core` (domain library) must NOT link oatpp — only `absl::*`, `status`, and (for its own metrics) `metrics`. Controllers/main are the only oatpp consumers.
- Upstream files modified: exactly two one-line-ish touches — `packager/third_party/CMakeLists.txt` (register oatpp wrapper) and `packager/CMakeLists.txt` (`add_subdirectory(webapi)`). Nothing else outside `packager/webapi/` and `packager/third_party/oatpp/`.
- The `packager` CLI binary is untouched.
- POSIX only: guard `add_subdirectory(webapi)` with `if(NOT MSVC)`.
- Flags and defaults (exact): `--api_port` 8088, `--api_bind_address` "0.0.0.0", `--api_token` "" (empty = auth disabled; when set, `/api/v1/*` requires `Authorization: Bearer <token>`, `/health` and `/swagger/*` stay open), `--packager_bin` (default: `packager` beside `packager-api`), `--event_metrics_port_range` "19100-19199", `--event_log_dir` (default: system temp), `--metrics_port` 0.
- Event state machine strings (exact, uppercase in JSON): `STARTING`, `RUNNING`, `STOPPING`, `STOPPED`, `FAILED`.
- Error body shape everywhere: `{"error": {"code": "...", "message": "...", "detail": "..."}}` with codes: 400 `invalid_request`, 401 `unauthorized`, 404 `not_found`, 409 `duplicate_event`, 503 `resource_exhausted`, 500 `internal`.
- Repo style: BSD header on new files, `///` Doxygen on public API of core headers, `<packager/...>` includes in core; the oatpp layer follows VxPackager's style (it is a port).
- Commits end with the implementer model's `Co-Authored-By` trailer.
- Build commands: `cmake -B build -S .` to reconfigure, `cmake --build build -t <target>`; test binaries under `build/packager/...`.
- macOS is the dev machine; everything in this phase is platform-neutral POSIX.

---

### Task 1: Vendor oatpp + oatpp-swagger

**Files:**
- Create: `packager/third_party/oatpp/CMakeLists.txt`
- Modify: `packager/third_party/CMakeLists.txt` (after the `prometheus-cpp` line)

**Interfaces:**
- Consumes: nothing.
- Produces: CMake targets `oatpp::oatpp` and `oatpp::oatpp-swagger`, plus cache variable `OATPP_SWAGGER_RES_PATH` for later tasks.

- [ ] **Step 1: Write the wrapper CMakeLists**

Create `packager/third_party/oatpp/CMakeLists.txt` (ported from VxPackager's proven root-CMake recipe, adjusted for this location):

```cmake
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

FetchContent_MakeAvailable(oatpp oatpp_swagger)

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
```

- [ ] **Step 2: Register it**

In `packager/third_party/CMakeLists.txt`, after `add_subdirectory(prometheus-cpp EXCLUDE_FROM_ALL)`, add:

```cmake
add_subdirectory(oatpp EXCLUDE_FROM_ALL)
```

Note: FetchContent needs network at configure time (same as submodule clone).

- [ ] **Step 3: Verify it builds**

```bash
cmake -B build -S .
cmake --build build -t oatpp-swagger
```

Expected: `oatpp` and `oatpp-swagger` static libs build. If `oatpp_swagger_SOURCE_DIR` is empty at wrapper evaluation, move the `OATPP_SWAGGER_RES_PATH` set below `FetchContent_MakeAvailable` (it already is — this note is for the error case) or derive it as `${CMAKE_BINARY_DIR}/_deps/oatpp_swagger-src/res`; record which form worked.

- [ ] **Step 4: Commit**

```bash
git add packager/third_party/oatpp packager/third_party/CMakeLists.txt
git commit -m "build: vendor oatpp + oatpp-swagger 1.3.0 for the web API"
```

---

### Task 2: EventManager core (subprocess supervision, no HTTP)

**Files:**
- Create: `packager/webapi/service/event_manager.h`
- Create: `packager/webapi/service/event_manager.cc`
- Create: `packager/webapi/service/event_manager_unittest.cc`
- Create: `packager/webapi/CMakeLists.txt`
- Modify: `packager/CMakeLists.txt` (subdirectory list, after `add_subdirectory(mpd)`)

**Interfaces:**
- Consumes: `shaka::Status` (`<packager/status.h>`), absl.
- Produces (used by Tasks 3–6):

```cpp
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
};

/// One-shot HTTP GET http://127.0.0.1:port/path with |timeout_ms|;
/// returns body, or std::nullopt on connect/read failure. Used for
/// readiness probing and the metrics proxy.
std::optional<std::string> HttpGetLocal(int port, const std::string& path,
                                        int timeout_ms);

}  // namespace webapi
}  // namespace shaka
```

- [ ] **Step 1: Write the failing tests**

Create `packager/webapi/service/event_manager_unittest.cc`. The fake child is a `/bin/sh` script written by the fixture; scripts cover: exit-after-sleep, TERM-handled (drain), TERM-ignored (kill escalation), immediate-crash.

```cpp
// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/webapi/service/event_manager.h>

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

#include <gtest/gtest.h>

namespace shaka {
namespace webapi {
namespace {

class EventManagerTest : public testing::Test {
 protected:
  void SetUp() override {
    char dir_template[] = "/tmp/event_manager_test_XXXXXX";
    ASSERT_NE(nullptr, mkdtemp(dir_template));
    temp_dir_ = dir_template;
  }
  void TearDown() override {
    std::string cmd = "rm -rf " + temp_dir_;
    system(cmd.c_str());
  }

  // Writes an executable shell script and returns its path.
  std::string WriteScript(const std::string& name, const std::string& body) {
    const std::string path = temp_dir_ + "/" + name;
    std::ofstream out(path);
    out << "#!/bin/sh\n" << body;
    out.close();
    chmod(path.c_str(), 0755);
    return path;
  }

  EventManager::Config MakeConfig(const std::string& bin) {
    EventManager::Config config;
    config.packager_bin = bin;
    config.log_dir = temp_dir_;
    config.metrics_port_min = 21100;
    config.metrics_port_max = 21102;  // pool of 3 for exhaustion tests
    config.readiness_timeout_ms = 200;
    return config;
  }

  // Polls GetEvent until |state| or 5s deadline.
  bool WaitForState(EventManager* manager, const std::string& id,
                    EventState state) {
    for (int i = 0; i < 100; ++i) {
      auto snap = manager->GetEvent(id);
      if (snap && snap->state == state)
        return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
  }

  std::string temp_dir_;
};

TEST_F(EventManagerTest, LifecycleRunningThenDrainStop) {
  // Child logs a line, handles TERM by exiting 0, sleeps forever.
  const std::string bin = WriteScript(
      "child.sh", "echo hello-from-child >&2\ntrap 'exit 0' TERM\n"
                  "while true; do sleep 0.1; done\n");
  EventManager manager(MakeConfig(bin));
  ASSERT_TRUE(manager.CreateEvent("ev1", {"--fake_flag", "x"}, 5).ok());

  ASSERT_TRUE(WaitForState(&manager, "ev1", EventState::kRunning));
  auto snap = manager.GetEvent("ev1");
  ASSERT_TRUE(snap.has_value());
  EXPECT_GT(snap->pid, 0);
  EXPECT_GE(snap->metrics_port, 21100);
  EXPECT_LE(snap->metrics_port, 21102);
  // argv_tail is preserved and --metrics_port appended.
  EXPECT_EQ("--fake_flag", snap->argv[1]);
  EXPECT_EQ("--metrics_port", snap->argv[3]);

  // stderr reached the log file.
  std::ifstream log(snap->log_path);
  std::string line;
  std::getline(log, line);
  EXPECT_EQ("hello-from-child", line);

  ASSERT_TRUE(manager.StopEvent("ev1", /*kill_now=*/false).ok());
  ASSERT_TRUE(WaitForState(&manager, "ev1", EventState::kStopped));
  EXPECT_EQ(0, manager.GetEvent("ev1")->exit_code.value());
}

TEST_F(EventManagerTest, KillEscalationWhenTermIgnored) {
  const std::string bin = WriteScript(
      "stubborn.sh", "trap '' TERM\nwhile true; do sleep 0.1; done\n");
  EventManager manager(MakeConfig(bin));
  ASSERT_TRUE(manager.CreateEvent("ev1", {}, /*stop_timeout_seconds=*/1).ok());
  ASSERT_TRUE(WaitForState(&manager, "ev1", EventState::kRunning));

  ASSERT_TRUE(manager.StopEvent("ev1", /*kill_now=*/false).ok());
  // TERM ignored -> after 1s timeout the manager SIGKILLs.
  ASSERT_TRUE(WaitForState(&manager, "ev1", EventState::kStopped));
  // SIGKILL death is reported as 128+9.
  EXPECT_EQ(137, manager.GetEvent("ev1")->exit_code.value());
}

TEST_F(EventManagerTest, CrashBecomesFailedWithExitCode) {
  const std::string bin = WriteScript("crash.sh", "exit 7\n");
  EventManager manager(MakeConfig(bin));
  ASSERT_TRUE(manager.CreateEvent("ev1", {}, 5).ok());
  ASSERT_TRUE(WaitForState(&manager, "ev1", EventState::kFailed));
  EXPECT_EQ(7, manager.GetEvent("ev1")->exit_code.value());
}

TEST_F(EventManagerTest, DuplicateIdRejectedAndPortPoolExhausts) {
  const std::string bin = WriteScript(
      "child.sh", "trap 'exit 0' TERM\nwhile true; do sleep 0.1; done\n");
  EventManager manager(MakeConfig(bin));  // pool size 3
  ASSERT_TRUE(manager.CreateEvent("dup", {}, 5).ok());
  EXPECT_FALSE(manager.CreateEvent("dup", {}, 5).ok());

  ASSERT_TRUE(manager.CreateEvent("e2", {}, 5).ok());
  ASSERT_TRUE(manager.CreateEvent("e3", {}, 5).ok());
  // Pool of 3 exhausted (dup + e2 + e3).
  EXPECT_FALSE(manager.CreateEvent("e4", {}, 5).ok());

  // Stopping an event frees its port for reuse.
  ASSERT_TRUE(manager.StopEvent("e3", /*kill_now=*/true).ok());
  ASSERT_TRUE(WaitForState(&manager, "e3", EventState::kStopped));
  EXPECT_TRUE(manager.CreateEvent("e5", {}, 5).ok());
}

TEST_F(EventManagerTest, ShutdownStopsEverything) {
  const std::string bin = WriteScript(
      "child.sh", "trap 'exit 0' TERM\nwhile true; do sleep 0.1; done\n");
  auto manager = std::make_unique<EventManager>(MakeConfig(bin));
  ASSERT_TRUE(manager->CreateEvent("a", {}, 2).ok());
  ASSERT_TRUE(manager->CreateEvent("b", {}, 2).ok());
  ASSERT_TRUE(WaitForState(manager.get(), "a", EventState::kRunning));
  ASSERT_TRUE(WaitForState(manager.get(), "b", EventState::kRunning));
  manager->Shutdown();
  EXPECT_EQ(EventState::kStopped, manager->GetEvent("a")->state);
  EXPECT_EQ(EventState::kStopped, manager->GetEvent("b")->state);
}

TEST(EventStateNameTest, MatchesApiStrings) {
  EXPECT_EQ("STARTING", EventStateName(EventState::kStarting));
  EXPECT_EQ("RUNNING", EventStateName(EventState::kRunning));
  EXPECT_EQ("STOPPING", EventStateName(EventState::kStopping));
  EXPECT_EQ("STOPPED", EventStateName(EventState::kStopped));
  EXPECT_EQ("FAILED", EventStateName(EventState::kFailed));
}

}  // namespace
}  // namespace webapi
}  // namespace shaka
```

- [ ] **Step 2: Write the CMake and register the directory; verify RED**

Create `packager/webapi/CMakeLists.txt`:

```cmake
# Copyright 2026 Google LLC. All rights reserved.
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file or at
# https://developers.google.com/open-source/licenses/bsd

add_library(webapi_core STATIC
    service/event_manager.cc)
target_link_libraries(webapi_core
    absl::strings
    absl::synchronization
    status)

add_executable(webapi_core_unittest
    service/event_manager_unittest.cc)
target_link_libraries(webapi_core_unittest
    gtest
    gtest_main
    webapi_core)
add_gtest(webapi_core_unittest)
```

In `packager/CMakeLists.txt`, in the subdirectory list after `add_subdirectory(mpd)`:

```cmake
if(NOT MSVC)
  add_subdirectory(webapi)
endif()
```

Run: `cmake -B build -S . && cmake --build build -t webapi_core_unittest`
Expected: FAIL — `event_manager.h` does not exist.

- [ ] **Step 3: Implement EventManager**

Create `packager/webapi/service/event_manager.h` with exactly the interface from the Interfaces block above (BSD header, `///` comments, include guard `PACKAGER_WEBAPI_SERVICE_EVENT_MANAGER_H_`, includes: `<atomic>`, `<map>`, `<memory>`, `<mutex>`, `<optional>`, `<set>`, `<string>`, `<thread>`, `<vector>`, `<packager/status.h>`). The private section:

```cpp
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
```

Create `packager/webapi/service/event_manager.cc`:

```cpp
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
```

Behavioral notes: `StopEvent(drain)` sets `kStopping`, so a SIGKILL death (137) after escalation correctly resolves to `kStopped` — exactly what the `KillEscalationWhenTermIgnored` test asserts.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build -t webapi_core_unittest && ./build/packager/webapi/webapi_core_unittest
```

Expected: all 6 tests PASS in under ~15 seconds total. Flakiness in the escalation test usually means the watchdog fired before `waitpid` observed the TERM-ignored state — the fixed 1s timeout and 5s wait ceiling are sized to avoid it.

- [ ] **Step 5: Commit**

```bash
git add packager/webapi packager/CMakeLists.txt
git commit -m "feat: EventManager subprocess supervision core for the web API"
```

---

### Task 3: Event request model, validation, argv generation

**Files:**
- Create: `packager/webapi/service/event_spec.h`
- Create: `packager/webapi/service/event_spec.cc`
- Create: `packager/webapi/service/event_spec_unittest.cc`
- Modify: `packager/webapi/CMakeLists.txt` (add sources/test)

**Interfaces:**
- Consumes: nothing from Task 2 (parallel-safe).
- Produces (used by Tasks 4–5):

```cpp
namespace shaka {
namespace webapi {

struct StreamSpec {
  std::string input;             // required
  std::string stream;            // required (selector: video/audio/text/0..)
  std::string init_segment;      // optional
  std::string segment_template;  // required for live
  std::string output;            // optional (VOD single-file)
};

struct EncryptionKeySpec {
  std::string label;   // may be empty
  std::string key_id;  // 32 hex chars
  std::string key;     // 32 hex chars
};

struct EncryptionSpec {
  std::string scheme;  // "cenc" | "cbcs"
  std::vector<EncryptionKeySpec> keys;
  std::string iv;      // optional; 16 or 32 hex chars
  int clear_lead = 0;
};

struct EventCreateRequest {
  std::string event_id;                       // optional; empty = generate
  std::vector<StreamSpec> streams;            // required, >= 1
  std::string mpd_output;                     // at least one output required
  std::string hls_master_playlist_output;
  std::string hls_playlist_type;              // LIVE|EVENT|VOD when HLS set
  int segment_duration = 6;
  int time_shift_buffer_depth = 0;            // 0 = omit flag
  std::optional<EncryptionSpec> encryption;
  std::vector<std::string> extra_args;
  int stop_timeout_seconds = 10;
};

/// Returns "" when valid, else "<field>: <problem>".
std::string ValidateEventRequest(const EventCreateRequest& request);

/// argv AFTER the binary path, WITHOUT --metrics_port (EventManager appends
/// that). Stream descriptors first, then flags.
std::vector<std::string> BuildEventArgv(const EventCreateRequest& request);

/// 32-hex-char lowercase random id, for requests without event_id.
std::string GenerateEventId();

}  // namespace webapi
}  // namespace shaka
```

- [ ] **Step 1: Write the failing tests**

Create `packager/webapi/service/event_spec_unittest.cc`:

```cpp
// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/webapi/service/event_spec.h>

#include <gtest/gtest.h>

namespace shaka {
namespace webapi {
namespace {

EventCreateRequest ValidLiveRequest() {
  EventCreateRequest request;
  request.event_id = "sport1";
  StreamSpec video;
  video.input = "udp://239.1.1.1:5000";
  video.stream = "video";
  video.init_segment = "/out/video_init.mp4";
  video.segment_template = "/out/video_$Number$.m4s";
  request.streams.push_back(video);
  request.mpd_output = "/out/output.mpd";
  request.segment_duration = 2;
  request.time_shift_buffer_depth = 30;
  return request;
}

TEST(ValidateEventRequestTest, AcceptsValidAndNamesBrokenField) {
  EXPECT_EQ("", ValidateEventRequest(ValidLiveRequest()));

  EventCreateRequest no_streams = ValidLiveRequest();
  no_streams.streams.clear();
  EXPECT_NE(std::string::npos,
            ValidateEventRequest(no_streams).find("streams"));

  EventCreateRequest no_output = ValidLiveRequest();
  no_output.mpd_output.clear();
  EXPECT_NE(std::string::npos,
            ValidateEventRequest(no_output).find("output"));

  EventCreateRequest bad_duration = ValidLiveRequest();
  bad_duration.segment_duration = 0;
  EXPECT_NE(std::string::npos,
            ValidateEventRequest(bad_duration).find("segment_duration"));

  EventCreateRequest bad_key = ValidLiveRequest();
  EncryptionSpec enc;
  enc.scheme = "cenc";
  enc.keys.push_back({"", "notahexkey", "00112233445566778899aabbccddeeff"});
  bad_key.encryption = enc;
  EXPECT_NE(std::string::npos, ValidateEventRequest(bad_key).find("key_id"));

  EventCreateRequest bad_scheme = ValidLiveRequest();
  EncryptionSpec enc2;
  enc2.scheme = "aes-9000";
  enc2.keys.push_back({"", "00112233445566778899aabbccddeeff",
                       "00112233445566778899aabbccddeeff"});
  bad_scheme.encryption = enc2;
  EXPECT_NE(std::string::npos,
            ValidateEventRequest(bad_scheme).find("scheme"));
}

TEST(BuildEventArgvTest, GoldenLiveDashWithEncryption) {
  EventCreateRequest request = ValidLiveRequest();
  EncryptionSpec enc;
  enc.scheme = "cbcs";
  enc.keys.push_back({"", "11111111111111111111111111111111",
                      "22222222222222222222222222222222"});
  enc.iv = "33333333333333333333333333333333";
  enc.clear_lead = 0;
  request.encryption = enc;
  request.extra_args = {"--dump_stream_info"};

  const std::vector<std::string> argv = BuildEventArgv(request);
  const std::vector<std::string> expected = {
      "input=udp://239.1.1.1:5000,stream=video,"
      "init_segment=/out/video_init.mp4,"
      "segment_template=/out/video_$Number$.m4s",
      "--mpd_output", "/out/output.mpd",
      "--segment_duration", "2",
      "--time_shift_buffer_depth", "30",
      "--enable_raw_key_encryption",
      "--protection_scheme", "cbcs",
      "--keys",
      "label=:key_id=11111111111111111111111111111111:"
      "key=22222222222222222222222222222222",
      "--iv", "33333333333333333333333333333333",
      "--clear_lead", "0",
      "--dump_stream_info",
  };
  EXPECT_EQ(expected, argv);
}

TEST(BuildEventArgvTest, HlsOnlyOmitsUnsetFlags) {
  EventCreateRequest request = ValidLiveRequest();
  request.mpd_output.clear();
  request.hls_master_playlist_output = "/out/master.m3u8";
  request.hls_playlist_type = "LIVE";
  request.time_shift_buffer_depth = 0;

  const std::vector<std::string> argv = BuildEventArgv(request);
  const std::vector<std::string> expected = {
      "input=udp://239.1.1.1:5000,stream=video,"
      "init_segment=/out/video_init.mp4,"
      "segment_template=/out/video_$Number$.m4s",
      "--hls_master_playlist_output", "/out/master.m3u8",
      "--hls_playlist_type", "LIVE",
      "--segment_duration", "2",
  };
  EXPECT_EQ(expected, argv);
}

TEST(GenerateEventIdTest, ShapeAndUniqueness) {
  const std::string a = GenerateEventId();
  const std::string b = GenerateEventId();
  EXPECT_EQ(32u, a.size());
  EXPECT_NE(a, b);
  for (char c : a)
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
}

}  // namespace
}  // namespace webapi
}  // namespace shaka
```

- [ ] **Step 2: Register in CMake, verify RED**

Add `service/event_spec.cc` to `webapi_core` sources and `service/event_spec_unittest.cc` to `webapi_core_unittest`. Build: expected FAIL (header missing).

- [ ] **Step 3: Implement**

`packager/webapi/service/event_spec.h`: exactly the Interfaces block (BSD header, guard `PACKAGER_WEBAPI_SERVICE_EVENT_SPEC_H_`).

`packager/webapi/service/event_spec.cc`:

```cpp
// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/webapi/service/event_spec.h>

#include <random>

#include <absl/strings/str_cat.h>

namespace shaka {
namespace webapi {
namespace {

bool IsHex(const std::string& value, size_t length) {
  if (value.size() != length)
    return false;
  for (char c : value) {
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F');
    if (!hex)
      return false;
  }
  return true;
}

}  // namespace

std::string ValidateEventRequest(const EventCreateRequest& request) {
  if (request.streams.empty())
    return "streams: at least one stream is required";
  for (size_t i = 0; i < request.streams.size(); ++i) {
    const StreamSpec& stream = request.streams[i];
    if (stream.input.empty())
      return absl::StrCat("streams[", i, "].input: required");
    if (stream.stream.empty())
      return absl::StrCat("streams[", i, "].stream: required");
    if (stream.segment_template.empty() && stream.output.empty())
      return absl::StrCat("streams[", i,
                          "].segment_template: required (or output)");
  }
  if (request.mpd_output.empty() && request.hls_master_playlist_output.empty())
    return "output: mpd_output or hls_master_playlist_output is required";
  if (!request.hls_master_playlist_output.empty() &&
      request.hls_playlist_type.empty())
    return "hls_playlist_type: required with hls_master_playlist_output";
  if (request.segment_duration <= 0)
    return "segment_duration: must be positive";
  if (request.time_shift_buffer_depth < 0)
    return "time_shift_buffer_depth: must be >= 0";
  if (request.stop_timeout_seconds <= 0)
    return "stop_timeout_seconds: must be positive";
  if (request.encryption.has_value()) {
    const EncryptionSpec& enc = request.encryption.value();
    if (enc.scheme != "cenc" && enc.scheme != "cbcs")
      return "encryption.scheme: must be cenc or cbcs";
    if (enc.keys.empty())
      return "encryption.keys: at least one key is required";
    for (size_t i = 0; i < enc.keys.size(); ++i) {
      if (!IsHex(enc.keys[i].key_id, 32))
        return absl::StrCat("encryption.keys[", i,
                            "].key_id: must be 32 hex chars");
      if (!IsHex(enc.keys[i].key, 32))
        return absl::StrCat("encryption.keys[", i,
                            "].key: must be 32 hex chars");
    }
    if (!enc.iv.empty() && !IsHex(enc.iv, 16) && !IsHex(enc.iv, 32))
      return "encryption.iv: must be 16 or 32 hex chars";
  }
  return "";
}

std::vector<std::string> BuildEventArgv(const EventCreateRequest& request) {
  std::vector<std::string> argv;
  for (const StreamSpec& stream : request.streams) {
    std::string descriptor =
        absl::StrCat("input=", stream.input, ",stream=", stream.stream);
    if (!stream.init_segment.empty())
      absl::StrAppend(&descriptor, ",init_segment=", stream.init_segment);
    if (!stream.segment_template.empty())
      absl::StrAppend(&descriptor,
                      ",segment_template=", stream.segment_template);
    if (!stream.output.empty())
      absl::StrAppend(&descriptor, ",output=", stream.output);
    argv.push_back(descriptor);
  }
  if (!request.mpd_output.empty()) {
    argv.push_back("--mpd_output");
    argv.push_back(request.mpd_output);
  }
  if (!request.hls_master_playlist_output.empty()) {
    argv.push_back("--hls_master_playlist_output");
    argv.push_back(request.hls_master_playlist_output);
    argv.push_back("--hls_playlist_type");
    argv.push_back(request.hls_playlist_type);
  }
  argv.push_back("--segment_duration");
  argv.push_back(absl::StrCat(request.segment_duration));
  if (request.time_shift_buffer_depth > 0) {
    argv.push_back("--time_shift_buffer_depth");
    argv.push_back(absl::StrCat(request.time_shift_buffer_depth));
  }
  if (request.encryption.has_value()) {
    const EncryptionSpec& enc = request.encryption.value();
    argv.push_back("--enable_raw_key_encryption");
    argv.push_back("--protection_scheme");
    argv.push_back(enc.scheme);
    std::string keys;
    for (size_t i = 0; i < enc.keys.size(); ++i) {
      if (i > 0)
        absl::StrAppend(&keys, ",");
      absl::StrAppend(&keys, "label=", enc.keys[i].label,
                      ":key_id=", enc.keys[i].key_id,
                      ":key=", enc.keys[i].key);
    }
    argv.push_back("--keys");
    argv.push_back(keys);
    if (!enc.iv.empty()) {
      argv.push_back("--iv");
      argv.push_back(enc.iv);
    }
    argv.push_back("--clear_lead");
    argv.push_back(absl::StrCat(enc.clear_lead));
  }
  for (const std::string& arg : request.extra_args)
    argv.push_back(arg);
  return argv;
}

std::string GenerateEventId() {
  static const char kHex[] = "0123456789abcdef";
  std::random_device device;
  std::mt19937_64 rng(device());
  std::uniform_int_distribution<int> dist(0, 15);
  std::string id;
  id.reserve(32);
  for (int i = 0; i < 32; ++i)
    id.push_back(kHex[dist(rng)]);
  return id;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build -t webapi_core_unittest && ./build/packager/webapi/webapi_core_unittest --gtest_filter='*EventRequest*:*BuildEventArgv*:*GenerateEventId*'
```

Expected: PASS. Then the full `webapi_core_unittest` — all PASS. Cross-check the flag names against the real binary once: `./build/packager/packager --help 2>&1 | grep -E "enable_raw_key_encryption|protection_scheme|clear_lead|time_shift"` — all four must exist; if a name differs, fix `BuildEventArgv` AND the golden test to the real flag and note it in the report.

- [ ] **Step 5: Commit**

```bash
git add packager/webapi
git commit -m "feat: event request validation and packager argv generation"
```

---

### Task 4: packager-api server (DTOs, controllers, main, swagger)

**Files:**
- Create: `packager/webapi/model/event_dto.hpp`
- Create: `packager/webapi/controller/health_controller.hpp`
- Create: `packager/webapi/controller/event_controller.hpp`
- Create: `packager/webapi/main.cc`
- Modify: `packager/webapi/CMakeLists.txt`

**Interfaces:**
- Consumes: `EventManager`, `EventSnapshot`, `EventStateName`, `HttpGetLocal` (Task 2); `EventCreateRequest`, `ValidateEventRequest`, `BuildEventArgv`, `GenerateEventId` (Task 3); oatpp targets (Task 1); `GetPackagerVersion()` (`<packager/version/version.h>`).
- Produces: the `packager-api` executable serving `/health`, `/api/v1/events` CRUD, `/api/v1/events/{id}/logs`, `/api/v1/events/{id}/metrics`, `/swagger/ui`, `/swagger/doc`. Task 5 adds auth + API self-metrics; Task 6 tests everything over HTTP.

This is the VxPackager port. oatpp code lives in `.hpp` files with inline endpoint bodies (VxPackager's controller style, kept — the codegen macros make split declarations noisy).

- [ ] **Step 1: Write the DTOs**

Create `packager/webapi/model/event_dto.hpp`:

```cpp
// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

namespace shaka {
namespace webapi {

#include OATPP_CODEGEN_BEGIN(DTO)

class StreamSpecDto : public oatpp::DTO {
  DTO_INIT(StreamSpecDto, DTO)
  DTO_FIELD(String, input);
  DTO_FIELD(String, stream);
  DTO_FIELD(String, init_segment);
  DTO_FIELD(String, segment_template);
  DTO_FIELD(String, output);
};

class EncryptionKeyDto : public oatpp::DTO {
  DTO_INIT(EncryptionKeyDto, DTO)
  DTO_FIELD(String, label) = "";
  DTO_FIELD(String, key_id);
  DTO_FIELD(String, key);
};

class EncryptionDto : public oatpp::DTO {
  DTO_INIT(EncryptionDto, DTO)
  DTO_FIELD(String, scheme) = "cenc";
  DTO_FIELD(List<Object<EncryptionKeyDto>>, keys);
  DTO_FIELD(String, iv);
  DTO_FIELD(Int32, clear_lead) = 0;
};

class EventCreateDto : public oatpp::DTO {
  DTO_INIT(EventCreateDto, DTO)
  DTO_FIELD(String, event_id);
  DTO_FIELD(List<Object<StreamSpecDto>>, streams);
  DTO_FIELD(String, mpd_output);
  DTO_FIELD(String, hls_master_playlist_output);
  DTO_FIELD(String, hls_playlist_type);
  DTO_FIELD(Int32, segment_duration) = 6;
  DTO_FIELD(Int32, time_shift_buffer_depth) = 0;
  DTO_FIELD(Object<EncryptionDto>, encryption);
  DTO_FIELD(List<String>, extra_args);
  DTO_FIELD(Int32, stop_timeout_seconds) = 10;
};

class EventStatusDto : public oatpp::DTO {
  DTO_INIT(EventStatusDto, DTO)
  DTO_FIELD(String, event_id);
  DTO_FIELD(String, state);
  DTO_FIELD(Int32, pid);
  DTO_FIELD(Int32, exit_code);          // present only when terminal
  DTO_FIELD(Int32, metrics_port);
  DTO_FIELD(String, log_path);
  DTO_FIELD(List<String>, argv);
  DTO_FIELD(Int64, created_unix);
  DTO_FIELD(Int64, started_unix);
  DTO_FIELD(Int64, stopped_unix);
  DTO_FIELD(Int64, uptime_seconds);
};

class EventListDto : public oatpp::DTO {
  DTO_INIT(EventListDto, DTO)
  DTO_FIELD(List<Object<EventStatusDto>>, events);
};

class ErrorDetailDto : public oatpp::DTO {
  DTO_INIT(ErrorDetailDto, DTO)
  DTO_FIELD(String, code);
  DTO_FIELD(String, message);
  DTO_FIELD(String, detail);
};

class ErrorDto : public oatpp::DTO {
  DTO_INIT(ErrorDto, DTO)
  DTO_FIELD(Object<ErrorDetailDto>, error);
};

class HealthDto : public oatpp::DTO {
  DTO_INIT(HealthDto, DTO)
  DTO_FIELD(String, status) = "ok";
  DTO_FIELD(String, packager_version);
};

#include OATPP_CODEGEN_END(DTO)

}  // namespace webapi
}  // namespace shaka
```

- [ ] **Step 2: Write the controllers**

Create `packager/webapi/controller/health_controller.hpp`:

```cpp
// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#pragma once

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include <packager/version/version.h>
#include <packager/webapi/model/event_dto.hpp>

namespace shaka {
namespace webapi {

class HealthController : public oatpp::web::server::api::ApiController {
 public:
  explicit HealthController(
      const std::shared_ptr<oatpp::parser::json::mapping::ObjectMapper>&
          object_mapper)
      : oatpp::web::server::api::ApiController(object_mapper) {}

#include OATPP_CODEGEN_BEGIN(ApiController)

  ENDPOINT_INFO(health) {
    info->summary = "Liveness and packager version";
    info->addResponse<Object<HealthDto>>(Status::CODE_200, "application/json");
  }
  ENDPOINT("GET", "/health", health) {
    auto dto = HealthDto::createShared();
    dto->packager_version = GetPackagerVersion();
    return createDtoResponse(Status::CODE_200, dto);
  }

#include OATPP_CODEGEN_END(ApiController)
};

}  // namespace webapi
}  // namespace shaka
```

Create `packager/webapi/controller/event_controller.hpp`:

```cpp
// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#pragma once

#include <ctime>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include <packager/webapi/model/event_dto.hpp>
#include <packager/webapi/service/event_manager.h>
#include <packager/webapi/service/event_spec.h>

namespace shaka {
namespace webapi {

class EventController : public oatpp::web::server::api::ApiController {
 public:
  EventController(
      const std::shared_ptr<oatpp::parser::json::mapping::ObjectMapper>&
          object_mapper,
      std::shared_ptr<EventManager> event_manager)
      : oatpp::web::server::api::ApiController(object_mapper),
        event_manager_(std::move(event_manager)) {}

#include OATPP_CODEGEN_BEGIN(ApiController)

  ENDPOINT_INFO(createEvent) {
    info->summary = "Create and start a packaging event";
    info->addConsumes<Object<EventCreateDto>>("application/json");
    info->addResponse<Object<EventStatusDto>>(Status::CODE_201,
                                              "application/json");
    info->addResponse<Object<ErrorDto>>(Status::CODE_400, "application/json");
    info->addResponse<Object<ErrorDto>>(Status::CODE_409, "application/json");
    info->addResponse<Object<ErrorDto>>(Status::CODE_503, "application/json");
  }
  ENDPOINT("POST", "/api/v1/events", createEvent,
           BODY_DTO(Object<EventCreateDto>, body)) {
    EventCreateRequest request = FromDto(body);
    const std::string validation = ValidateEventRequest(request);
    if (!validation.empty())
      return Error(Status::CODE_400, "invalid_request", validation);
    if (request.event_id.empty())
      request.event_id = GenerateEventId();

    const shaka::Status status = event_manager_->CreateEvent(
        request.event_id, BuildEventArgv(request),
        request.stop_timeout_seconds);
    if (!status.ok()) {
      const std::string message = status.error_message();
      if (message.find("duplicate") != std::string::npos)
        return Error(Status::CODE_409, "duplicate_event", message);
      if (message.find("exhausted") != std::string::npos)
        return Error(Status::CODE_503, "resource_exhausted", message);
      return Error(Status::CODE_500, "internal", message);
    }
    return createDtoResponse(Status::CODE_201,
                             ToDto(*event_manager_->GetEvent(request.event_id)));
  }

  ENDPOINT_INFO(listEvents) {
    info->summary = "List packaging events";
    info->addResponse<Object<EventListDto>>(Status::CODE_200,
                                            "application/json");
  }
  ENDPOINT("GET", "/api/v1/events", listEvents) {
    auto dto = EventListDto::createShared();
    dto->events = oatpp::List<oatpp::Object<EventStatusDto>>::createShared();
    for (const EventSnapshot& snap : event_manager_->ListEvents())
      dto->events->push_back(ToDto(snap));
    return createDtoResponse(Status::CODE_200, dto);
  }

  ENDPOINT_INFO(getEvent) {
    info->summary = "Get one event's status";
    info->addResponse<Object<EventStatusDto>>(Status::CODE_200,
                                              "application/json");
    info->addResponse<Object<ErrorDto>>(Status::CODE_404, "application/json");
  }
  ENDPOINT("GET", "/api/v1/events/{event_id}", getEvent,
           PATH(String, event_id)) {
    auto snap = event_manager_->GetEvent(event_id->std_str());
    if (!snap.has_value())
      return Error(Status::CODE_404, "not_found", "unknown event");
    return createDtoResponse(Status::CODE_200, ToDto(*snap));
  }

  ENDPOINT_INFO(stopEvent) {
    info->summary = "Stop an event (mode=drain default, mode=kill immediate)";
    info->addResponse<Object<EventStatusDto>>(Status::CODE_202,
                                              "application/json");
    info->addResponse<Object<ErrorDto>>(Status::CODE_404, "application/json");
  }
  ENDPOINT("DELETE", "/api/v1/events/{event_id}", stopEvent,
           PATH(String, event_id), QUERY(String, mode, "drain")) {
    const bool kill_now = mode->std_str() == "kill";
    const shaka::Status status =
        event_manager_->StopEvent(event_id->std_str(), kill_now);
    if (!status.ok())
      return Error(Status::CODE_404, "not_found", status.error_message());
    return createDtoResponse(Status::CODE_202,
                             ToDto(*event_manager_->GetEvent(
                                 event_id->std_str())));
  }

  ENDPOINT_INFO(getLogs) {
    info->summary = "Tail of the event's stderr log";
  }
  ENDPOINT("GET", "/api/v1/events/{event_id}/logs", getLogs,
           PATH(String, event_id), QUERY(Int32, tail, 100)) {
    auto snap = event_manager_->GetEvent(event_id->std_str());
    if (!snap.has_value())
      return Error(Status::CODE_404, "not_found", "unknown event");
    return createResponse(Status::CODE_200,
                          TailFile(snap->log_path, *tail).c_str());
  }

  ENDPOINT_INFO(getEventMetrics) {
    info->summary = "Proxy one scrape of the event's Prometheus /metrics";
  }
  ENDPOINT("GET", "/api/v1/events/{event_id}/metrics", getEventMetrics,
           PATH(String, event_id)) {
    auto snap = event_manager_->GetEvent(event_id->std_str());
    if (!snap.has_value())
      return Error(Status::CODE_404, "not_found", "unknown event");
    auto body = HttpGetLocal(snap->metrics_port, "/metrics", 2000);
    if (!body.has_value())
      return Error(Status::CODE_503, "resource_exhausted",
                   "event metrics endpoint unreachable");
    auto response = createResponse(Status::CODE_200, body->c_str());
    response->putHeader("Content-Type", "text/plain; version=0.0.4");
    return response;
  }

#include OATPP_CODEGEN_END(ApiController)

 private:
  std::shared_ptr<oatpp::web::protocol::http::outgoing::Response> Error(
      const oatpp::web::protocol::http::Status& status,
      const std::string& code, const std::string& message) {
    auto detail = ErrorDetailDto::createShared();
    detail->code = code.c_str();
    detail->message = message.c_str();
    auto error = ErrorDto::createShared();
    error->error = detail;
    return createDtoResponse(status, error);
  }

  static EventCreateRequest FromDto(const oatpp::Object<EventCreateDto>& dto);
  oatpp::Object<EventStatusDto> ToDto(const EventSnapshot& snap) {
    auto dto = EventStatusDto::createShared();
    dto->event_id = snap.id.c_str();
    dto->state = EventStateName(snap.state).c_str();
    dto->pid = snap.pid;
    if (snap.exit_code.has_value())
      dto->exit_code = snap.exit_code.value();
    dto->metrics_port = snap.metrics_port;
    dto->log_path = snap.log_path.c_str();
    dto->argv = oatpp::List<oatpp::String>::createShared();
    for (const std::string& arg : snap.argv)
      dto->argv->push_back(arg.c_str());
    dto->created_unix = snap.created_unix;
    dto->started_unix = snap.started_unix;
    dto->stopped_unix = snap.stopped_unix;
    dto->uptime_seconds =
        snap.started_unix == 0
            ? 0
            : (snap.stopped_unix != 0 ? snap.stopped_unix : time(nullptr)) -
                  snap.started_unix;
    return dto;
  }

  static std::string TailFile(const std::string& path, int lines);

  std::shared_ptr<EventManager> event_manager_;
};

}  // namespace webapi
}  // namespace shaka
```

`FromDto` and `TailFile` go in a small `packager/webapi/controller/event_controller.cc` (they need no codegen macros):

```cpp
// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/webapi/controller/event_controller.hpp>

#include <deque>

namespace shaka {
namespace webapi {
namespace {

std::string ToStd(const oatpp::String& value) {
  return value ? value->std_str() : std::string();
}

}  // namespace

// static
EventCreateRequest EventController::FromDto(
    const oatpp::Object<EventCreateDto>& dto) {
  EventCreateRequest request;
  request.event_id = ToStd(dto->event_id);
  if (dto->streams) {
    for (const auto& stream_dto : *dto->streams) {
      StreamSpec stream;
      stream.input = ToStd(stream_dto->input);
      stream.stream = ToStd(stream_dto->stream);
      stream.init_segment = ToStd(stream_dto->init_segment);
      stream.segment_template = ToStd(stream_dto->segment_template);
      stream.output = ToStd(stream_dto->output);
      request.streams.push_back(stream);
    }
  }
  request.mpd_output = ToStd(dto->mpd_output);
  request.hls_master_playlist_output =
      ToStd(dto->hls_master_playlist_output);
  request.hls_playlist_type = ToStd(dto->hls_playlist_type);
  if (dto->segment_duration)
    request.segment_duration = *dto->segment_duration;
  if (dto->time_shift_buffer_depth)
    request.time_shift_buffer_depth = *dto->time_shift_buffer_depth;
  if (dto->encryption) {
    EncryptionSpec enc;
    enc.scheme = ToStd(dto->encryption->scheme);
    if (dto->encryption->keys) {
      for (const auto& key_dto : *dto->encryption->keys) {
        enc.keys.push_back({ToStd(key_dto->label), ToStd(key_dto->key_id),
                            ToStd(key_dto->key)});
      }
    }
    enc.iv = ToStd(dto->encryption->iv);
    if (dto->encryption->clear_lead)
      enc.clear_lead = *dto->encryption->clear_lead;
    request.encryption = enc;
  }
  if (dto->extra_args) {
    for (const auto& arg : *dto->extra_args)
      request.extra_args.push_back(ToStd(arg));
  }
  if (dto->stop_timeout_seconds)
    request.stop_timeout_seconds = *dto->stop_timeout_seconds;
  return request;
}

// static
std::string EventController::TailFile(const std::string& path, int lines) {
  std::ifstream in(path);
  std::deque<std::string> tail;
  std::string line;
  while (std::getline(in, line)) {
    tail.push_back(line);
    if (static_cast<int>(tail.size()) > lines)
      tail.pop_front();
  }
  std::string result;
  for (const std::string& kept : tail) {
    result += kept;
    result += '\n';
  }
  return result;
}

}  // namespace webapi
}  // namespace shaka
```

oatpp 1.3.0 API notes for the implementer: `oatpp::String::std_str()` exists in 1.3.0 (it was removed later — if the compiler rejects it, use `*value` / `value->c_str()` equivalents and note it); `QUERY(Int32, tail, 100)` declares a defaulted query parameter; `createResponse(Status, const char*)` makes a plain-text response.

- [ ] **Step 3: Write main.cc**

Create `packager/webapi/main.cc` (VxPackager's bootstrap, with absl flags replacing env vars):

```cpp
// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <libgen.h>
#include <unistd.h>

#include <csignal>
#include <iostream>
#include <memory>
#include <string>

#include <absl/flags/flag.h>
#include <absl/flags/parse.h>
#include <absl/log/initialize.h>
#include <absl/strings/str_split.h>
#include <oatpp-swagger/Controller.hpp>
#include <oatpp-swagger/Resources.hpp>
#include <oatpp/network/Server.hpp>
#include <oatpp/network/tcp/server/ConnectionProvider.hpp>
#include <oatpp/web/server/HttpConnectionHandler.hpp>
#include <oatpp/web/server/HttpRouter.hpp>

#include <packager/version/version.h>
#include <packager/webapi/controller/event_controller.hpp>
#include <packager/webapi/controller/health_controller.hpp>
#include <packager/webapi/service/event_manager.h>

ABSL_FLAG(int32_t, api_port, 8088, "HTTP port for the web API.");
ABSL_FLAG(std::string, api_bind_address, "0.0.0.0", "Bind address.");
ABSL_FLAG(std::string,
          api_token,
          "",
          "Static bearer token. When set, /api/v1/* requests require "
          "'Authorization: Bearer <token>'. /health and /swagger stay open.");
ABSL_FLAG(std::string,
          packager_bin,
          "",
          "Path to the packager binary spawned per event. Defaults to "
          "'packager' next to this executable.");
ABSL_FLAG(std::string,
          event_metrics_port_range,
          "19100-19199",
          "Inclusive port range allocated to per-event --metrics_port.");
ABSL_FLAG(std::string,
          event_log_dir,
          "/tmp",
          "Directory for per-event stderr log files.");

namespace shaka {
namespace webapi {
namespace {

volatile std::sig_atomic_t g_shutdown_requested = 0;

void HandleSignal(int) {
  g_shutdown_requested = 1;
}

std::string DefaultPackagerBin(const char* argv0) {
  std::string self(argv0);
  char* dir = dirname(self.data());
  return std::string(dir) + "/packager";
}

int RunServer(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  oatpp::base::Environment::init();

  EventManager::Config config;
  config.packager_bin = absl::GetFlag(FLAGS_packager_bin).empty()
                            ? DefaultPackagerBin(argv[0])
                            : absl::GetFlag(FLAGS_packager_bin);
  config.log_dir = absl::GetFlag(FLAGS_event_log_dir);
  const std::vector<std::string> range =
      absl::StrSplit(absl::GetFlag(FLAGS_event_metrics_port_range), '-');
  if (range.size() != 2) {
    std::cerr << "--event_metrics_port_range must be MIN-MAX" << std::endl;
    return 1;
  }
  config.metrics_port_min = std::stoi(range[0]);
  config.metrics_port_max = std::stoi(range[1]);

  auto event_manager = std::make_shared<EventManager>(config);

  auto router = oatpp::web::server::HttpRouter::createShared();
  auto object_mapper =
      oatpp::parser::json::mapping::ObjectMapper::createShared();
  object_mapper->getSerializer()->getConfig()->includeNullFields = false;

  auto health_controller =
      std::make_shared<HealthController>(object_mapper);
  auto event_controller =
      std::make_shared<EventController>(object_mapper, event_manager);
  router->addController(health_controller);
  router->addController(event_controller);

  auto doc_info =
      oatpp::swagger::DocumentInfo::Builder()
          .setTitle("Shaka Packager Web API")
          .setVersion(GetPackagerVersion().c_str())
          .setDescription("Event control API for packaging as a service")
          .setLicenseName("BSD")
          .setLicenseUrl(
              "https://developers.google.com/open-source/licenses/bsd")
          .build();
  auto swagger_resources =
      oatpp::swagger::Resources::loadResources(OATPP_SWAGGER_RES_PATH);
  oatpp::web::server::api::Endpoints doc_endpoints;
  doc_endpoints.append(health_controller->getEndpoints());
  doc_endpoints.append(event_controller->getEndpoints());
  router->addController(oatpp::swagger::Controller::createShared(
      doc_endpoints, doc_info, swagger_resources));

  auto connection_handler =
      oatpp::web::server::HttpConnectionHandler::createShared(router);
  auto connection_provider =
      oatpp::network::tcp::server::ConnectionProvider::createShared(
          oatpp::network::Address(
              absl::GetFlag(FLAGS_api_bind_address).c_str(),
              static_cast<uint16_t>(absl::GetFlag(FLAGS_api_port))));
  oatpp::network::Server server(connection_provider, connection_handler);

  std::signal(SIGTERM, HandleSignal);
  std::signal(SIGINT, HandleSignal);

  std::cout << "packager-api listening on "
            << absl::GetFlag(FLAGS_api_bind_address) << ":"
            << absl::GetFlag(FLAGS_api_port) << std::endl;

  server.run([&] { return g_shutdown_requested == 0; });

  std::cout << "shutting down; draining events" << std::endl;
  event_manager->Shutdown();
  oatpp::base::Environment::destroy();
  return 0;
}

}  // namespace
}  // namespace webapi
}  // namespace shaka

int main(int argc, char** argv) {
  return shaka::webapi::RunServer(argc, argv);
}
```

Implementation note: `oatpp::network::Server::run(std::function<bool()>)` exists in 1.3.0 as the conditional-run overload; if the exact signature differs, run the server on a thread and poll `g_shutdown_requested` in main, then `server.stop()` — either shape is acceptable; report which was used.

- [ ] **Step 4: Wire the executable in CMake**

Append to `packager/webapi/CMakeLists.txt`:

```cmake
add_executable(packager-api
    controller/event_controller.cc
    main.cc)
target_compile_definitions(packager-api PRIVATE
    OATPP_SWAGGER_RES_PATH="${OATPP_SWAGGER_RES_PATH}")
target_link_libraries(packager-api
    absl::flags
    absl::flags_parse
    absl::log
    absl::strings
    oatpp::oatpp
    oatpp::oatpp-swagger
    version
    webapi_core)
```

(`packager-api` deliberately does NOT link `libpackager` in phase 1 — events run in subprocesses; phase 2's ops harness adds it.)

- [ ] **Step 5: Build and smoke-test by hand**

```bash
cmake --build build -t packager-api packager
./build/packager/webapi/packager-api --api_port 18800 &
sleep 1
curl -s localhost:18800/health
curl -s localhost:18800/swagger/doc | head -c 200
curl -s -X POST localhost:18800/api/v1/events -H 'Content-Type: application/json' -d '{"streams":[]}'
kill %1
```

Expected: health JSON with a version; swagger doc starting with `{"openapi"` or `{"swagger"`; the bad create returns the 400 error body `{"error":{"code":"invalid_request",...}}`. Fix compile/API mismatches now (this step is where oatpp 1.3.0 signature drift surfaces); the automated e2e lands in Task 6.

- [ ] **Step 6: Commit**

```bash
git add packager/webapi
git commit -m "feat: packager-api server with event endpoints and swagger"
```

---

### Task 5: Bearer-token auth + API self-metrics

**Files:**
- Create: `packager/webapi/service/auth_interceptor.hpp`
- Modify: `packager/webapi/main.cc`
- Modify: `packager/webapi/controller/event_controller.hpp` (request counter)
- Modify: `packager/webapi/CMakeLists.txt` (link `metrics`)

**Interfaces:**
- Consumes: oatpp interceptor API; `MetricsService` (`<packager/metrics/metrics_service.h>`); `--metrics_port` flag pattern.
- Produces: 401 enforcement on `/api/v1/*` when `--api_token` set; `shaka_api_requests_total{route,code}` and `shaka_api_events_running` on the API's own `--metrics_port`.

- [ ] **Step 1: Write the interceptor**

Create `packager/webapi/service/auth_interceptor.hpp`:

```cpp
// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#pragma once

#include <string>

#include <oatpp/web/server/interceptor/RequestInterceptor.hpp>

namespace shaka {
namespace webapi {

/// Rejects /api/v1/* requests lacking 'Authorization: Bearer <token>' when
/// a token is configured. /health and /swagger stay open.
class AuthInterceptor
    : public oatpp::web::server::interceptor::RequestInterceptor {
 public:
  explicit AuthInterceptor(std::string token) : token_(std::move(token)) {}

  std::shared_ptr<OutgoingResponse> intercept(
      const std::shared_ptr<IncomingRequest>& request) override {
    if (token_.empty())
      return nullptr;  // auth disabled
    const auto path = request->getStartingLine().path.toString();
    if (path->std_str().rfind("/api/v1/", 0) != 0)
      return nullptr;  // open route
    const auto header = request->getHeader("Authorization");
    if (header && header->std_str() == "Bearer " + token_)
      return nullptr;
    auto response = OutgoingResponse::createShared(
        oatpp::web::protocol::http::Status::CODE_401,
        oatpp::web::protocol::http::outgoing::BufferBody::createShared(
            oatpp::String(
                "{\"error\":{\"code\":\"unauthorized\","
                "\"message\":\"missing or invalid bearer token\"}}")));
    response->putHeader("Content-Type", "application/json");
    return response;
  }

 private:
  const std::string token_;
};

}  // namespace webapi
}  // namespace shaka
```

Wire it in `main.cc` after creating `connection_handler`:

```cpp
  connection_handler->addRequestInterceptor(
      std::make_shared<AuthInterceptor>(absl::GetFlag(FLAGS_api_token)));
```

(If 1.3.0's `HttpConnectionHandler` lacks `addRequestInterceptor`, the method exists on it in 1.3.0 as `addRequestInterceptor(const std::shared_ptr<RequestInterceptor>&)`; if the build disagrees, check `oatpp/web/server/HttpProcessor.hpp` for the correct registration point and report the form used.)

- [ ] **Step 2: API self-metrics**

Add flag to `main.cc`:

```cpp
ABSL_FLAG(int32_t, metrics_port, 0,
          "Prometheus endpoint for the API process itself. 0 disables.");
```

In `RunServer` after flag parsing:

```cpp
  if (absl::GetFlag(FLAGS_metrics_port) > 0) {
    const Status metrics_status = MetricsService::Instance().StartExposer(
        absl::GetFlag(FLAGS_api_bind_address),
        absl::GetFlag(FLAGS_metrics_port));
    if (!metrics_status.ok()) {
      std::cerr << metrics_status.ToString() << std::endl;
      return 1;
    }
  }
```

and `MetricsService::Instance().StopExposer();` just before `return 0`. Include `<packager/metrics/metrics_service.h>`.

In `EventController`, add a request counter: a private helper fetching (once, in the constructor)

```cpp
  prometheus::Family<prometheus::Counter>* requests_family_ =
      &prometheus::BuildCounter()
           .Name("shaka_api_requests_total")
           .Help("API requests by route and status code.")
           .Register(MetricsService::Instance().registry());
```

and at each endpoint's return path call
`requests_family_->Add({{"route", "<route>"}, {"code", "<code>"}}).Increment();`
with route values exactly: `create_event`, `list_events`, `get_event`, `stop_event`, `get_logs`, `get_event_metrics`, and code the numeric status returned (e.g. "201"). Wrap this in a tiny private method `void Count(const char* route, int code)` so each endpoint adds one line.

Add a gauge for running events in `main.cc` via a Collectable-free approach: skip it — `shaka_api_events_running` instead comes from a callback the controller cannot own cleanly; implement it as a Gauge updated inside `Count()`:

```cpp
  prometheus::Gauge* events_running_ =
      &prometheus::BuildGauge()
           .Name("shaka_api_events_running")
           .Help("Events currently in STARTING or RUNNING state.")
           .Register(MetricsService::Instance().registry())
           .Add({});
```

updated in `Count()` with `events_running_->Set(<count of ListEvents() in kStarting/kRunning>)`. This keeps phase 1 free of new Collectable plumbing; the value refreshes on every API request, which is adequate for an ops dashboard.

CMake: add `metrics` to `packager-api`'s `target_link_libraries` (NOT to `webapi_core`).

- [ ] **Step 3: Smoke-test auth + metrics by hand**

```bash
cmake --build build -t packager-api
./build/packager/webapi/packager-api --api_port 18800 --api_token secret --metrics_port 18801 &
sleep 1
curl -s -o /dev/null -w '%{http_code}\n' localhost:18800/api/v1/events            # 401
curl -s -o /dev/null -w '%{http_code}\n' -H 'Authorization: Bearer secret' localhost:18800/api/v1/events   # 200
curl -s -o /dev/null -w '%{http_code}\n' localhost:18800/health                   # 200 (open)
curl -s localhost:18801/metrics | grep shaka_api_requests_total
kill %1
```

Expected: 401 / 200 / 200 and at least one `shaka_api_requests_total{...}` sample.

- [ ] **Step 4: Commit**

```bash
git add packager/webapi
git commit -m "feat: bearer-token auth and API self-metrics for packager-api"
```

---

### Task 6: End-to-end integration test

**Files:**
- Create: `packager/webapi/test/webapi_test.py`
- Modify: `packager/webapi/CMakeLists.txt` (ctest registration)

**Interfaces:**
- Consumes: everything above; `packager/tools/redundant_ts/replay_ts.py`; the `packager` binary; env vars `PACKAGER_API_BIN`, `PACKAGER_BIN`, `PACKAGER_SRC_DIR` (set by CMake below).
- Produces: automated coverage of the full event lifecycle over HTTP.

- [ ] **Step 1: Write the test**

Create `packager/webapi/test/webapi_test.py`:

```python
#!/usr/bin/env python3
# Copyright 2026 Google LLC. All rights reserved.
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file or at
# https://developers.google.com/open-source/licenses/bsd
"""End-to-end tests for packager-api (phase 1: event control)."""

import json
import os
import socket
import subprocess
import tempfile
import time
import unittest
import urllib.error
import urllib.request

API_BIN = os.environ['PACKAGER_API_BIN']
PACKAGER_BIN = os.environ['PACKAGER_BIN']
SRC_DIR = os.environ['PACKAGER_SRC_DIR']


def free_port(kind):
  probe = socket.socket(socket.AF_INET, kind)
  probe.bind(('127.0.0.1', 0))
  port = probe.getsockname()[1]
  probe.close()
  return port


def http(method, url, body=None, token=None):
  request = urllib.request.Request(url, method=method)
  if token:
    request.add_header('Authorization', 'Bearer ' + token)
  data = None
  if body is not None:
    request.add_header('Content-Type', 'application/json')
    data = json.dumps(body).encode('utf8')
  try:
    with urllib.request.urlopen(request, data=data, timeout=10) as response:
      return response.status, response.read().decode('utf8')
  except urllib.error.HTTPError as error:
    return error.code, error.read().decode('utf8')


class WebApiTest(unittest.TestCase):

  def setUp(self):
    self.tmp_dir = tempfile.mkdtemp()
    self.api_port = free_port(socket.SOCK_STREAM)
    self.base = 'http://127.0.0.1:%d' % self.api_port
    self.api = subprocess.Popen([
        API_BIN,
        '--api_port', str(self.api_port),
        '--api_bind_address', '127.0.0.1',
        '--packager_bin', PACKAGER_BIN,
        '--event_log_dir', self.tmp_dir,
        '--event_metrics_port_range', '20500-20599',
    ], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    # Wait for the server to accept connections.
    for _ in range(50):
      try:
        status, _ = http('GET', self.base + '/health')
        if status == 200:
          return
      except (ConnectionError, urllib.error.URLError):
        time.sleep(0.2)
    self.fail('packager-api did not come up; stderr:\n%s'
              % self.api.stderr.peek().decode('utf8', 'replace'))

  def tearDown(self):
    self.api.terminate()
    self.api.communicate(timeout=30)

  def wait_for_state(self, event_id, state, deadline=15):
    last = None
    for _ in range(int(deadline / 0.5)):
      status, body = http('GET', self.base + '/api/v1/events/' + event_id)
      self.assertEqual(200, status, body)
      last = json.loads(body)
      if last['state'] == state:
        return last
      time.sleep(0.5)
    self.fail('event %s never reached %s; last: %r' % (event_id, state, last))

  def testHealthAndSwagger(self):
    status, body = http('GET', self.base + '/health')
    self.assertEqual(200, status)
    self.assertIn('packager_version', json.loads(body))
    status, body = http('GET', self.base + '/swagger/doc')
    self.assertEqual(200, status)
    doc = json.loads(body)  # must be valid JSON
    self.assertTrue('openapi' in doc or 'swagger' in doc)
    paths = doc['paths']
    self.assertIn('/api/v1/events', paths)

  def testValidationAndUnknownEvent(self):
    status, body = http('POST', self.base + '/api/v1/events',
                        {'streams': []})
    self.assertEqual(400, status)
    self.assertEqual('invalid_request', json.loads(body)['error']['code'])
    status, body = http('GET', self.base + '/api/v1/events/nope')
    self.assertEqual(404, status)

  def testFullEventLifecycle(self):
    udp_port = free_port(socket.SOCK_DGRAM)
    create = {
        'event_id': 'e2e',
        'streams': [{
            'input': 'udp://127.0.0.1:%d?timeout=100000' % udp_port,
            'stream': 'video',
            'init_segment': self.tmp_dir + '/video_init.mp4',
            'segment_template': self.tmp_dir + '/video_$Number$.m4s',
        }],
        'mpd_output': self.tmp_dir + '/output.mpd',
        'segment_duration': 1,
        'stop_timeout_seconds': 5,
    }
    status, body = http('POST', self.base + '/api/v1/events', create)
    self.assertEqual(201, status, body)
    self.assertEqual('e2e', json.loads(body)['event_id'])

    # Duplicate id is rejected.
    status, body = http('POST', self.base + '/api/v1/events', create)
    self.assertEqual(409, status, body)

    self.wait_for_state('e2e', 'RUNNING')

    # Feed it a short burst of TS.
    replay = os.path.join(SRC_DIR, 'packager', 'tools', 'redundant_ts',
                          'replay_ts.py')
    ts_file = os.path.join(SRC_DIR, 'packager', 'media', 'test', 'data',
                           'bear-640x360.ts')
    subprocess.check_call(['python3', replay, ts_file,
                           '--ports', str(udp_port), '--pps', '300'])
    time.sleep(2)

    # Metrics proxy shows the event's own counters.
    status, body = http('GET', self.base + '/api/v1/events/e2e/metrics')
    self.assertEqual(200, status, body)
    self.assertIn('shaka_udp_datagrams_received_total', body)

    # Logs endpoint returns something.
    status, body = http('GET', self.base + '/api/v1/events/e2e/logs?tail=50')
    self.assertEqual(200, status)

    # List shows it.
    status, body = http('GET', self.base + '/api/v1/events')
    self.assertEqual(200, status)
    self.assertEqual(1, len(json.loads(body)['events']))

    # Drain stop -> STOPPED with an exit code.
    status, body = http('DELETE', self.base + '/api/v1/events/e2e')
    self.assertEqual(202, status, body)
    final = self.wait_for_state('e2e', 'STOPPED')
    self.assertIn('exit_code', final)

  def testBearerTokenAuth(self):
    # Restart the API with a token.
    self.api.terminate()
    self.api.communicate(timeout=30)
    self.api_port = free_port(socket.SOCK_STREAM)
    self.base = 'http://127.0.0.1:%d' % self.api_port
    self.api = subprocess.Popen([
        API_BIN, '--api_port', str(self.api_port),
        '--api_bind_address', '127.0.0.1',
        '--packager_bin', PACKAGER_BIN,
        '--event_log_dir', self.tmp_dir,
        '--api_token', 'sesame',
    ], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    for _ in range(50):
      try:
        if http('GET', self.base + '/health')[0] == 200:
          break
      except (ConnectionError, urllib.error.URLError):
        time.sleep(0.2)
    self.assertEqual(401, http('GET', self.base + '/api/v1/events')[0])
    self.assertEqual(200, http('GET', self.base + '/api/v1/events',
                               token='sesame')[0])
    self.assertEqual(200, http('GET', self.base + '/health')[0])


if __name__ == '__main__':
  unittest.main(verbosity=2)
```

- [ ] **Step 2: Register with ctest**

Append to `packager/webapi/CMakeLists.txt`:

```cmake
if(NOT SKIP_INTEGRATION_TESTS)
  add_test(NAME webapi_test_py
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/test/webapi_test.py"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(webapi_test_py PROPERTIES ENVIRONMENT
      "PACKAGER_SRC_DIR=${CMAKE_SOURCE_DIR};PACKAGER_BIN=$<TARGET_FILE:packager>;PACKAGER_API_BIN=$<TARGET_FILE:packager-api>")
endif()
```

- [ ] **Step 3: Run it**

```bash
cmake -B build -S . && cmake --build build -t packager-api packager
ctest --test-dir build -R webapi_test_py --output-on-failure
```

Expected: 4/4 tests pass. Run twice for stability. If `testFullEventLifecycle` flakes on RUNNING, the readiness probe may need the event's first metrics scrape — the default 5s fallback covers a debug-build packager's slow boot; raise only with evidence.

- [ ] **Step 4: Commit**

```bash
git add packager/webapi
git commit -m "test: end-to-end webapi event lifecycle, auth, and swagger coverage"
```

---

### Task 7: Documentation

**Files:**
- Create: `packager/webapi/README.md`
- Create: `docs/source/documentation/webapi.rst` (or `docs/source/options/webapi_options.rst` if `documentation/` has no toctree slot — match the tree; check `docs/source/index.rst` for where pages register and follow it)
- Modify: the toctree file that registers the new page

**Interfaces:**
- Consumes: final flag names/endpoints from Tasks 4–5.
- Produces: user-facing reference.

- [ ] **Step 1: Write README.md** — endpoints table (the 8 routes), curl quickstart (create a UDP live event, check status, scrape proxy metrics, drain stop), flags table (the 7 flags with defaults), build (`cmake --build build -t packager-api`), and the security paragraph: unauthenticated by default, `--api_token` for bearer auth, bind to a management interface or front with a proxy for anything beyond a closed network; the OpenAPI doc at `/swagger/doc` is the authoritative endpoint reference.

- [ ] **Step 2: Write the rst** — same content adapted to the docs tree conventions (look at `docs/source/tutorials/live.rst` heading styles); register it in the appropriate toctree; keep it short and point at `/swagger/ui` for the living reference.

- [ ] **Step 3: Commit**

```bash
git add packager/webapi/README.md docs/source
git commit -m "docs: packager-api web service reference"
```

---

## Final verification (after all tasks)

- [ ] `ctest --test-dir build -R "webapi"` — unit + e2e green, twice.
- [ ] Regression sweep: `ctest --test-dir build -R "metrics_unittest|file_unittest|mp2t_unittest|media_event_unittest|mpd_unittest"` — untouched suites stay green.
- [ ] Manual: start `packager-api`, open `http://localhost:8088/swagger/ui` in a browser, execute `/health` from the UI.
- [ ] Confirm exactly two files outside `packager/webapi/` + `packager/third_party/oatpp/` changed: `packager/third_party/CMakeLists.txt`, `packager/CMakeLists.txt`.

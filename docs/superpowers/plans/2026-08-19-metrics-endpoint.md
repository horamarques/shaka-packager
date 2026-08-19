# Prometheus Metrics Endpoint Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expose real-time live-channel metrics (`/metrics`, Prometheus text format) from a running packager on an opt-in `--metrics_port`, covering UDP input, redundant merger, TS parse health, output segments, and manifest/live state.

**Architecture:** Vendored prometheus-cpp (core + pull Exposer, its bundled civetweb) behind a `MetricsService` singleton in a new leaf library `packager/metrics/`. Hot paths increment pre-fetched atomic `prometheus::Counter*` handles; lock-owned state (redundant merger, MPD live window) is exported via `prometheus::Collectable` snapshot adapters held as `weak_ptr` by the exposer. The exposer starts in `Packager::Initialize` when `metrics_port > 0` and stops in `~Packager` before pipeline teardown.

**Tech Stack:** C++17, CMake submodule vendoring, prometheus-cpp v1.3.x, abseil, gtest, python integration tests (`packager_test.py`).

**Spec:** `docs/superpowers/specs/2026-08-19-metrics-endpoint-design.md`

## Global Constraints

- Metric name prefix `shaka_`; counters end `_total`; no histograms in v1.
- Default behavior unchanged: `metrics_port = 0` disables the listener; counters are always maintained (atomic increments are unconditional).
- One `Packager` instance per process; a second concurrent `StartExposer` returns an error `Status`.
- `shaka_udp_kernel_drops_total` is Linux-only (`SO_RXQ_OVFL`), compile-guarded, **absent** (not zero) elsewhere.
- The `metrics` CMake target must stay a **leaf**: it may link only prometheus-cpp, `status`, `version`, and absl — never `file`, `media_*`, or `mpd_builder` (those link `metrics`; a back-edge creates a cycle).
- `RedundantInputMerger` stays non-thread-safe; all merger access serializes under `RedundantUdpFile::merger_mutex_`.
- Follow repo style: BSD license header on new files, `///` Doxygen comments on public API, `DISALLOW_COPY_AND_ASSIGN`, includes as `<packager/...>`.
- Commits end with `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
- Spec deviation (approved rationale documented here): the `{input}` label value is the input **URL** (`File::file_name()`), not a stream index — the File layer has no index to plumb. `{stream}` on output metrics is the muxer-listener stream index. `{representation}` is the MPD notification id.
- The test build runs on macOS (developer machine): every unit test must pass on Darwin; Linux-only code paths compile out cleanly.
- Build/test commands: `cmake --build build -t <target>` then run the test binary from `build/packager/...`, or `ctest --test-dir build -R <name>`. If `build/` is not configured: `cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug` first.

---

### Task 1: Vendor prometheus-cpp

**Files:**
- Create: `packager/third_party/prometheus-cpp/CMakeLists.txt`
- Create (submodule): `packager/third_party/prometheus-cpp/source`
- Modify: `.gitmodules` (via `git submodule add`)
- Modify: `packager/third_party/CMakeLists.txt` (one line, after `mongoose`)

**Interfaces:**
- Consumes: nothing.
- Produces: CMake targets `prometheus-cpp::core` and `prometheus-cpp::pull` linkable by later tasks.

- [ ] **Step 1: Add the submodule pinned to the latest v1.x release**

```bash
cd /Users/pedromarques/Documents/Development/Velocix/shaka-packager
git submodule add https://github.com/jupp0r/prometheus-cpp packager/third_party/prometheus-cpp/source
cd packager/third_party/prometheus-cpp/source
git fetch --tags
git checkout v1.3.0
git submodule update --init --recursive   # pulls its bundled civetweb
```

If `v1.3.0` does not exist, run `git tag --list 'v1.*'` and check out the newest v1.x tag; record the chosen tag in the commit message.

- [ ] **Step 2: Write the wrapper CMakeLists**

Create `packager/third_party/prometheus-cpp/CMakeLists.txt`:

```cmake
# Copyright 2026 Google LLC. All rights reserved.
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file or at
# https://developers.google.com/open-source/licenses/bsd

# CMake build file for the prometheus-cpp library, which provides the
# metrics registry and the /metrics HTTP exposer (bundled civetweb).

set(ENABLE_PUSH OFF)
set(ENABLE_COMPRESSION OFF)
set(ENABLE_TESTING OFF)

add_subdirectory(source EXCLUDE_FROM_ALL)
```

- [ ] **Step 3: Register it in the third-party build**

In `packager/third_party/CMakeLists.txt`, directly after the `add_subdirectory(mongoose EXCLUDE_FROM_ALL)` line (line 52), add:

```cmake
add_subdirectory(prometheus-cpp EXCLUDE_FROM_ALL)
```

- [ ] **Step 4: Verify it builds**

```bash
cmake -B build -S .   # reconfigure to pick up the new subdirectory
cmake --build build -t pull
```

Expected: `core` and `pull` static libraries compile with no errors. (`pull` depends on `core`, so one target proves both.)

- [ ] **Step 5: Commit**

```bash
git add .gitmodules packager/third_party/prometheus-cpp packager/third_party/CMakeLists.txt
git commit -m "build: vendor prometheus-cpp v1.3.0 (core + pull exposer)"
```

---

### Task 2: MetricsService singleton (`packager/metrics/`)

**Files:**
- Create: `packager/metrics/metrics_service.h`
- Create: `packager/metrics/metrics_service.cc`
- Create: `packager/metrics/metrics_service_unittest.cc`
- Create: `packager/metrics/CMakeLists.txt`
- Modify: `packager/CMakeLists.txt` (`add_subdirectory(metrics)` in the subdirectory list around line 116)

**Interfaces:**
- Consumes: `prometheus-cpp::core`, `prometheus-cpp::pull` (Task 1), `shaka::Status` (`<packager/status.h>`), `GetPackagerVersion()` (`<packager/version/version.h>`).
- Produces (used by every later task):
  - `shaka::MetricsService& MetricsService::Instance()`
  - `prometheus::Registry& MetricsService::registry()`
  - `Status MetricsService::StartExposer(const std::string& bind_address, int port)`
  - `void MetricsService::StopExposer()`
  - `int MetricsService::listening_port() const` (0 when stopped; real port when started, including resolved ephemeral port for port 0)
  - `void MetricsService::RegisterCollectable(std::weak_ptr<prometheus::Collectable>)`
  - `std::vector<prometheus::MetricFamily> MetricsService::CollectAllForTesting()`

- [ ] **Step 1: Write the failing test**

Create `packager/metrics/metrics_service_unittest.cc`:

```cpp
// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/metrics/metrics_service.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include <prometheus/counter.h>

namespace shaka {
namespace {

// Fetches http://127.0.0.1:port/metrics with a raw socket (no HTTP client
// dependency) and returns the full response (headers + body).
std::string HttpGetMetrics(int port) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_GE(sock, 0);
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  EXPECT_EQ(0, connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
  const char request[] =
      "GET /metrics HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
  EXPECT_EQ(static_cast<ssize_t>(sizeof(request) - 1),
            send(sock, request, sizeof(request) - 1, 0));
  std::string response;
  char buffer[4096];
  ssize_t n;
  while ((n = recv(sock, buffer, sizeof(buffer), 0)) > 0)
    response.append(buffer, n);
  close(sock);
  return response;
}

TEST(MetricsServiceTest, RegistryCountersAppearInCollectAll) {
  auto& counter = prometheus::BuildCounter()
                      .Name("shaka_test_registry_total")
                      .Help("test")
                      .Register(MetricsService::Instance().registry())
                      .Add({{"case", "registry"}});
  counter.Increment(3);
  bool found = false;
  for (const auto& family : MetricsService::Instance().CollectAllForTesting()) {
    if (family.name == "shaka_test_registry_total") {
      found = true;
      ASSERT_EQ(1u, family.metric.size());
      EXPECT_DOUBLE_EQ(3.0, family.metric[0].counter.value);
    }
  }
  EXPECT_TRUE(found);
}

class TestCollectable : public prometheus::Collectable {
 public:
  std::vector<prometheus::MetricFamily> Collect() const override {
    prometheus::MetricFamily family;
    family.name = "shaka_test_collectable_gauge";
    family.help = "test";
    family.type = prometheus::MetricType::Gauge;
    prometheus::ClientMetric metric;
    metric.gauge.value = 42.0;
    family.metric.push_back(metric);
    return {family};
  }
};

TEST(MetricsServiceTest, WeakCollectableIsSkippedAfterDestruction) {
  auto collectable = std::make_shared<TestCollectable>();
  MetricsService::Instance().RegisterCollectable(collectable);

  auto has_gauge = [] {
    for (const auto& f : MetricsService::Instance().CollectAllForTesting()) {
      if (f.name == "shaka_test_collectable_gauge")
        return true;
    }
    return false;
  };
  EXPECT_TRUE(has_gauge());
  collectable.reset();
  EXPECT_FALSE(has_gauge());
}

TEST(MetricsServiceTest, ExposerServesMetricsAndDoubleStartFails) {
  // Port 0: the OS picks a free port; listening_port() reports it.
  ASSERT_TRUE(MetricsService::Instance().StartExposer("127.0.0.1", 0).ok());
  const int port = MetricsService::Instance().listening_port();
  ASSERT_GT(port, 0);

  // Second start must fail while running.
  EXPECT_FALSE(MetricsService::Instance().StartExposer("127.0.0.1", 0).ok());

  const std::string response = HttpGetMetrics(port);
  EXPECT_NE(std::string::npos, response.find("200 OK"));
  EXPECT_NE(std::string::npos, response.find("shaka_build_info"));

  MetricsService::Instance().StopExposer();
  EXPECT_EQ(0, MetricsService::Instance().listening_port());
  // Restart after stop is allowed.
  ASSERT_TRUE(MetricsService::Instance().StartExposer("127.0.0.1", 0).ok());
  MetricsService::Instance().StopExposer();
}

}  // namespace
}  // namespace shaka
```

- [ ] **Step 2: Write the CMake for the new directory and register it**

Create `packager/metrics/CMakeLists.txt`:

```cmake
# Copyright 2026 Google LLC. All rights reserved.
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file or at
# https://developers.google.com/open-source/licenses/bsd

add_library(metrics STATIC
    metrics_service.cc)
target_link_libraries(metrics
    absl::strings
    prometheus-cpp::core
    prometheus-cpp::pull
    status
    version)

add_executable(metrics_unittest
    metrics_service_unittest.cc)
target_link_libraries(metrics_unittest
    gtest
    gtest_main
    metrics)
add_gtest(metrics_unittest)
```

In `packager/CMakeLists.txt`, in the subdirectory list (lines 115–124), after `add_subdirectory(kv_pairs)` add:

```cmake
add_subdirectory(metrics)
```

- [ ] **Step 3: Run the test to verify it fails**

```bash
cmake --build build -t metrics_unittest
```

Expected: FAIL to compile — `packager/metrics/metrics_service.h` does not exist.

- [ ] **Step 4: Write the implementation**

Create `packager/metrics/metrics_service.h`:

```cpp
// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_METRICS_METRICS_SERVICE_H_
#define PACKAGER_METRICS_METRICS_SERVICE_H_

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <prometheus/collectable.h>
#include <prometheus/registry.h>

#include <packager/status.h>

namespace prometheus {
class Exposer;
}  // namespace prometheus

namespace shaka {

/// Process-wide metrics registry with an optional Prometheus HTTP exposer
/// (/metrics). Hot-path writers fetch counter/gauge handles from registry()
/// once at setup; snapshot-style sources (state owned under another
/// component's lock) register a prometheus::Collectable instead.
///
/// The exposer runs at most once per process: one Packager instance per
/// process is the supported model, and a second concurrent StartExposer
/// returns an error.
class MetricsService {
 public:
  static MetricsService& Instance();

  /// Starts the /metrics HTTP listener on @a bind_address : @a port.
  /// Port 0 binds an ephemeral port (tests); see listening_port().
  /// @return an error Status if already running or the bind fails.
  Status StartExposer(const std::string& bind_address, int port);
  void StopExposer();
  /// @return the actual listening port, or 0 when the exposer is stopped.
  int listening_port() const;

  prometheus::Registry& registry() { return *registry_; }

  /// Registers a scrape-time snapshot source. Held weakly: a destroyed
  /// source is skipped, so component teardown needs no unregister call.
  void RegisterCollectable(std::weak_ptr<prometheus::Collectable> collectable);

  /// Collects the registry plus all live registered collectables.
  std::vector<prometheus::MetricFamily> CollectAllForTesting();

 private:
  class MultiCollectable;

  MetricsService();
  MetricsService(const MetricsService&) = delete;
  MetricsService& operator=(const MetricsService&) = delete;

  std::shared_ptr<prometheus::Registry> registry_;
  std::shared_ptr<MultiCollectable> extra_collectables_;

  mutable std::mutex mutex_;
  std::unique_ptr<prometheus::Exposer> exposer_;
  int listening_port_ = 0;
};

}  // namespace shaka

#endif  // PACKAGER_METRICS_METRICS_SERVICE_H_
```

Create `packager/metrics/metrics_service.cc`:

```cpp
// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/metrics/metrics_service.h>

#include <utility>

#include <absl/strings/str_cat.h>
#include <prometheus/exposer.h>
#include <prometheus/gauge.h>

#include <packager/version/version.h>

namespace shaka {

// Aggregates weakly-held collectables so sources can be registered before
// the exposer starts and can die at any time without unregistering.
class MetricsService::MultiCollectable : public prometheus::Collectable {
 public:
  void Add(std::weak_ptr<prometheus::Collectable> collectable) {
    std::lock_guard<std::mutex> lock(mutex_);
    collectables_.push_back(std::move(collectable));
  }

  std::vector<prometheus::MetricFamily> Collect() const override {
    std::vector<std::shared_ptr<prometheus::Collectable>> live;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const auto& weak : collectables_) {
        if (auto strong = weak.lock())
          live.push_back(std::move(strong));
      }
    }
    std::vector<prometheus::MetricFamily> families;
    for (const auto& collectable : live) {
      auto sub = collectable->Collect();
      families.insert(families.end(), std::make_move_iterator(sub.begin()),
                      std::make_move_iterator(sub.end()));
    }
    return families;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::weak_ptr<prometheus::Collectable>> collectables_;
};

MetricsService::MetricsService()
    : registry_(std::make_shared<prometheus::Registry>()),
      extra_collectables_(std::make_shared<MultiCollectable>()) {}

// static
MetricsService& MetricsService::Instance() {
  // Leaked intentionally: counters may be touched during static teardown.
  static MetricsService* const service = new MetricsService();
  return *service;
}

Status MetricsService::StartExposer(const std::string& bind_address,
                                    int port) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (exposer_) {
    return Status(error::INVALID_ARGUMENT,
                  "Metrics exposer already running; one Packager instance "
                  "per process is supported.");
  }
  const std::string endpoint = absl::StrCat(bind_address, ":", port);
  try {
    exposer_.reset(new prometheus::Exposer(endpoint));
  } catch (const std::exception& e) {
    return Status(error::INVALID_ARGUMENT,
                  absl::StrCat("Failed to start metrics exposer on ", endpoint,
                               ": ", e.what()));
  }
  exposer_->RegisterCollectable(registry_);
  exposer_->RegisterCollectable(extra_collectables_);
  const auto ports = exposer_->GetListeningPorts();
  listening_port_ = ports.empty() ? 0 : ports.front();

  prometheus::BuildGauge()
      .Name("shaka_build_info")
      .Help("Build information; the value is always 1.")
      .Register(*registry_)
      .Add({{"version", GetPackagerVersion()}})
      .Set(1);
  return Status::OK;
}

void MetricsService::StopExposer() {
  std::lock_guard<std::mutex> lock(mutex_);
  exposer_.reset();
  listening_port_ = 0;
}

int MetricsService::listening_port() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return listening_port_;
}

void MetricsService::RegisterCollectable(
    std::weak_ptr<prometheus::Collectable> collectable) {
  extra_collectables_->Add(std::move(collectable));
}

std::vector<prometheus::MetricFamily>
MetricsService::CollectAllForTesting() {
  std::vector<prometheus::MetricFamily> families = registry_->Collect();
  auto extra = extra_collectables_->Collect();
  families.insert(families.end(), std::make_move_iterator(extra.begin()),
                  std::make_move_iterator(extra.end()));
  return families;
}

}  // namespace shaka
```

Note: `shaka_build_info` uses `Family::Add`, which returns the existing gauge on a repeated `{version=...}` label set, so stop/start does not duplicate it.

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --build build -t metrics_unittest && ./build/packager/metrics/metrics_unittest
```

Expected: all 3 tests PASS. If the exposer test flakes on `GetListeningPorts`, check that the prometheus-cpp tag from Task 1 provides it (it exists throughout v1.x).

- [ ] **Step 6: Commit**

```bash
git add packager/metrics packager/CMakeLists.txt
git commit -m "feat: MetricsService singleton with prometheus registry and exposer"
```

---

### Task 3: `--metrics_port` flag, PackagingParams, Packager lifecycle

**Files:**
- Modify: `include/packager/packager.h` (PackagingParams, ~line 88)
- Modify: `packager/packager.cc` (Initialize ~line 978, ~Packager at line 965)
- Modify: `packager/app/packager_main.cc` (flags ~line 81, GetPackagingParams ~line 598)
- Modify: `packager/CMakeLists.txt` (`libpackager_deps`, line 139–160)

**Interfaces:**
- Consumes: `MetricsService::StartExposer/StopExposer` (Task 2).
- Produces: `PackagingParams::metrics_port` (`int32_t`, default 0) and `PackagingParams::metrics_bind_address` (`std::string`, default `"0.0.0.0"`); CLI flags `--metrics_port`, `--metrics_bind_address`. Later tasks assume the exposer is running whenever `--metrics_port` is passed.

- [ ] **Step 1: Add the params**

In `include/packager/packager.h`, after the `buffer_callback_params` field (line 84), add:

```cpp
  /// Port for the Prometheus metrics HTTP endpoint (/metrics).
  /// 0 (default) disables the endpoint.
  int32_t metrics_port = 0;
  /// Bind address for the metrics endpoint.
  std::string metrics_bind_address = "0.0.0.0";
```

- [ ] **Step 2: Wire the Packager lifecycle**

In `packager/packager.cc`:

Add the include (with the other `<packager/...>` includes):

```cpp
#include <packager/metrics/metrics_service.h>
```

In `Initialize`, directly after the version-injection block (lines 975–978, so the injected test version reaches `shaka_build_info`):

```cpp
  if (packaging_params.metrics_port > 0) {
    RETURN_IF_ERROR(MetricsService::Instance().StartExposer(
        packaging_params.metrics_bind_address, packaging_params.metrics_port));
  }
```

Change the destructor (line 965) so the exposer stops before `internal_` (and every collectable source it references) is destroyed:

```cpp
Packager::~Packager() {
  // Stop serving scrapes before the pipeline objects that back the
  // registered collectables are torn down.
  MetricsService::Instance().StopExposer();
}
```

In `packager/CMakeLists.txt`, add `metrics` to `libpackager_deps` (alphabetically, after `media_trick_play` on line 155):

```cmake
  metrics
```

- [ ] **Step 3: Add the CLI flags**

In `packager/app/packager_main.cc`, after the `single_threaded` flag (line 81), add:

```cpp
ABSL_FLAG(int32_t,
          metrics_port,
          0,
          "Port for the Prometheus metrics HTTP endpoint (/metrics). "
          "0 (default) disables the endpoint.");
ABSL_FLAG(std::string,
          metrics_bind_address,
          "0.0.0.0",
          "Bind address for the Prometheus metrics HTTP endpoint.");
```

In `GetPackagingParams()`, after the `output_media_info` mapping (line 598), add:

```cpp
  packaging_params.metrics_port = absl::GetFlag(FLAGS_metrics_port);
  if (packaging_params.metrics_port < 0 ||
      packaging_params.metrics_port > 65535) {
    LOG(ERROR) << "--metrics_port must be in [0, 65535].";
    return std::nullopt;
  }
  packaging_params.metrics_bind_address =
      absl::GetFlag(FLAGS_metrics_bind_address);
```

- [ ] **Step 4: Verify build and flag plumbing**

```bash
cmake --build build -t packager
./build/packager/packager --help 2>&1 | grep -A2 metrics_port
./build/packager/packager foo --metrics_port 70000 2>&1 | grep "must be in"
```

Expected: the flag shows in help; the out-of-range value logs the error and the process exits non-zero. (End-to-end scrape verification lands in Task 9.)

- [ ] **Step 5: Commit**

```bash
git add include/packager/packager.h packager/packager.cc packager/app/packager_main.cc packager/CMakeLists.txt
git commit -m "feat: --metrics_port flag and Packager-scoped metrics exposer lifecycle"
```

---

### Task 4: UdpFile input counters + Linux kernel drop counts

**Files:**
- Modify: `packager/file/udp_file.h` (private section, lines 48–56)
- Modify: `packager/file/udp_file.cc` (`Read` lines 79–94, `Open` tail lines 282–307)
- Create: `packager/file/udp_file_metrics_unittest.cc`
- Modify: `packager/file/CMakeLists.txt` (link `metrics`; add the test file)

**Interfaces:**
- Consumes: `MetricsService::Instance().registry()` (Task 2).
- Produces metrics: `shaka_udp_bytes_received_total{input}`, `shaka_udp_datagrams_received_total{input}`, `shaka_udp_recv_timeouts_total{input}`, `shaka_udp_recv_errors_total{input}`, `shaka_udp_last_receive_timestamp_seconds{input}`, and (Linux only) `shaka_udp_kernel_drops_total{input}`. `{input}` = the full `udp://...` URL (`file_name()`).

- [ ] **Step 1: Write the failing test**

Create `packager/file/udp_file_metrics_unittest.cc`:

```cpp
// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <packager/file.h>
#include <packager/metrics/metrics_service.h>

namespace shaka {
namespace {

// Returns the value of |family_name| for the metric whose "input" label is
// |input|, or -1 when absent.
double MetricValue(const std::string& family_name, const std::string& input) {
  for (const auto& family : MetricsService::Instance().CollectAllForTesting()) {
    if (family.name != family_name)
      continue;
    for (const auto& metric : family.metric) {
      for (const auto& label : metric.label) {
        if (label.name == "input" && label.value == input)
          return family.type == prometheus::MetricType::Counter
                     ? metric.counter.value
                     : metric.gauge.value;
      }
    }
  }
  return -1;
}

TEST(UdpFileMetricsTest, CountsBytesDatagramsAndTimeouts) {
  // Bind an ephemeral port to find a free one, then reuse it.
  int probe = socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(probe, 0);
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(0, bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
  socklen_t addr_len = sizeof(addr);
  ASSERT_EQ(0, getsockname(probe, reinterpret_cast<sockaddr*>(&addr),
                           &addr_len));
  const int port = ntohs(addr.sin_port);
  close(probe);

  const std::string url =
      "udp://127.0.0.1:" + std::to_string(port) + "?timeout=200000";
  File* reader = File::OpenWithNoBuffering(url.c_str(), "r");
  ASSERT_TRUE(reader);

  // The label value is the URL without the scheme prefix consumed by the
  // factory; capture whatever file_name() reports via the registry after
  // the first read.
  int sender = socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(sender, 0);
  addr.sin_port = htons(port);
  const std::vector<uint8_t> datagram(1316, 0x47);
  for (int i = 0; i < 3; ++i) {
    ASSERT_EQ(static_cast<ssize_t>(datagram.size()),
              sendto(sender, datagram.data(), datagram.size(), 0,
                     reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
  }
  close(sender);

  std::vector<uint8_t> buffer(65536);
  int received = 0;
  // 3 reads succeed; the 4th hits the 200ms timeout.
  for (int i = 0; i < 4; ++i) {
    if (reader->Read(buffer.data(), buffer.size()) > 0)
      ++received;
  }
  ASSERT_EQ(3, received);

  const std::string input = "127.0.0.1:" + std::to_string(port) +
                            "?timeout=200000";
  EXPECT_DOUBLE_EQ(3.0,
                   MetricValue("shaka_udp_datagrams_received_total", input));
  EXPECT_DOUBLE_EQ(3.0 * 1316,
                   MetricValue("shaka_udp_bytes_received_total", input));
  EXPECT_GE(MetricValue("shaka_udp_recv_timeouts_total", input), 1.0);
  EXPECT_GT(MetricValue("shaka_udp_last_receive_timestamp_seconds", input),
            0.0);
#if !defined(__linux__)
  EXPECT_DOUBLE_EQ(-1.0, MetricValue("shaka_udp_kernel_drops_total", input));
#endif

  reader->Close();
}

}  // namespace
}  // namespace shaka
```

Note on the `input` value: `File::Open`/`OpenWithNoBuffering` strip the `udp://` prefix before constructing `UdpFile`, so `file_name()` is `127.0.0.1:<port>?timeout=200000`. If the first run shows the label with the scheme included, adjust the test's `input` string to match `file_name()` — the label must simply equal `file_name()`.

- [ ] **Step 2: Register the test and link metrics; verify failure**

In `packager/file/CMakeLists.txt`: add `metrics` to `target_link_libraries(file ...)` (after `libcurl`), add `udp_file_metrics_unittest.cc` to the `file_unittest` sources, and add `metrics` to `file_unittest`'s libraries.

```bash
cmake --build build -t file_unittest && ./build/packager/file/file_unittest --gtest_filter='UdpFileMetricsTest.*'
```

Expected: compiles, test FAILS (all `MetricValue` calls return -1 — no metrics exist yet).

- [ ] **Step 3: Implement the counters**

In `packager/file/udp_file.h`, forward-declare and add members. Before `namespace shaka`:

```cpp
namespace prometheus {
class Counter;
class Gauge;
}  // namespace prometheus
```

In the private section (after `socket_`):

```cpp
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
```

In `packager/file/udp_file.cc`:

Add includes:

```cpp
#include <prometheus/counter.h>
#include <prometheus/gauge.h>

#include <packager/metrics/metrics_service.h>
```

In `Open()`, immediately before `socket_ = new_socket.release();` (line 306), add:

```cpp
#if defined(__linux__)
  // Ask the kernel to attach cumulative receive-queue drop counts to each
  // datagram (read in Read() via recvmsg cmsg). Non-fatal when unsupported.
  const int optval_one = 1;
  if (setsockopt(new_socket.get(), SOL_SOCKET, SO_RXQ_OVFL,
                 reinterpret_cast<const char*>(&optval_one),
                 sizeof(optval_one)) < 0) {
    LOG(WARNING) << "Failed to enable SO_RXQ_OVFL, kernel drop counts "
                    "unavailable, error = "
                 << GetSocketErrorCode();
  }
#endif

  auto& registry = MetricsService::Instance().registry();
  const prometheus::Labels labels{{"input", file_name()}};
  bytes_received_counter_ =
      &prometheus::BuildCounter()
           .Name("shaka_udp_bytes_received_total")
           .Help("UDP payload bytes received.")
           .Register(registry)
           .Add(labels);
  datagrams_received_counter_ =
      &prometheus::BuildCounter()
           .Name("shaka_udp_datagrams_received_total")
           .Help("UDP datagrams received.")
           .Register(registry)
           .Add(labels);
  recv_timeouts_counter_ =
      &prometheus::BuildCounter()
           .Name("shaka_udp_recv_timeouts_total")
           .Help("UDP receive timeouts (SO_RCVTIMEO expiries).")
           .Register(registry)
           .Add(labels);
  recv_errors_counter_ =
      &prometheus::BuildCounter()
           .Name("shaka_udp_recv_errors_total")
           .Help("UDP receive errors other than timeouts.")
           .Register(registry)
           .Add(labels);
  last_receive_timestamp_gauge_ =
      &prometheus::BuildGauge()
           .Name("shaka_udp_last_receive_timestamp_seconds")
           .Help("Unix time of the last received datagram.")
           .Register(registry)
           .Add(labels);
#if defined(__linux__)
  kernel_drops_counter_ =
      &prometheus::BuildCounter()
           .Name("shaka_udp_kernel_drops_total")
           .Help("Datagrams dropped by the kernel receive queue "
                 "(SO_RXQ_OVFL).")
           .Register(registry)
           .Add(labels);
#endif
```

(`prometheus::Labels` needs `#include <prometheus/labels.h>` on some versions; add it if the build asks.)

Replace `Read()` (lines 79–94) with:

```cpp
int64_t UdpFile::Read(void* buffer, uint64_t length) {
  DCHECK(buffer);
  DCHECK_GE(length, 65535u)
      << "Buffer may be too small to read entire datagram.";

  if (socket_ == INVALID_SOCKET)
    return -1;

  int64_t result;
#if defined(__linux__)
  struct iovec iov;
  iov.iov_base = buffer;
  iov.iov_len = static_cast<size_t>(length);
  alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(uint32_t))];
  struct msghdr msg = {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);
  do {
    result = recvmsg(socket_, &msg, 0);
  } while (result == -1 && GetSocketErrorCode() == EINTR_CODE);
  if (result >= 0 && kernel_drops_counter_) {
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
      if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_RXQ_OVFL) {
        uint32_t drops;
        memcpy(&drops, CMSG_DATA(cmsg), sizeof(drops));
        if (kernel_drop_count_valid_ && drops >= last_kernel_drop_count_) {
          kernel_drops_counter_->Increment(drops - last_kernel_drop_count_);
        }
        last_kernel_drop_count_ = drops;
        kernel_drop_count_valid_ = true;
      }
    }
  }
#else
  do {
    result = recvfrom(socket_, reinterpret_cast<char*>(buffer),
                      static_cast<int>(length), 0, NULL, 0);
  } while (result == -1 && GetSocketErrorCode() == EINTR_CODE);
#endif

  if (result >= 0) {
    if (datagrams_received_counter_)
      datagrams_received_counter_->Increment();
    if (bytes_received_counter_)
      bytes_received_counter_->Increment(static_cast<double>(result));
    if (last_receive_timestamp_gauge_)
      last_receive_timestamp_gauge_->SetToCurrentTime();
  } else {
    const int error_code = GetSocketErrorCode();
#if defined(OS_WIN)
    const bool is_timeout = error_code == WSAETIMEDOUT;
#else
    const bool is_timeout =
        error_code == EAGAIN || error_code == EWOULDBLOCK;
#endif
    if (is_timeout) {
      if (recv_timeouts_counter_)
        recv_timeouts_counter_->Increment();
    } else if (recv_errors_counter_) {
      recv_errors_counter_->Increment();
    }
  }
  return result;
}
```

Also add `#include <cstring>` if not present (for `memcpy`).

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build -t file_unittest && ./build/packager/file/file_unittest --gtest_filter='UdpFileMetricsTest.*'
```

Expected: PASS. Then run the whole file suite to check for regressions (the redundant tests exercise `UdpFile` heavily):

```bash
./build/packager/file/file_unittest
```

Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add packager/file/udp_file.h packager/file/udp_file.cc packager/file/udp_file_metrics_unittest.cc packager/file/CMakeLists.txt
git commit -m "feat: UDP input metrics (bytes, datagrams, timeouts, errors, kernel drops)"
```

---

### Task 5: Redundant-input snapshot + Collectable

**Files:**
- Modify: `packager/file/redundant_udp_file.h` (public `StatsSnapshot` + `GetStatsSnapshot`, collector member)
- Modify: `packager/file/redundant_udp_file.cc` (snapshot, collector class, registration in `Open`, detach in `Close`, `MaybeLogStats` on the snapshot)
- Modify: `packager/file/redundant_udp_file_unittest.cc` (new test)

**Interfaces:**
- Consumes: `MetricsService::RegisterCollectable` (Task 2), `RedundantInputMerger::GetLegStats/switches/active_leg/emitted_cc_errors/max_skew_ms/window_evictions` (existing).
- Produces: `RedundantUdpFile::StatsSnapshot { std::vector<RedundantInputMerger::LegStats> legs; size_t active_leg; uint64_t switches; uint64_t emitted_cc_errors; int64_t max_skew_ms; uint64_t window_evictions; }` and `StatsSnapshot GetStatsSnapshot();` (locks `merger_mutex_`). Metrics: per-leg `shaka_redundant_leg_{packets,dropped_dup,resyncs,cc_errors}_total{input,leg}`, `shaka_redundant_leg_healthy{input,leg}` (0/1), `shaka_redundant_leg_active{input,leg}` (0/1); global `shaka_redundant_switches_total{input}`, `shaka_redundant_emitted_cc_errors_total{input}`, `shaka_redundant_max_skew_ms{input}`, `shaka_redundant_window_evictions_total{input}`.

- [ ] **Step 1: Write the failing test**

Append to `packager/file/redundant_udp_file_unittest.cc` (it already has helpers to open `redundant://` files and push UDP data from test sockets — reuse its existing sender pattern; add `#include <packager/metrics/metrics_service.h>`):

```cpp
double RedundantMetricValue(const std::string& family_name,
                            const std::string& leg) {
  for (const auto& family :
       MetricsService::Instance().CollectAllForTesting()) {
    if (family.name != family_name)
      continue;
    for (const auto& metric : family.metric) {
      bool leg_matches = leg.empty();
      for (const auto& label : metric.label) {
        if (label.name == "leg" && label.value == leg)
          leg_matches = true;
      }
      if (leg_matches) {
        return family.type == prometheus::MetricType::Counter
                   ? metric.counter.value
                   : metric.gauge.value;
      }
    }
  }
  return -1;
}

TEST(RedundantUdpFileMetricsTest, SnapshotIsExportedViaCollectable) {
  // Open a 2-leg merge-mode redundant file on ephemeral loopback ports,
  // send the same 10 distinct TS packets to both legs (7 per datagram is
  // not required), read the merged output, then scrape.
  // Reuse the port-probing and sender helpers used by the existing
  // RedundantUdpFile end-to-end tests in this file.
  //
  // After reading the packets back:
  EXPECT_GE(RedundantMetricValue("shaka_redundant_leg_packets_total", "0"),
            10.0);
  EXPECT_GE(RedundantMetricValue("shaka_redundant_leg_packets_total", "1"),
            10.0);
  EXPECT_GE(RedundantMetricValue("shaka_redundant_leg_dropped_dup_total", "1"),
            1.0);
  EXPECT_DOUBLE_EQ(1.0,
                   RedundantMetricValue("shaka_redundant_leg_healthy", "0"));
  EXPECT_DOUBLE_EQ(
      0.0, RedundantMetricValue("shaka_redundant_switches_total", ""));

  // After Close(), the collectable must disappear from scrapes (weak_ptr
  // expiry + detach), leaving no redundant families behind.
}
```

Write the full test body against the file's existing helpers (open with `File::Open("redundant://udp://127.0.0.1:<p1>|udp://127.0.0.1:<p2>", "r")` — mirroring the file's existing end-to-end test — send `BuildTsPacket(seq)`-style packets to both legs, `Read()` until 10 packets arrive). After `reader->Close()`, assert `RedundantMetricValue("shaka_redundant_leg_packets_total", "0")` returns the *stale-free* result: the family must be absent (−1) because the only source was the (now-detached and destroyed) collectable.

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build -t file_unittest && ./build/packager/file/file_unittest --gtest_filter='RedundantUdpFileMetricsTest.*'
```

Expected: FAIL (families absent, values −1).

- [ ] **Step 3: Implement snapshot + collector**

In `packager/file/redundant_udp_file.h`:

Add to the public section:

```cpp
  /// Point-in-time copy of the merger counters, safe to take from any
  /// thread (locks the internal merger mutex).
  struct StatsSnapshot {
    std::vector<RedundantInputMerger::LegStats> legs;
    size_t active_leg = 0;
    uint64_t switches = 0;
    uint64_t emitted_cc_errors = 0;
    int64_t max_skew_ms = 0;
    uint64_t window_evictions = 0;
  };
  StatsSnapshot GetStatsSnapshot();
```

Forward-declare the collector above the class and add a member:

```cpp
class RedundantUdpStatsCollector;
```

Private members (after `stop_`):

```cpp
  // Scrape-time exporter of GetStatsSnapshot(); registered weakly with
  // MetricsService in Open() and detached in Close() before delete this.
  std::shared_ptr<RedundantUdpStatsCollector> stats_collector_;
```

Private method (next to `MaybeLogStats`):

```cpp
  // Snapshot with merger_mutex_ already held.
  StatsSnapshot SnapshotLocked() const;
```

In `packager/file/redundant_udp_file.cc`:

Add includes:

```cpp
#include <prometheus/collectable.h>
#include <prometheus/metric_family.h>

#include <packager/metrics/metrics_service.h>
```

Define the collector (after the anonymous namespace, before `RedundantUdpFile` methods):

```cpp
// Exports RedundantUdpFile::GetStatsSnapshot() as Prometheus families.
// Detach() severs the back-pointer before the file self-deletes in Close();
// the MetricsService holds this object weakly, so its own destruction needs
// no unregistration.
class RedundantUdpStatsCollector : public prometheus::Collectable {
 public:
  RedundantUdpStatsCollector(RedundantUdpFile* file, std::string input_label)
      : file_(file), input_(std::move(input_label)) {}

  void Detach() {
    std::lock_guard<std::mutex> lock(mutex_);
    file_ = nullptr;
  }

  std::vector<prometheus::MetricFamily> Collect() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_)
      return {};
    const RedundantUdpFile::StatsSnapshot snap = file_->GetStatsSnapshot();

    std::vector<prometheus::MetricFamily> families;
    auto add_family = [&](const char* name, const char* help,
                          prometheus::MetricType type) {
      prometheus::MetricFamily family;
      family.name = name;
      family.help = help;
      family.type = type;
      families.push_back(std::move(family));
      return &families.back();
    };
    auto add_metric = [&](prometheus::MetricFamily* family, double value,
                          int leg /* -1 = global */) {
      prometheus::ClientMetric metric;
      metric.label.push_back({"input", input_});
      if (leg >= 0)
        metric.label.push_back({"leg", std::to_string(leg)});
      if (family->type == prometheus::MetricType::Counter)
        metric.counter.value = value;
      else
        metric.gauge.value = value;
      family->metric.push_back(std::move(metric));
    };

    using prometheus::MetricType;
    auto* packets = add_family("shaka_redundant_leg_packets_total",
                               "Well-framed TS packets per leg.",
                               MetricType::Counter);
    auto* dropped = add_family("shaka_redundant_leg_dropped_dup_total",
                               "Packets dropped as duplicates per leg.",
                               MetricType::Counter);
    auto* resyncs = add_family("shaka_redundant_leg_resyncs_total",
                               "Sync-byte resyncs per leg.",
                               MetricType::Counter);
    auto* cc_errors = add_family("shaka_redundant_leg_cc_errors_total",
                                 "Continuity-counter errors per leg.",
                                 MetricType::Counter);
    auto* healthy = add_family("shaka_redundant_leg_healthy",
                               "1 when the leg is HEALTHY, else 0.",
                               MetricType::Gauge);
    auto* active = add_family("shaka_redundant_leg_active",
                              "1 for the active leg (failover mode).",
                              MetricType::Gauge);
    for (size_t i = 0; i < snap.legs.size(); ++i) {
      const auto& leg = snap.legs[i];
      const int leg_index = static_cast<int>(i);
      add_metric(packets, static_cast<double>(leg.packets), leg_index);
      add_metric(dropped, static_cast<double>(leg.dropped_dup), leg_index);
      add_metric(resyncs, static_cast<double>(leg.resyncs), leg_index);
      add_metric(cc_errors, static_cast<double>(leg.cc_errors), leg_index);
      add_metric(healthy,
                 leg.state == RedundantInputMerger::LegState::kHealthy ? 1 : 0,
                 leg_index);
      add_metric(active, snap.active_leg == i ? 1 : 0, leg_index);
    }
    add_metric(add_family("shaka_redundant_switches_total",
                          "Active-leg switches (failover mode).",
                          MetricType::Counter),
               static_cast<double>(snap.switches), -1);
    add_metric(add_family("shaka_redundant_emitted_cc_errors_total",
                          "Continuity-counter errors in the emitted stream.",
                          MetricType::Counter),
               static_cast<double>(snap.emitted_cc_errors), -1);
    add_metric(add_family("shaka_redundant_max_skew_ms",
                          "Largest observed inter-leg arrival skew, ms.",
                          MetricType::Gauge),
               static_cast<double>(snap.max_skew_ms), -1);
    add_metric(add_family("shaka_redundant_window_evictions_total",
                          "Dedup-window evictions by the packet-count bound.",
                          MetricType::Counter),
               static_cast<double>(snap.window_evictions), -1);
    return families;
  }

 private:
  mutable std::mutex mutex_;
  RedundantUdpFile* file_;
  const std::string input_;
};
```

Add the snapshot methods:

```cpp
RedundantUdpFile::StatsSnapshot RedundantUdpFile::SnapshotLocked() const {
  StatsSnapshot snap;
  for (size_t i = 0; i < config_.num_legs; ++i)
    snap.legs.push_back(merger_->GetLegStats(i));
  snap.active_leg = merger_->active_leg();
  snap.switches = merger_->switches();
  snap.emitted_cc_errors = merger_->emitted_cc_errors();
  snap.max_skew_ms = merger_->max_skew_ms();
  snap.window_evictions = merger_->window_evictions();
  return snap;
}

RedundantUdpFile::StatsSnapshot RedundantUdpFile::GetStatsSnapshot() {
  std::lock_guard<std::mutex> lock(merger_mutex_);
  return SnapshotLocked();
}
```

(`SnapshotLocked` is const but reads `merger_` getters — fine. Mark `MaybeLogStats`'s caller contract unchanged.)

Rewrite `MaybeLogStats` to derive the log line from `SnapshotLocked()` (same output format, single source of truth):

```cpp
void RedundantUdpFile::MaybeLogStats(int64_t now_ms) {
  // Called with merger_mutex_ held.
  if (now_ms - last_stats_log_ms_ < kStatsLogPeriodMs)
    return;
  const bool first = last_stats_log_ms_ == 0;
  last_stats_log_ms_ = now_ms;
  if (first)
    return;  // Skip the incomplete first period.
  const StatsSnapshot snap = SnapshotLocked();
  for (size_t i = 0; i < snap.legs.size(); ++i) {
    const RedundantInputMerger::LegStats& stats = snap.legs[i];
    LOG(INFO) << "redundant_input: leg=" << i << " pkts=" << stats.packets
              << " dropped_dup=" << stats.dropped_dup
              << " resyncs=" << stats.resyncs
              << " cc_errors=" << stats.cc_errors << " state="
              << (stats.state == RedundantInputMerger::LegState::kHealthy
                      ? "HEALTHY"
                      : "UNHEALTHY")
              << " active=" << (snap.active_leg == i ? "yes" : "no");
  }
  LOG(INFO) << "redundant_input: switches=" << snap.switches
            << " emitted_cc_errors=" << snap.emitted_cc_errors
            << " max_skew_ms=" << snap.max_skew_ms
            << " window_evictions=" << snap.window_evictions;
}
```

In `Open()`, after the merger is created (line 125) add:

```cpp
  stats_collector_ =
      std::make_shared<RedundantUdpStatsCollector>(this, file_name());
  MetricsService::Instance().RegisterCollectable(stats_collector_);
```

In `Close()`, immediately before `delete this;`:

```cpp
  if (stats_collector_) {
    // Sever the collector's back-pointer before self-deletion; a scrape
    // holding the shared_ptr sees an empty result instead of a dangle.
    stats_collector_->Detach();
    stats_collector_.reset();
  }
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build -t file_unittest && ./build/packager/file/file_unittest
```

Expected: the new metrics test AND every pre-existing redundant test PASS (the log-line format must be byte-identical — the e2e leg-kill test greps stderr).

- [ ] **Step 5: Commit**

```bash
git add packager/file/redundant_udp_file.h packager/file/redundant_udp_file.cc packager/file/redundant_udp_file_unittest.cc
git commit -m "feat: export redundant-input merger stats via metrics collectable"
```

---

### Task 6: TS parse-health counters

**Files:**
- Modify: `packager/media/formats/mp2t/ts_packet.h` (store TEI + getter)
- Modify: `packager/media/formats/mp2t/ts_packet.cc` (line ~93: keep the parsed bit)
- Modify: `packager/media/formats/mp2t/mp2t_media_parser.cc` (PidState class at line 47: counter/gauge members; increments at lines 127/146/434; TEI tally in `Parse`; PTS gauge in `OnEmitMediaSample` at line 504)
- Modify: `packager/media/formats/mp2t/mp2t_media_parser.h` (parser members)
- Create: `packager/media/formats/mp2t/mp2t_metrics_unittest.cc`
- Modify: `packager/media/formats/mp2t/CMakeLists.txt` (link `metrics` into `mp2t`; add test file to `mp2t_unittest`)

**Interfaces:**
- Consumes: `MetricsService::Instance().registry()` (Task 2).
- Produces metrics: `shaka_ts_cc_errors_total{pid}`, `shaka_ts_pes_errors_total{pid}`, `shaka_ts_tei_packets_total`, `shaka_ts_unsupported_streams_total`, `shaka_media_latest_pts_seconds{pid}` (90 kHz ticks / 90000.0). Also: `bool TsPacket::transport_error_indicator() const`.

- [ ] **Step 1: Write the failing test**

Create `packager/media/formats/mp2t/mp2t_metrics_unittest.cc`:

```cpp
// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <packager/media/base/media_sample.h>
#include <packager/media/base/stream_info.h>
#include <packager/media/base/text_sample.h>
#include <packager/media/formats/mp2t/mp2t_media_parser.h>
#include <packager/media/test/test_data_util.h>
#include <packager/metrics/metrics_service.h>

namespace shaka {
namespace media {
namespace mp2t {
namespace {

double MetricValue(const std::string& family_name) {
  double total = 0;
  bool found = false;
  for (const auto& family :
       MetricsService::Instance().CollectAllForTesting()) {
    if (family.name != family_name)
      continue;
    for (const auto& metric : family.metric) {
      found = true;
      total += family.type == prometheus::MetricType::Counter
                   ? metric.counter.value
                   : metric.gauge.value;
    }
  }
  return found ? total : -1;
}

class Mp2tMetricsTest : public testing::Test {
 protected:
  void ParseBytes(const std::vector<uint8_t>& buffer) {
    parser_.Init(
        [](const std::vector<std::shared_ptr<StreamInfo>>&) {},
        [](uint32_t, std::shared_ptr<MediaSample>) { return true; },
        [](uint32_t, std::shared_ptr<TextSample>) { return true; },
        nullptr);
    ASSERT_TRUE(parser_.Parse(buffer.data(), static_cast<int>(buffer.size())));
  }
  Mp2tMediaParser parser_;
};

TEST_F(Mp2tMetricsTest, DroppedPacketsIncrementCcErrors) {
  const std::vector<uint8_t> buffer = ReadTestDataFile("bear-640x360.ts");
  ASSERT_FALSE(buffer.empty());
  // Drop every 10th 188-byte packet to force CC gaps on enabled PIDs.
  std::vector<uint8_t> holey;
  for (size_t i = 0; i * 188 < buffer.size(); ++i) {
    if (i % 10 == 9)
      continue;
    holey.insert(holey.end(), buffer.begin() + i * 188,
                 buffer.begin() + std::min((i + 1) * 188, buffer.size()));
  }
  const double cc_before = MetricValue("shaka_ts_cc_errors_total");
  ParseBytes(holey);
  const double cc_after = MetricValue("shaka_ts_cc_errors_total");
  EXPECT_GT(cc_after, cc_before < 0 ? 0 : cc_before);
  // The biggest-PTS gauge must be exported and positive.
  EXPECT_GT(MetricValue("shaka_media_latest_pts_seconds"), 0.0);
}

TEST_F(Mp2tMetricsTest, TeiFlagIsCounted) {
  // One syntactically valid TS packet with TEI set on an unknown PID.
  std::vector<uint8_t> packet(188, 0xFF);
  packet[0] = 0x47;
  packet[1] = 0x80 | 0x1F;  // TEI=1, PUSI=0, priority=0, pid high = 0x1F..
  packet[2] = 0xFE;         // ..pid low: 0x1FFE (unknown, not NULL 0x1FFF)
  packet[3] = 0x10;         // no adaptation field, payload only, CC=0
  const double tei_before = MetricValue("shaka_ts_tei_packets_total");
  ParseBytes(packet);
  const double tei_after = MetricValue("shaka_ts_tei_packets_total");
  EXPECT_DOUBLE_EQ((tei_before < 0 ? 0 : tei_before) + 1, tei_after);
}

}  // namespace
}  // namespace mp2t
}  // namespace media
}  // namespace shaka
```

Note: the registry is process-global and shared across the whole `mp2t_unittest` binary, so both tests assert **deltas**, never absolute values.

- [ ] **Step 2: Register the test and verify failure**

In `packager/media/formats/mp2t/CMakeLists.txt`: add `metrics` to `target_link_libraries(mp2t ...)` and `mp2t_metrics_unittest.cc` to the `mp2t_unittest` sources (that target already links `mp2t`; add `metrics` there too if the link fails).

```bash
cmake --build build -t mp2t_unittest && ./build/packager/media/formats/mp2t/mp2t_unittest --gtest_filter='Mp2tMetricsTest.*'
```

Expected: FAIL (families absent / no delta).

- [ ] **Step 3: Implement**

`packager/media/formats/mp2t/ts_packet.h` — add next to the other bit accessors and members:

```cpp
  bool transport_error_indicator() const { return transport_error_indicator_; }
```
```cpp
  bool transport_error_indicator_ = false;
```

`packager/media/formats/mp2t/ts_packet.cc` — after line 93 (`ReadBits(1, &transport_error_indicator)`), later in the function where the other locals get stored (next to `payload_unit_start_indicator_ = ...` at line 100), add:

```cpp
  transport_error_indicator_ = (transport_error_indicator != 0);
```

`packager/media/formats/mp2t/mp2t_media_parser.cc`:

Includes:

```cpp
#include <absl/strings/str_cat.h>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>

#include <packager/metrics/metrics_service.h>
```

Family helpers in the anonymous namespace (top of file):

```cpp
prometheus::Family<prometheus::Counter>& TsCcErrorFamily() {
  static auto& family =
      prometheus::BuildCounter()
          .Name("shaka_ts_cc_errors_total")
          .Help("MPEG-TS continuity-counter errors, per PID.")
          .Register(MetricsService::Instance().registry());
  return family;
}

prometheus::Family<prometheus::Counter>& TsPesErrorFamily() {
  static auto& family =
      prometheus::BuildCounter()
          .Name("shaka_ts_pes_errors_total")
          .Help("PES/section parse failures, per PID.")
          .Register(MetricsService::Instance().registry());
  return family;
}

prometheus::Family<prometheus::Gauge>& LatestPtsFamily() {
  static auto& family =
      prometheus::BuildGauge()
          .Name("shaka_media_latest_pts_seconds")
          .Help("Latest media timestamp seen on the input, per PID, in "
                "seconds of the 90kHz TS clock.")
          .Register(MetricsService::Instance().registry());
  return family;
}
```

In `class PidState` (defined at line 47 of this file), add members and initialize in the constructor (line 101):

```cpp
  // Metrics handles, resolved once per PID at construction.
  prometheus::Counter* cc_errors_counter_;
  prometheus::Counter* pes_errors_counter_;
  prometheus::Gauge* latest_pts_gauge_;
```

Constructor initializer additions:

```cpp
      cc_errors_counter_(
          &TsCcErrorFamily().Add({{"pid", absl::StrCat(pid)}})),
      pes_errors_counter_(
          &TsPesErrorFamily().Add({{"pid", absl::StrCat(pid)}})),
      latest_pts_gauge_(
          &LatestPtsFamily().Add({{"pid", absl::StrCat(pid)}})),
```

Add a public setter on `PidState`:

```cpp
  void set_latest_pts_seconds(double seconds) {
    latest_pts_gauge_->Set(seconds);
  }
```

Increment sites:
- In `PushTsPacket`, inside the discontinuity branch (after the `LOG(WARNING)` at line 127): `cc_errors_counter_->Increment();`
- In `PushTsPacket`, inside the `if (!status)` branch (line 145): `pes_errors_counter_->Increment();`

Parser-level counters — in `packager/media/formats/mp2t/mp2t_media_parser.h`, forward-declare `namespace prometheus { class Counter; }` above `namespace shaka` and add private members:

```cpp
  // Metrics handles, resolved once in the constructor.
  prometheus::Counter* tei_packets_counter_ = nullptr;
  prometheus::Counter* unsupported_streams_counter_ = nullptr;
```

In the `Mp2tMediaParser` constructor (mp2t_media_parser.cc line 180):

```cpp
Mp2tMediaParser::Mp2tMediaParser()
    : sbr_in_mimetype_(false),
      is_initialized_(false),
      tei_packets_counter_(
          &prometheus::BuildCounter()
               .Name("shaka_ts_tei_packets_total")
               .Help("TS packets with the transport_error_indicator set.")
               .Register(MetricsService::Instance().registry())
               .Add({})),
      unsupported_streams_counter_(
          &prometheus::BuildCounter()
               .Name("shaka_ts_unsupported_streams_total")
               .Help("Elementary streams ignored as unsupported.")
               .Register(MetricsService::Instance().registry())
               .Add({})) {}
```

(Keep the member-declaration order in the header consistent with this initializer order to avoid `-Wreorder`.)

In `Mp2tMediaParser::Parse`, right after the successful `TsPacket::Parse` (the `if (!ts_packet)` guard, ~line 245):

```cpp
    if (ts_packet->transport_error_indicator())
      tei_packets_counter_->Increment();
```

In the "Ignoring unsupported stream" branch (line 434): `unsupported_streams_counter_->Increment();`

In `OnEmitMediaSample`, next to `update_biggest_pts(timestamp_for_heartbeat);` (line 504):

```cpp
  pid_state->second->set_latest_pts_seconds(timestamp_for_heartbeat / 90000.0);
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build -t mp2t_unittest && ./build/packager/media/formats/mp2t/mp2t_unittest
```

Expected: the two new tests PASS and the whole existing mp2t suite stays green.

- [ ] **Step 5: Commit**

```bash
git add packager/media/formats/mp2t packager/media/formats/mp2t/CMakeLists.txt
git commit -m "feat: TS parse-health metrics (CC/PES errors, TEI, latest PTS)"
```

---

### Task 7: MetricsMuxerListener (output segment family)

**Files:**
- Create: `packager/media/event/metrics_muxer_listener.h`
- Create: `packager/media/event/metrics_muxer_listener.cc`
- Create: `packager/media/event/metrics_muxer_listener_unittest.cc`
- Modify: `packager/media/event/muxer_listener_factory.cc` (`CreateListener`, lines 101–137)
- Modify: `packager/media/event/CMakeLists.txt`

**Interfaces:**
- Consumes: `MuxerListener` interface (existing, signatures in `muxer_listener.h`), `MetricsService` (Task 2).
- Produces: `class MetricsMuxerListener : public MuxerListener`, ctor `explicit MetricsMuxerListener(const std::string& stream_label)`. Metrics (labels `{stream}`): `shaka_segments_emitted_total`, `shaka_segment_bytes_total`, `shaka_last_segment_duration_seconds`, `shaka_last_segment_timestamp_seconds`, `shaka_cue_events_total{direction=in|out}`, `shaka_key_rotations_total`.

- [ ] **Step 1: Write the failing test**

Create `packager/media/event/metrics_muxer_listener_unittest.cc`:

```cpp
// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/event/metrics_muxer_listener.h>

#include <string>

#include <gtest/gtest.h>

#include <packager/media/base/video_stream_info.h>
#include <packager/media/event/muxer_listener_test_helper.h>
#include <packager/metrics/metrics_service.h>

namespace shaka {
namespace media {
namespace {

double MetricValue(const std::string& family_name,
                   const std::string& stream_label,
                   const std::string& direction = "") {
  for (const auto& family :
       MetricsService::Instance().CollectAllForTesting()) {
    if (family.name != family_name)
      continue;
    for (const auto& metric : family.metric) {
      bool stream_ok = false, direction_ok = direction.empty();
      for (const auto& label : metric.label) {
        if (label.name == "stream" && label.value == stream_label)
          stream_ok = true;
        if (label.name == "direction" && label.value == direction)
          direction_ok = true;
      }
      if (stream_ok && direction_ok) {
        return family.type == prometheus::MetricType::Counter
                   ? metric.counter.value
                   : metric.gauge.value;
      }
    }
  }
  return -1;
}

TEST(MetricsMuxerListenerTest, TalliesSegmentsCuesAndKeyRotations) {
  // Unique label per test run keeps assertions absolute despite the
  // process-global registry.
  const std::string label = "test_stream_7";
  MetricsMuxerListener listener(label);

  MuxerOptions muxer_options;
  std::shared_ptr<StreamInfo> stream_info =
      CreateVideoStreamInfo(GetDefaultVideoStreamInfoParams());
  listener.OnMediaStart(muxer_options, *stream_info, 90000,
                        MuxerListener::kContainerMpeg2ts);

  listener.OnNewSegment("seg_1.ts", /*start_time=*/900000,
                        /*duration=*/180000, /*segment_file_size=*/500000,
                        /*segment_number=*/1);
  listener.OnNewSegment("seg_2.ts", 1080000, 180000, 600000, 2);

  EXPECT_DOUBLE_EQ(2.0, MetricValue("shaka_segments_emitted_total", label));
  EXPECT_DOUBLE_EQ(1100000.0,
                   MetricValue("shaka_segment_bytes_total", label));
  EXPECT_DOUBLE_EQ(2.0,
                   MetricValue("shaka_last_segment_duration_seconds", label));
  EXPECT_DOUBLE_EQ((1080000.0 + 180000.0) / 90000.0,
                   MetricValue("shaka_last_segment_timestamp_seconds", label));

  listener.OnCueEvent(1260000, "", /*is_cue_out=*/true, 30.0);
  listener.OnCueEvent(1290000, "", /*is_cue_out=*/false, 0.0);
  EXPECT_DOUBLE_EQ(1.0,
                   MetricValue("shaka_cue_events_total", label, "out"));
  EXPECT_DOUBLE_EQ(1.0, MetricValue("shaka_cue_events_total", label, "in"));

  const std::vector<uint8_t> key_id(16, 1), iv(8, 2);
  listener.OnEncryptionInfoReady(/*is_initial_encryption_info=*/true,
                                 FOURCC_cenc, key_id, iv, {});
  listener.OnEncryptionInfoReady(/*is_initial_encryption_info=*/false,
                                 FOURCC_cenc, key_id, iv, {});
  EXPECT_DOUBLE_EQ(1.0, MetricValue("shaka_key_rotations_total", label));
}

}  // namespace
}  // namespace media
}  // namespace shaka
```

(`muxer_listener_test_helper.h` provides `CreateVideoStreamInfo` / `GetDefaultVideoStreamInfoParams`; the helper .cc is already in the unittest target. If the helper names differ, mirror whatever `mpd_notify_muxer_listener_unittest.cc` uses to build a `StreamInfo`.)

- [ ] **Step 2: Register in CMake and verify failure**

In `packager/media/event/CMakeLists.txt`: add `metrics_muxer_listener.cc` to `media_event` sources, `metrics` to its `target_link_libraries`, `metrics_muxer_listener_unittest.cc` to `media_event_unittest` sources, and `metrics` to the unittest libraries.

```bash
cmake --build build -t media_event_unittest
```

Expected: FAIL — header does not exist.

- [ ] **Step 3: Implement the listener**

Create `packager/media/event/metrics_muxer_listener.h`:

```cpp
// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_MEDIA_EVENT_METRICS_MUXER_LISTENER_H_
#define PACKAGER_MEDIA_EVENT_METRICS_MUXER_LISTENER_H_

#include <string>

#include <packager/media/event/muxer_listener.h>

namespace prometheus {
class Counter;
class Gauge;
}  // namespace prometheus

namespace shaka {
namespace media {

/// MuxerListener that tallies per-segment output metrics into the process
/// MetricsService registry. Purely observational: every callback is a
/// counter/gauge update, no I/O.
class MetricsMuxerListener : public MuxerListener {
 public:
  /// @param stream_label becomes the {stream} label on every metric.
  explicit MetricsMuxerListener(const std::string& stream_label);

  /// @name MuxerListener implementation overrides.
  /// @{
  void OnEncryptionInfoReady(bool is_initial_encryption_info,
                             FourCC protection_scheme,
                             const std::vector<uint8_t>& key_id,
                             const std::vector<uint8_t>& iv,
                             const std::vector<ProtectionSystemSpecificInfo>&
                                 key_system_info) override;
  void OnEncryptionStart() override;
  void OnMediaStart(const MuxerOptions& muxer_options,
                    const StreamInfo& stream_info,
                    int32_t time_scale,
                    ContainerType container_type) override;
  void OnSampleDurationReady(int32_t sample_duration) override;
  void OnMediaEnd(const MediaRanges& media_ranges,
                  float duration_seconds) override;
  void OnNewSegment(const std::string& segment_name,
                    int64_t start_time,
                    int64_t duration,
                    uint64_t segment_file_size,
                    int64_t segment_number) override;
  void OnKeyFrame(int64_t timestamp,
                  uint64_t start_byte_offset,
                  uint64_t size) override;
  void OnCueEvent(int64_t timestamp,
                  const std::string& cue_data,
                  bool is_cue_out,
                  double duration_in_seconds) override;
  /// @}

 private:
  MetricsMuxerListener(const MetricsMuxerListener&) = delete;
  MetricsMuxerListener& operator=(const MetricsMuxerListener&) = delete;

  int32_t time_scale_ = 0;

  prometheus::Counter* const segments_total_;
  prometheus::Counter* const segment_bytes_total_;
  prometheus::Gauge* const last_segment_duration_seconds_;
  prometheus::Gauge* const last_segment_timestamp_seconds_;
  prometheus::Counter* const cue_out_events_total_;
  prometheus::Counter* const cue_in_events_total_;
  prometheus::Counter* const key_rotations_total_;
};

}  // namespace media
}  // namespace shaka

#endif  // PACKAGER_MEDIA_EVENT_METRICS_MUXER_LISTENER_H_
```

Create `packager/media/event/metrics_muxer_listener.cc`:

```cpp
// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/event/metrics_muxer_listener.h>

#include <prometheus/counter.h>
#include <prometheus/gauge.h>

#include <packager/macros/compiler.h>
#include <packager/metrics/metrics_service.h>

namespace shaka {
namespace media {
namespace {

prometheus::Counter* AddCounter(const char* name,
                                const char* help,
                                const prometheus::Labels& labels) {
  return &prometheus::BuildCounter()
              .Name(name)
              .Help(help)
              .Register(MetricsService::Instance().registry())
              .Add(labels);
}

prometheus::Gauge* AddGauge(const char* name,
                            const char* help,
                            const prometheus::Labels& labels) {
  return &prometheus::BuildGauge()
              .Name(name)
              .Help(help)
              .Register(MetricsService::Instance().registry())
              .Add(labels);
}

}  // namespace

MetricsMuxerListener::MetricsMuxerListener(const std::string& stream_label)
    : segments_total_(AddCounter("shaka_segments_emitted_total",
                                 "Media segments emitted.",
                                 {{"stream", stream_label}})),
      segment_bytes_total_(AddCounter("shaka_segment_bytes_total",
                                      "Total bytes of emitted segments.",
                                      {{"stream", stream_label}})),
      last_segment_duration_seconds_(
          AddGauge("shaka_last_segment_duration_seconds",
                   "Duration of the most recent segment.",
                   {{"stream", stream_label}})),
      last_segment_timestamp_seconds_(
          AddGauge("shaka_last_segment_timestamp_seconds",
                   "Media-timeline end of the most recent segment.",
                   {{"stream", stream_label}})),
      cue_out_events_total_(AddCounter("shaka_cue_events_total",
                                       "Ad-cue events by direction.",
                                       {{"stream", stream_label},
                                        {"direction", "out"}})),
      cue_in_events_total_(AddCounter("shaka_cue_events_total",
                                      "Ad-cue events by direction.",
                                      {{"stream", stream_label},
                                       {"direction", "in"}})),
      key_rotations_total_(AddCounter("shaka_key_rotations_total",
                                      "Encryption key rotations.",
                                      {{"stream", stream_label}})) {}

void MetricsMuxerListener::OnEncryptionInfoReady(
    bool is_initial_encryption_info,
    FourCC protection_scheme,
    const std::vector<uint8_t>& key_id,
    const std::vector<uint8_t>& iv,
    const std::vector<ProtectionSystemSpecificInfo>& key_system_info) {
  UNUSED(protection_scheme);
  UNUSED(key_id);
  UNUSED(iv);
  UNUSED(key_system_info);
  if (!is_initial_encryption_info)
    key_rotations_total_->Increment();
}

void MetricsMuxerListener::OnEncryptionStart() {}

void MetricsMuxerListener::OnMediaStart(const MuxerOptions& muxer_options,
                                        const StreamInfo& stream_info,
                                        int32_t time_scale,
                                        ContainerType container_type) {
  UNUSED(muxer_options);
  UNUSED(stream_info);
  UNUSED(container_type);
  time_scale_ = time_scale;
}

void MetricsMuxerListener::OnSampleDurationReady(int32_t sample_duration) {
  UNUSED(sample_duration);
}

void MetricsMuxerListener::OnMediaEnd(const MediaRanges& media_ranges,
                                      float duration_seconds) {
  UNUSED(media_ranges);
  UNUSED(duration_seconds);
}

void MetricsMuxerListener::OnNewSegment(const std::string& segment_name,
                                        int64_t start_time,
                                        int64_t duration,
                                        uint64_t segment_file_size,
                                        int64_t segment_number) {
  UNUSED(segment_name);
  UNUSED(segment_number);
  segments_total_->Increment();
  segment_bytes_total_->Increment(static_cast<double>(segment_file_size));
  if (time_scale_ > 0) {
    last_segment_duration_seconds_->Set(static_cast<double>(duration) /
                                        time_scale_);
    last_segment_timestamp_seconds_->Set(
        static_cast<double>(start_time + duration) / time_scale_);
  }
}

void MetricsMuxerListener::OnKeyFrame(int64_t timestamp,
                                      uint64_t start_byte_offset,
                                      uint64_t size) {
  UNUSED(timestamp);
  UNUSED(start_byte_offset);
  UNUSED(size);
}

void MetricsMuxerListener::OnCueEvent(int64_t timestamp,
                                      const std::string& cue_data,
                                      bool is_cue_out,
                                      double duration_in_seconds) {
  UNUSED(timestamp);
  UNUSED(cue_data);
  UNUSED(duration_in_seconds);
  (is_cue_out ? cue_out_events_total_ : cue_in_events_total_)->Increment();
}

}  // namespace media
}  // namespace shaka
```

- [ ] **Step 4: Run the unit test**

```bash
cmake --build build -t media_event_unittest && ./build/packager/media/event/media_event_unittest --gtest_filter='MetricsMuxerListenerTest.*'
```

Expected: PASS.

- [ ] **Step 5: Wire into the factory**

In `packager/media/event/muxer_listener_factory.cc`, add includes:

```cpp
#include <absl/strings/str_cat.h>

#include <packager/media/event/metrics_muxer_listener.h>
```

In `CreateListener` (line 136), wrap the returned listener so the metrics listener sees every event exactly once (adding it inside the two per-codec `CombinedMuxerListener`s would double-count multi-codec streams):

```cpp
  // Wrap so metrics observe each event exactly once, regardless of the
  // multi-codec fan-out inside.
  std::unique_ptr<CombinedMuxerListener> with_metrics(
      new CombinedMuxerListener);
  with_metrics->AddListener(
      std::make_unique<MetricsMuxerListener>(absl::StrCat(stream_index)));
  with_metrics->AddListener(std::move(multi_codec_listener));
  return with_metrics;
```

(replacing the existing `return multi_codec_listener;`).

- [ ] **Step 6: Run the full event suite + commit**

```bash
./build/packager/media/event/media_event_unittest
```

Expected: all PASS (existing factory tests must not regress; if a test asserts on the concrete returned type, update it to assert behavior through the `MuxerListener` interface instead).

```bash
git add packager/media/event
git commit -m "feat: MetricsMuxerListener for per-segment output metrics"
```

---

### Task 8: Manifest / live-window metrics (MPD side)

**Files:**
- Modify: `packager/mpd/base/representation.h` (+2 public getters)
- Modify: `packager/mpd/base/representation.cc` (getter impl)
- Modify: `packager/mpd/base/simple_mpd_notifier.h` / `.cc` (`GetLiveStats`)
- Create: `packager/mpd/base/mpd_stats_collector.h` / `.cc`
- Modify: `packager/media/event/mpd_notify_muxer_listener.cc` (manifest write counters)
- Modify: `packager/packager.cc` (register collector, `PackagerInternal` member)
- Modify: `packager/mpd/CMakeLists.txt` (sources + link `metrics`)
- Modify: `packager/mpd/base/representation_unittest.cc` (getter test)

**Interfaces:**
- Consumes: `Representation::current_buffer_depth_`/`bandwidth_estimator_` (existing private state), `SimpleMpdNotifier::lock_`/`representation_map_` (existing), `MetricsService` (Task 2).
- Produces:
  - `double Representation::GetLiveBufferDepthSeconds() const`
  - `uint64_t Representation::GetEstimatedBandwidthBps() const`
  - `struct SimpleMpdNotifier::RepresentationLiveStats { uint32_t id; double buffer_depth_seconds; uint64_t bandwidth_bps; }`
  - `std::vector<RepresentationLiveStats> SimpleMpdNotifier::GetLiveStats()`
  - `class MpdStatsCollector : public prometheus::Collectable` (ctor takes `SimpleMpdNotifier*`)
  - Metrics: `shaka_live_buffer_depth_seconds{representation}`, `shaka_output_bandwidth_bps{representation}`, `shaka_manifest_writes_total{representation}`, `shaka_manifest_write_failures_total{representation}`.

- [ ] **Step 1: Write the failing getter test**

In `packager/mpd/base/representation_unittest.cc` (the `RepresentationTest` fixture is already a friend of `Representation`), add:

```cpp
TEST_F(RepresentationTest, LiveStatsGetters) {
  // Follow this file's existing pattern for constructing a Representation
  // with a video MediaInfo (reference_time_scale = 90000 in the defaults
  // used by the AddNewSegment tests — reuse the same fixture/helpers).
  auto representation = CreateRepresentation(GetDefaultVideoMediaInfo());
  ASSERT_TRUE(representation->Init());

  EXPECT_DOUBLE_EQ(0.0, representation->GetLiveBufferDepthSeconds());

  // Two 2-second segments at 90kHz, 1 MB each.
  representation->AddNewSegment(0, 180000, 1000000, 1);
  representation->AddNewSegment(180000, 180000, 1000000, 2);

  EXPECT_DOUBLE_EQ(4.0, representation->GetLiveBufferDepthSeconds());
  // 2 MB over 4 s = 4 Mbit/s.
  EXPECT_EQ(4000000u, representation->GetEstimatedBandwidthBps());
}
```

Adapt the construction lines to the fixture's real helper names (`CreateRepresentation` / default MediaInfo helpers exist in this file for the `AddNewSegment` tests; keep the timescale the fixture uses and adjust the expected values arithmetically if it is not 90000: depth = sum(durations)/timescale, bandwidth = total_bits/total_seconds rounded up).

Run to verify failure:

```bash
cmake --build build -t mpd_unittest && ./build/packager/mpd/mpd_unittest --gtest_filter='RepresentationTest.LiveStatsGetters'
```

Expected: FAIL to compile (getters missing). (If the mpd test binary has a different name, find it with `grep add_gtest packager/mpd/CMakeLists.txt`.)

- [ ] **Step 2: Implement getters + GetLiveStats**

`packager/mpd/base/representation.h`, public section (after `GetStartAndEndTimestamps`):

```cpp
  /// @return The current live (dynamic MPD) window depth in seconds; 0
  ///         before any segment is added.
  double GetLiveBufferDepthSeconds() const;

  /// @return The measured output bandwidth estimate in bits per second.
  uint64_t GetEstimatedBandwidthBps() const;
```

`packager/mpd/base/representation.cc` (uses the file-local `GetTimeScale` helper):

```cpp
double Representation::GetLiveBufferDepthSeconds() const {
  const int32_t time_scale = GetTimeScale(media_info_);
  if (time_scale <= 0)
    return 0;
  return static_cast<double>(current_buffer_depth_) / time_scale;
}

uint64_t Representation::GetEstimatedBandwidthBps() const {
  return bandwidth_estimator_.Estimate();
}
```

`packager/mpd/base/simple_mpd_notifier.h`, public section:

```cpp
  /// Point-in-time live stats per representation, for metrics export.
  struct RepresentationLiveStats {
    uint32_t id;
    double buffer_depth_seconds;
    uint64_t bandwidth_bps;
  };
  std::vector<RepresentationLiveStats> GetLiveStats();
```

`packager/mpd/base/simple_mpd_notifier.cc`:

```cpp
std::vector<SimpleMpdNotifier::RepresentationLiveStats>
SimpleMpdNotifier::GetLiveStats() {
  absl::MutexLock lock(&lock_);
  std::vector<RepresentationLiveStats> stats;
  stats.reserve(representation_map_.size());
  for (const auto& entry : representation_map_) {
    stats.push_back({entry.first,
                     entry.second->GetLiveBufferDepthSeconds(),
                     entry.second->GetEstimatedBandwidthBps()});
  }
  return stats;
}
```

- [ ] **Step 3: Run the getter test**

```bash
cmake --build build -t mpd_unittest && ./build/packager/mpd/mpd_unittest --gtest_filter='RepresentationTest.LiveStatsGetters'
```

Expected: PASS.

- [ ] **Step 4: Add the collector**

Create `packager/mpd/base/mpd_stats_collector.h`:

```cpp
// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_MPD_BASE_MPD_STATS_COLLECTOR_H_
#define PACKAGER_MPD_BASE_MPD_STATS_COLLECTOR_H_

#include <vector>

#include <prometheus/collectable.h>
#include <prometheus/metric_family.h>

namespace shaka {

class SimpleMpdNotifier;

/// Scrape-time exporter of per-representation live-window stats (buffer
/// depth, measured bandwidth). The notifier must outlive this collector's
/// last Collect() call — guaranteed by Packager stopping the metrics
/// exposer before pipeline teardown.
class MpdStatsCollector : public prometheus::Collectable {
 public:
  explicit MpdStatsCollector(SimpleMpdNotifier* notifier);

  std::vector<prometheus::MetricFamily> Collect() const override;

 private:
  MpdStatsCollector(const MpdStatsCollector&) = delete;
  MpdStatsCollector& operator=(const MpdStatsCollector&) = delete;

  SimpleMpdNotifier* const notifier_;
};

}  // namespace shaka

#endif  // PACKAGER_MPD_BASE_MPD_STATS_COLLECTOR_H_
```

Create `packager/mpd/base/mpd_stats_collector.cc`:

```cpp
// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/mpd/base/mpd_stats_collector.h>

#include <string>

#include <packager/mpd/base/simple_mpd_notifier.h>

namespace shaka {

MpdStatsCollector::MpdStatsCollector(SimpleMpdNotifier* notifier)
    : notifier_(notifier) {}

std::vector<prometheus::MetricFamily> MpdStatsCollector::Collect() const {
  const auto stats = notifier_->GetLiveStats();

  prometheus::MetricFamily depth;
  depth.name = "shaka_live_buffer_depth_seconds";
  depth.help = "Live (dynamic MPD) window depth per representation.";
  depth.type = prometheus::MetricType::Gauge;

  prometheus::MetricFamily bandwidth;
  bandwidth.name = "shaka_output_bandwidth_bps";
  bandwidth.help = "Measured output bandwidth per representation.";
  bandwidth.type = prometheus::MetricType::Gauge;

  for (const auto& entry : stats) {
    prometheus::ClientMetric metric;
    metric.label.push_back({"representation", std::to_string(entry.id)});
    metric.gauge.value = entry.buffer_depth_seconds;
    depth.metric.push_back(metric);
    metric.gauge.value = static_cast<double>(entry.bandwidth_bps);
    bandwidth.metric.push_back(std::move(metric));
  }
  return {std::move(depth), std::move(bandwidth)};
}

}  // namespace shaka
```

In `packager/mpd/CMakeLists.txt`: add `base/mpd_stats_collector.cc` + `.h` to the `mpd_builder` sources and `metrics` to its `target_link_libraries`.

- [ ] **Step 5: Manifest write counters**

In `packager/media/event/mpd_notify_muxer_listener.cc`, add includes:

```cpp
#include <absl/strings/str_cat.h>
#include <prometheus/counter.h>

#include <packager/metrics/metrics_service.h>
```

Anonymous-namespace helper:

```cpp
// Tallies live per-segment manifest writes; failures are the discarded
// bool returns of NotifyNewSegment/Flush.
void CountManifestWrite(uint32_t representation_id, bool ok) {
  static auto& writes =
      prometheus::BuildCounter()
          .Name("shaka_manifest_writes_total")
          .Help("Live manifest update attempts, per representation.")
          .Register(MetricsService::Instance().registry());
  static auto& failures =
      prometheus::BuildCounter()
          .Name("shaka_manifest_write_failures_total")
          .Help("Failed live manifest updates, per representation.")
          .Register(MetricsService::Instance().registry());
  const prometheus::Labels labels{
      {"representation", absl::StrCat(representation_id)}};
  writes.Add(labels).Increment();
  if (!ok)
    failures.Add(labels).Increment();
}
```

Rewrite the live branch of `OnNewSegment` (lines 219–224):

```cpp
  if (mpd_notifier_->dash_profile() == DashProfile::kLive) {
    const bool notified = mpd_notifier_->NotifyNewSegment(
        notification_id_.value(), start_time, duration, segment_file_size,
        segment_number);
    bool flushed = true;
    if (mpd_notifier_->mpd_type() == MpdType::kDynamic)
      flushed = mpd_notifier_->Flush();
    CountManifestWrite(notification_id_.value(), notified && flushed);
  } else {
```

Add `metrics` to `media_event`'s `target_link_libraries` if Task 7 has not already done so.

- [ ] **Step 6: Register the collector in Packager**

In `packager/packager.cc`:

Include:

```cpp
#include <packager/mpd/base/mpd_stats_collector.h>
```

In `PackagerInternal` (line 954), after `std::unique_ptr<MpdNotifier> mpd_notifier;`:

```cpp
  // Declared after mpd_notifier: destroyed first, and the exposer is
  // stopped in ~Packager before either goes away.
  std::shared_ptr<MpdStatsCollector> mpd_stats_collector;
```

In `Initialize`, inside the `if (!mpd_params.mpd_output.empty())` block after the successful `internal->mpd_notifier->Init()` (line 1047):

```cpp
    internal->mpd_stats_collector = std::make_shared<MpdStatsCollector>(
        static_cast<SimpleMpdNotifier*>(internal->mpd_notifier.get()));
    MetricsService::Instance().RegisterCollectable(
        internal->mpd_stats_collector);
```

- [ ] **Step 7: Build everything + run mpd/event suites**

```bash
cmake --build build -t mpd_unittest media_event_unittest packager
./build/packager/mpd/mpd_unittest && ./build/packager/media/event/media_event_unittest
```

Expected: all PASS.

- [ ] **Step 8: Commit**

```bash
git add packager/mpd packager/media/event/mpd_notify_muxer_listener.cc packager/packager.cc
git commit -m "feat: manifest and live-window metrics (buffer depth, bandwidth, write failures)"
```

---

### Task 9: End-to-end integration tests

**Files:**
- Modify: `packager/app/test/packager_test.py` (two tests, next to `testRedundantUdpInputSurvivesLegKill` at line 1335)

**Interfaces:**
- Consumes: everything above; the replay tool `packager/tools/redundant_ts/replay_ts.py`; `--metrics_port`.
- Produces: regression coverage that a live run serves every metric family.

- [ ] **Step 1: Write the tests** (modeled line-by-line on the existing redundant e2e test at `packager_test.py:1335` — same port probing, same subprocess/terminate pattern; `urllib.request` scrapes the endpoint while the packager runs):

```python
  def _probeFreePorts(self, count, socket_type):
    import socket as socket_module
    ports = []
    for _ in range(count):
      probe = socket_module.socket(socket_module.AF_INET, socket_type)
      probe.bind(('127.0.0.1', 0))
      ports.append(probe.getsockname()[1])
      probe.close()
    return ports

  def _scrapeMetrics(self, port):
    import urllib.request
    with urllib.request.urlopen(
        'http://127.0.0.1:%d/metrics' % port, timeout=5) as response:
      return response.read().decode('utf8')

  def testMetricsEndpointServesLiveFamilies(self):
    # Live DASH from redundant UDP legs with --metrics_port: every metric
    # family must appear on /metrics with sane values while running.
    import socket as socket_module
    udp_ports = self._probeFreePorts(2, socket_module.SOCK_DGRAM)
    metrics_port = self._probeFreePorts(1, socket_module.SOCK_STREAM)[0]

    url = ('redundant://udp://127.0.0.1:%d?timeout=100000'
           '|udp://127.0.0.1:%d?timeout=100000') % tuple(udp_ports)
    stream = ('input=%s,stream=video,init_segment=%s/video_init.mp4,'
              'segment_template=%s/video_$Number$.m4s') % (
                  url, self.tmp_dir, self.tmp_dir)
    cmd = [
        test_env.PACKAGER_BIN, stream,
        '--mpd_output', os.path.join(self.tmp_dir, 'output.mpd'),
        '--segment_duration', '1',
        '--time_shift_buffer_depth', '10',
        '--metrics_port', str(metrics_port),
        '--test_packager_version', '<tag>-<hash>-<test>',
    ]
    packager_process = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE)
    try:
      replay_tool = os.path.join(test_env.SRC_DIR, 'packager', 'tools',
                                 'redundant_ts', 'replay_ts.py')
      ts_file = os.path.join(self.test_data_dir, 'bear-640x360.ts')
      import time as time_module
      time_module.sleep(2)  # Let the packager bind sockets and the exposer.
      subprocess.check_call([
          'python3', replay_tool, ts_file,
          '--ports', str(udp_ports[0]), str(udp_ports[1]),
          '--pps', '300',
      ])
      time_module.sleep(2)  # Let segments and manifest writes land.
      metrics = self._scrapeMetrics(metrics_port)
    finally:
      packager_process.terminate()
      _, stderr = packager_process.communicate(timeout=30)

    for family in [
        'shaka_build_info',
        'shaka_udp_bytes_received_total',
        'shaka_udp_datagrams_received_total',
        'shaka_redundant_leg_packets_total',
        'shaka_media_latest_pts_seconds',
        'shaka_segments_emitted_total',
        'shaka_manifest_writes_total',
        'shaka_live_buffer_depth_seconds',
        'shaka_output_bandwidth_bps',
    ]:
      self.assertIn(family, metrics,
                    'missing %s in /metrics; stderr:\n%s' %
                    (family, stderr.decode('utf8', 'replace')))

    match = re.search(r'shaka_segments_emitted_total\{[^}]*\} ([0-9.e+]+)',
                      metrics)
    self.assertIsNotNone(match)
    self.assertGreater(float(match.group(1)), 0)

  def testMetricsFailoverSwitchCounter(self):
    # Failover mode: killing the active leg must show up on /metrics as a
    # switch and an unhealthy leg 0.
    import socket as socket_module
    udp_ports = self._probeFreePorts(2, socket_module.SOCK_DGRAM)
    metrics_port = self._probeFreePorts(1, socket_module.SOCK_STREAM)[0]

    url = ('redundant://udp://127.0.0.1:%d?timeout=100000'
           '|udp://127.0.0.1:%d?timeout=100000&mode=failover') % tuple(
               udp_ports)
    stream = ('input=%s,stream=video,init_segment=%s/video_init.mp4,'
              'segment_template=%s/video_$Number$.m4s') % (
                  url, self.tmp_dir, self.tmp_dir)
    cmd = [
        test_env.PACKAGER_BIN, stream,
        '--mpd_output', os.path.join(self.tmp_dir, 'output.mpd'),
        '--segment_duration', '1',
        '--metrics_port', str(metrics_port),
        '--test_packager_version', '<tag>-<hash>-<test>',
    ]
    packager_process = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE)
    try:
      replay_tool = os.path.join(test_env.SRC_DIR, 'packager', 'tools',
                                 'redundant_ts', 'replay_ts.py')
      ts_file = os.path.join(self.test_data_dir, 'bear-640x360.ts')
      import time as time_module
      time_module.sleep(2)
      subprocess.check_call([
          'python3', replay_tool, ts_file,
          '--ports', str(udp_ports[0]), str(udp_ports[1]),
          '--pps', '300', '--kill', '0:0.5',
      ])
      time_module.sleep(2)  # > failover_timeout_ms; health tick fires.
      metrics = self._scrapeMetrics(metrics_port)
    finally:
      packager_process.terminate()
      _, stderr = packager_process.communicate(timeout=30)

    switches = re.search(
        r'shaka_redundant_switches_total\{[^}]*\} ([0-9.e+]+)', metrics)
    self.assertIsNotNone(switches, 'no switches metric; stderr:\n%s' %
                         stderr.decode('utf8', 'replace'))
    self.assertGreaterEqual(float(switches.group(1)), 1.0)
    leg0 = re.search(
        r'shaka_redundant_leg_healthy\{[^}]*leg="0"[^}]*\} ([0-9.e+]+)',
        metrics)
    self.assertIsNotNone(leg0)
    self.assertEqual(0.0, float(leg0.group(1)))
```

Check the exact flag name for DASH live window with `./build/packager/packager --help | grep time_shift`; drop that flag if absent — it is not load-bearing for the assertions.

- [ ] **Step 2: Run the two tests**

```bash
cmake --build build -t packager
cd build/packager && python3 packager_test.py PackagerFunctionalTest.testMetricsEndpointServesLiveFamilies PackagerFunctionalTest.testMetricsFailoverSwitchCounter; cd -
```

(Match the test-class name used by the neighboring redundant test; run it the same way that test is run — check the top of `packager_test.py` for the unittest invocation convention and the env vars `PACKAGER_BIN`/`PACKAGER_SRC_DIR` set in `packager/CMakeLists.txt:282-284`; export them manually if running outside ctest.)

Expected: both PASS. Known macOS caveat: no `shaka_udp_kernel_drops_total` — the family list above deliberately omits it.

- [ ] **Step 3: Run the existing redundant e2e test to catch regressions**

```bash
cd build/packager && python3 packager_test.py PackagerFunctionalTest.testRedundantUdpInputSurvivesLegKill; cd -
```

Expected: PASS (log-line format and merge behavior unchanged).

- [ ] **Step 4: Commit**

```bash
git add packager/app/test/packager_test.py
git commit -m "test: end-to-end /metrics scrape and failover-switch coverage"
```

---

### Task 10: Documentation

**Files:**
- Create: `docs/source/options/metrics_options.rst`
- Modify: `docs/source/tutorials/live.rst` (paragraph next to the existing redundant-input section)

**Interfaces:**
- Consumes: final metric names/flags from Tasks 3–8.
- Produces: user-facing reference.

- [ ] **Step 1: Write the options include**

Create `docs/source/options/metrics_options.rst` (follow the structure of `docs/source/options/redundant_input_options.rst`):

```rst
Metrics options
^^^^^^^^^^^^^^^

--metrics_port <port>

    Port for the Prometheus metrics HTTP endpoint (``/metrics``).
    0 (default) disables the endpoint.

--metrics_bind_address <address>

    Bind address for the metrics endpoint. Defaults to ``0.0.0.0``.

Exported metrics (prefix ``shaka_``, Prometheus text format):

* Input (label ``input`` = the input URL): ``shaka_udp_bytes_received_total``,
  ``shaka_udp_datagrams_received_total``, ``shaka_udp_recv_timeouts_total``,
  ``shaka_udp_recv_errors_total``,
  ``shaka_udp_last_receive_timestamp_seconds`` and, on Linux only,
  ``shaka_udp_kernel_drops_total`` (kernel receive-queue drops via
  ``SO_RXQ_OVFL``; absent on other platforms).
* Redundant input (labels ``input``, ``leg``):
  ``shaka_redundant_leg_packets_total``,
  ``shaka_redundant_leg_dropped_dup_total``,
  ``shaka_redundant_leg_resyncs_total``,
  ``shaka_redundant_leg_cc_errors_total``, ``shaka_redundant_leg_healthy``,
  ``shaka_redundant_leg_active``, ``shaka_redundant_switches_total``,
  ``shaka_redundant_emitted_cc_errors_total``, ``shaka_redundant_max_skew_ms``
  and ``shaka_redundant_window_evictions_total``. Same semantics as the
  once-per-minute ``redundant_input:`` log line, which remains available.
* MPEG-TS parse health: ``shaka_ts_cc_errors_total`` and
  ``shaka_ts_pes_errors_total`` (label ``pid``),
  ``shaka_ts_tei_packets_total``, ``shaka_ts_unsupported_streams_total`` and
  ``shaka_media_latest_pts_seconds`` (label ``pid``; input staleness signal).
* Output segments (label ``stream``): ``shaka_segments_emitted_total``,
  ``shaka_segment_bytes_total``, ``shaka_last_segment_duration_seconds``,
  ``shaka_last_segment_timestamp_seconds``, ``shaka_cue_events_total``
  (label ``direction`` = ``in``/``out``) and ``shaka_key_rotations_total``.
* Manifest / live state (label ``representation``, DASH):
  ``shaka_manifest_writes_total``, ``shaka_manifest_write_failures_total``,
  ``shaka_live_buffer_depth_seconds`` and ``shaka_output_bandwidth_bps``.
* Process: ``shaka_build_info`` (label ``version``, value always 1).

Counters are maintained whether or not the endpoint is enabled;
``--metrics_port`` only controls the HTTP listener. One packager process
serves one endpoint; channel identity should come from deployment labels
(e.g. one process per channel, scraped per pod).
```

- [ ] **Step 2: Reference it from the live tutorial**

In `docs/source/tutorials/live.rst`, next to where `redundant_input_options.rst` is included (grep `redundant` in the file for the exact spot), add a short section:

```rst
Monitoring live channels
------------------------

Pass ``--metrics_port`` to expose real-time counters and gauges in
Prometheus text format on ``/metrics`` — input liveness, redundant-leg
health, TS parse errors, emitted segments and live manifest state.

.. include:: /options/metrics_options.rst
```

Match the heading style and include mechanics the file already uses for the redundant-input section.

- [ ] **Step 3: Commit**

```bash
git add docs/source/options/metrics_options.rst docs/source/tutorials/live.rst
git commit -m "docs: metrics endpoint flags and metric inventory"
```

---

## Final verification (after all tasks)

- [ ] Full unit-test sweep: `ctest --test-dir build` (or at minimum `file_unittest`, `metrics_unittest`, `mp2t_unittest`, `media_event_unittest`, `mpd_unittest`).
- [ ] Both new e2e tests plus `testRedundantUdpInputSurvivesLegKill` pass.
- [ ] `./build/packager/packager --help` shows both flags; running with `--metrics_port` on a busy port fails `Initialize` with a clear error (spec edge case): start `nc -l <port>` and confirm.
- [ ] Confirm no behavior change with metrics disabled: run any pre-existing packaging test without `--metrics_port` and confirm identical outputs.

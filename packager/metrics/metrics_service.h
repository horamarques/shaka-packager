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

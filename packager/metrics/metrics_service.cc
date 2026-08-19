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

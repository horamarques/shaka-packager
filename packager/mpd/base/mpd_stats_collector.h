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
/// last Collect() call -- guaranteed by Packager stopping the metrics
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

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

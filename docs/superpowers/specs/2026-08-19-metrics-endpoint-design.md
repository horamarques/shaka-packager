# Prometheus Metrics Endpoint — Feature Design

Date: 2026-08-19
Status: approved
Approach: prometheus-cpp (vendored) with its built-in pull Exposer — chosen by
Pedro over a hand-rolled registry and over log-scrape-only alternatives.

## Feature summary

Expose real-time live-channel metrics from a running packager process over
HTTP in Prometheus text exposition format, at `/metrics` on an opt-in port.
Covers five stat families: UDP input, redundant input merger, TS parse
health, output segments, and manifest/live state. Off by default; when
disabled the packager behaves identically to today (counters are still
maintained; only the HTTP listener is gated).

Non-goals for v1:
- Histograms (consistent with the max-skew "max only" precedent).
- Push protocols (StatsD, OTLP, remote-write).
- A JSON status endpoint.
- Multi-`Packager`-instance support in one process (see Lifecycle).
- Channel identity labels: one process = one channel; identity comes from
  deployment labels (pod/host), not from the process.

## Dependency: prometheus-cpp

- New submodule `packager/third_party/prometheus-cpp/source` →
  `https://github.com/jupp0r/prometheus-cpp`, pinned to the latest release
  tag (v1.3.x line). Its bundled civetweb comes via `--recursive` checkout.
- Wrapper `packager/third_party/prometheus-cpp/CMakeLists.txt` in the
  existing pattern (set options, then `add_subdirectory(source
  EXCLUDE_FROM_ALL)`), with:
  - `ENABLE_PUSH=OFF` (no pushgateway, drops the curl dependency)
  - `ENABLE_COMPRESSION=OFF` (no zlib coupling)
  - `ENABLE_TESTING=OFF`
- One `add_subdirectory(prometheus-cpp EXCLUDE_FROM_ALL)` line in
  `packager/third_party/CMakeLists.txt`.
- Link `prometheus-cpp::core` and `prometheus-cpp::pull` where used.
- Mongoose remains test-only; this feature does not touch it.

## Components

### MetricsService (new: packager/metrics/)

Singleton owning the shared `prometheus::Registry` and, when enabled, the
`prometheus::Exposer`.

Why a singleton: the `File` layer has no dependency injection —
`UdpFile`/`RedundantUdpFile` are constructed by the File factory from a URL
string, and the TS parser is buried inside the demuxer. Threading a registry
handle through those constructors would fork far more upstream code than a
single well-bounded global.

API surface (approximate):

```cpp
class MetricsService {
 public:
  static MetricsService& Instance();

  // Idempotent per process. Returns error Status if an exposer is already
  // running (one Packager instance per process is the supported model).
  Status StartExposer(const std::string& bind_address, int port);
  void StopExposer();

  prometheus::Registry& registry();
  // For scrape-time snapshot sources.
  void RegisterCollectable(std::weak_ptr<prometheus::Collectable> c);
};
```

Two consumption patterns:
1. **Hot-path counters** — components fetch `prometheus::Counter&` /
   `prometheus::Gauge&` handles once at construction
   (`BuildCounter().Name(...).Register(registry).Add({labels})`); per-event
   increment is a lock-free atomic add. No string lookups on hot paths.
2. **Scrape-time Collectables** — small adapter classes implementing
   `prometheus::Collectable::Collect()` for state that already lives under
   another component's lock (merger snapshot, MPD live-window state). The
   Exposer holds them as `weak_ptr`, so component destruction mid-scrape is
   structurally safe: the weak_ptr expires and the scrape skips it.

### Configuration plumbing

- `PackagingParams::metrics_port` (int, 0 = disabled, default 0) and
  `PackagingParams::metrics_bind_address` (string, default `"0.0.0.0"`).
- CLI flags `--metrics_port` / `--metrics_bind_address` in the app layer,
  mapped in `packager_main.cc`.
- `Packager::Initialize` calls `StartExposer` when `metrics_port > 0`;
  `Packager` teardown calls `StopExposer` before pipeline teardown.

## Metric inventory

Prefix `shaka_`. Types: `_total` = counter, others = gauge. Labels bounded:
`{input}` = input index, `{stream}` = output stream index/type, `{leg}` =
redundant leg index, `{pid}` = TS PID (bounded by the mux, ~dozens).

### Input (UDP) — labels {input}

| Metric | Source |
|---|---|
| `shaka_udp_bytes_received_total` | `UdpFile::Read` |
| `shaka_udp_datagrams_received_total` | `UdpFile::Read` |
| `shaka_udp_recv_timeouts_total` | `UdpFile::Read` error path, classified via `GetSocketErrorCode()` |
| `shaka_udp_recv_errors_total` | same, non-timeout errors |
| `shaka_udp_kernel_drops_total` | Linux `SO_RXQ_OVFL` cmsg via `recvmsg`; compile-guarded, absent on other platforms |
| `shaka_udp_last_receive_timestamp_seconds` | `UdpFile::Read` |

### Redundant input — via Collectable snapshot

Per leg `{leg}`: `shaka_redundant_leg_packets_total`,
`shaka_redundant_leg_dropped_dup_total`, `shaka_redundant_leg_resyncs_total`,
`shaka_redundant_leg_cc_errors_total`, `shaka_redundant_leg_healthy` (0/1),
`shaka_redundant_leg_active` (0/1).
Global: `shaka_redundant_switches_total`,
`shaka_redundant_emitted_cc_errors_total`, `shaka_redundant_max_skew_ms`,
`shaka_redundant_window_evictions_total`.

Backed by a new `RedundantUdpFile::GetStatsSnapshot()` that locks
`merger_mutex_` and copies the counters out. `MaybeLogStats` is rewritten on
top of the same snapshot so the log line and the endpoint cannot disagree.
The once-per-minute `redundant_input:` log line is kept unchanged in format.

### TS parse health

| Metric | Site (existing detection, new increment) |
|---|---|
| `shaka_ts_cc_errors_total{pid}` | `mp2t_media_parser.cc` `PidState::PushTsPacket` discontinuity branch |
| `shaka_ts_pes_errors_total{pid}` | `mp2t_media_parser.cc` "Parsing failed for pid" branch |
| `shaka_ts_unsupported_streams_total` | `mp2t_media_parser.cc` "Ignoring unsupported stream" branch |
| `shaka_ts_tei_packets_total` | `ts_packet.cc` — `transport_error_indicator` is parsed and currently discarded |
| `shaka_media_latest_pts_seconds{pid}` | parser's existing biggest-PTS tracker (staleness signal; per-PID state) |

### Output segments — via new MetricsMuxerListener, labels {stream}

`shaka_segments_emitted_total`, `shaka_segment_bytes_total`,
`shaka_last_segment_duration_seconds`,
`shaka_last_segment_timestamp_seconds`,
`shaka_cue_events_total{direction=in|out}`, `shaka_key_rotations_total`.

`MetricsMuxerListener` implements the existing `MuxerListener` interface and
is appended in `muxer_listener_factory.cc` (fan-out via the existing
`CombinedMuxerListener`); zero changes to any muxer.

### Manifest / live state — labels {representation}

| Metric | Source |
|---|---|
| `shaka_manifest_writes_total` | `mpd_notify_muxer_listener.cc` — count the `NotifyNewSegment`/`Flush` calls whose bool returns are currently discarded |
| `shaka_manifest_write_failures_total` | same site, false returns |
| `shaka_live_buffer_depth_seconds` | `Representation::current_buffer_depth_` via new const getter, read under the notifier's existing `absl::Mutex`, exposed via Collectable |
| `shaka_output_bandwidth_bps` | existing `BandwidthEstimator::Estimate()` via new const getter, same Collectable |

### Process

`shaka_build_info{version="..."}` gauge, value 1.

## Threading & lifecycle

- Writers: job threads (demux/mux pipeline), IO thread-pool workers
  (`UdpFile::Read` under `ThreadedIoFile`), redundant reader threads.
  Readers: civetweb scrape threads inside the Exposer.
- prometheus-cpp counters/gauges are internally atomic — no new locking on
  hot paths.
- Collectables take the owning component's existing lock briefly inside
  `Collect()` (copy a few integers, microseconds).
- `RedundantInputMerger` stays non-thread-safe pure logic; all merger access
  remains serialized under `RedundantUdpFile::merger_mutex_`, including the
  new snapshot.
- Exposer lifetime is inside `Packager` lifetime: started in `Initialize`,
  stopped before pipeline teardown. weak_ptr registration makes
  scrape-vs-teardown races structurally impossible.
- Supported model: one `Packager` instance per process (the CLI case). A
  second concurrent `StartExposer` returns an error Status.

## Edge cases (specified behavior)

- `--metrics_port` with a port already in use: `Packager::Initialize` fails
  with a clear error Status (fail loud, not degrade silently).
- Metrics disabled (default): all counter increments still execute (atomic
  add, negligible); no listener socket is opened; behavior otherwise
  byte-identical to today.
- Non-Linux platforms: `shaka_udp_kernel_drops_total` is absent (not zero) —
  absence is the documented signal that the platform cannot measure it.
- Scrape during startup (families registered but no traffic yet): counters
  report 0; `last_*` gauges report 0 until first event.
- VOD runs: the endpoint works identically; live-window gauges simply stay
  at their VOD-meaningless values. No special-casing.

## Test matrix

Unit (gtest, alongside existing patterns):
- `MetricsService`: start/stop, double-start error, registry access.
- `MetricsMuxerListener`: synthetic `OnNewSegment`/`OnCueEvent`/
  `OnEncryptionInfoReady` calls produce the expected counter/gauge values.
- `RedundantUdpFile::GetStatsSnapshot`: snapshot matches merger getters;
  log line derived from the same snapshot.
- `UdpFile` counters: bytes/datagrams tally against a loopback sender
  (extends existing udp_file test setup).
- TS parser counters: feed packets with CC gaps / TEI set, assert counts.

Integration (extends the e2e tooling from the redundant-input work):
- Run a live packaging job with `--metrics_port`, curl `/metrics`, assert
  each family is present with sane values (segments > 0, bytes > 0,
  buffer depth > 0 after a few segments).
- Redundant run: kill one leg, assert `shaka_redundant_leg_healthy` drops
  and `shaka_redundant_switches_total` increments (failover mode).

macOS caveat (test environment): kernel-drop metric compiles out; tests
assert its absence on Darwin rather than failing.

## Rollout

- Fork-only feature; follow the existing fork workflow (feature commits on
  main, fork issues for any upstream bugs discovered).
- Docs: new `docs/source/options/metrics_options.rst` include (flag
  reference + metric inventory) and a paragraph in the live tutorial next
  to the existing redundant-input section.
- Deployment note: one process per channel; scrape per pod; channel label
  comes from pod metadata, not the process.

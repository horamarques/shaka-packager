# Packager WebAPI (OpenAPI) — Feature Design

Date: 2026-08-21
Status: approved (design approved in chat; spec pending Pedro's review)
Prior art: Pedro's VxPackager fork (github.com/horamarques/VxPackager, private) —
oatpp REST service over shaka::Packager with live start/stop/status/list,
VOD packaging, and Swagger UI. This design ports its proven shape into this
fork and extends it into a full origin-server API.

## Feature summary

A new `packager-api` executable exposing an OpenAPI-documented REST service
that (1) starts, stops, and supervises packaging events, and (2) offers
unitary media operations (package/encrypt/inspect single segments, generate
manifests and DRM artifacts on demand) so the fork can serve as the media
engine of a streaming origin server.

Delivered in three phases against one spec:
- **Phase 1 — Event control**: API server, OpenAPI/Swagger, subprocess
  event supervision.
- **Phase 2 — JIT origin loop**: the eleven low-effort unitary ops that
  reuse one shared in-process pipeline harness.
- **Phase 3 — Advanced ops**: the six ops needing deeper pipeline work.

Non-goals (all phases):
- Auto-restart policies for events (exit code is reported; the operator or
  an orchestrator decides).
- Event persistence across API restarts (in-memory registry only; documented).
- Input auto-probing on event creation.
- Serving media segments themselves (the origin's HTTP layer does that; this
  API produces artifacts, it is not the delivery path).
- FFmpeg/FFprobe dependencies: the I-frame playlist op uses shaka's native
  keyframe data, NOT VxPackager's FFprobe approach.

## Architecture

### Binary and layout

New executable `packager-api`, linking `libpackager`, built from
`packager/webapi/` (mirrors VxPackager's layout):

```
packager/webapi/
  CMakeLists.txt
  main.cc                  flags, oatpp bootstrap, component wiring
  controller/              oatpp controllers (annotated -> OpenAPI)
    health_controller.*
    event_controller.*
    ops_controller.*
  model/                   oatpp DTOs (request/response schemas)
  service/
    event_manager.*        subprocess supervision (phase 1)
    ops_pipeline.*         shared in-process one-shot Packager harness (phase 2)
    <one .cc per op family>
```

The main `packager` binary is untouched.

### Dependencies

- **oatpp** and **oatpp-swagger**, vendored as submodules under
  `packager/third_party/<name>/source` with wrapper CMakeLists (the
  established pattern), pinned to the same 1.3.x release tag pair
  (oatpp and oatpp-swagger releases are versioned in lockstep; pin the
  newest 1.3.x at implementation time and record it).
- oatpp is namespaced C++ — no C symbol-collision risk with the vendored
  mongoose/civetweb (lesson from the metrics build).
- Swagger UI static resources ship with oatpp-swagger; `OATPP_SWAGGER_RES_PATH`
  is wired via compile definition as in VxPackager.

### Execution models

- **Events (phase 1): subprocess per event.** The API spawns the existing
  `packager` binary with generated argv and supervises it. Rationale:
  crash isolation (one bad stream cannot take down the API or other
  events), and each event keeps its own `--metrics_port`, preserving the
  one-Packager-per-process model the metrics feature is built on.
- **Unitary ops (phases 2-3): in-process.** Each op builds a one-shot
  `shaka::Packager` with `BufferCallbackParams` (callback:// I/O — shaka's
  official in-memory embedding hook) and runs it to completion inside the
  API process. Rationale: request/response latency; process spawn per JIT
  segment would cost ~100ms+ where an origin wants milliseconds. Ops run
  with `metrics_port = 0` (no exposer conflict); their counters aggregate
  into the API process's own registry.

## Configuration (flags on packager-api)

| Flag | Default | Meaning |
|---|---|---|
| `--api_port` | 8088 | HTTP port (VxPackager's default) |
| `--api_bind_address` | 0.0.0.0 | Bind address |
| `--api_token` | "" | When set, all `/api/v1/*` requests require `Authorization: Bearer <token>`; `/health` and `/swagger/*` stay open |
| `--packager_bin` | packager next to packager-api | Path to the packager binary for event subprocesses |
| `--event_metrics_port_range` | 19100-19199 | Port pool for per-event `--metrics_port` |
| `--event_log_dir` | system temp | Per-event stderr capture directory |
| `--metrics_port` | 0 | The API process's own Prometheus endpoint (reuses MetricsService; serves unitary-op counters) |

Security posture: unauthenticated by default like the metrics endpoint;
docs instruct binding to a management interface or fronting with a proxy,
and recommend `--api_token` for anything beyond a closed network.

## Phase 1 — Event control API

### Endpoints

| Method + path | Behavior |
|---|---|
| `GET /health` | 200 with packager version; no auth |
| `POST /api/v1/events` | Create + start an event; 201 with event resource; 409 if the client-supplied id exists |
| `GET /api/v1/events` | List: id, state, uptime_seconds, input summary |
| `GET /api/v1/events/{id}` | Full status: state, pid, exit_code (when dead), argv, metrics_port, log_path, created/started/stopped timestamps |
| `DELETE /api/v1/events/{id}?mode=drain\|kill` | Stop. drain (default) = SIGTERM, escalate to SIGKILL after `stop_timeout_seconds` (event param, default 10). kill = SIGKILL now. 202 while STOPPING |
| `GET /api/v1/events/{id}/logs?tail=N` | Last N lines (default 100) of the event's stderr capture |
| `GET /api/v1/events/{id}/metrics` | Reverse-proxy of one scrape of the event's own `/metrics` (Prometheus text passthrough) |
| `GET /swagger/ui`, `GET /swagger/doc` | Swagger UI and OpenAPI document, generated from DTO annotations |

### Event DTO (create request)

A typed subset of the packager CLI, deliberately mirroring flag names so the
docs transfer:

```json
{
  "event_id": "sport1",                      // optional; server generates UUID if absent
  "streams": [{
    "input": "redundant://udp://...|udp://...",  // any packager-supported URL
    "stream": "video",                            // stream selector
    "init_segment": "/var/www/live/sport1/video_init.mp4",
    "segment_template": "/var/www/live/sport1/video_$Number$.m4s"
  }],
  "mpd_output": "/var/www/live/sport1/output.mpd",   // optional
  "hls_master_playlist_output": "...",               // optional
  "hls_playlist_type": "LIVE",                       // when HLS
  "segment_duration": 2,
  "time_shift_buffer_depth": 30,                     // optional
  "encryption": {                                    // optional, raw-key
    "scheme": "cenc",                                // cenc | cbcs
    "keys": [{"label": "", "key_id": "<hex32>", "key": "<hex32>"}],
    "iv": "<hex16|hex32>",                           // optional
    "clear_lead": 0
  },
  "extra_args": ["--flag", "value"],                 // escape hatch, documented as unsupported surface
  "stop_timeout_seconds": 10
}
```

Validation errors return 400 with a JSON error body naming the field.

### EventManager

- Registry: `event_id -> EventRecord{state, pid, argv, metrics_port,
  log_fd/path, timestamps, exit_code}` under one mutex.
- Spawn: `posix_spawn`/fork+exec of `--packager_bin` with argv built from
  the DTO plus an allocated `--metrics_port` from the pool; stderr dup'd to
  the log file.
- Monitor: one thread per child (or a single waitpid loop) transitions
  RUNNING -> STOPPED/FAILED on exit and frees the metrics port.
- State machine: `STARTING -> RUNNING -> STOPPING -> STOPPED | FAILED(exit_code)`.
  STARTING covers spawn-to-first-successful-metrics-scrape (or 5s), so the
  create response can report early bind failures as FAILED with the log tail.
- Port pool exhaustion, unknown packager binary, and spawn failure are 503
  or 500 with an explanatory error body.
- API shutdown (SIGTERM) drains: stops all events (drain mode), then exits.

## Phase 2 — JIT origin loop (unitary ops, in-process)

All phase-2 ops share one harness: `OpsPipeline` builds a one-shot
`shaka::Packager` with `BufferCallbackParams` for input and/or output
buffers, runs it synchronously, and maps `Status` failures to HTTP 422
(media errors) or 500 (internal). Media bytes travel as request/response
bodies (`application/octet-stream` or multipart); path-based variants
accept `"input_path"` for content already on the origin's disk.

| Endpoint | In -> Out |
|---|---|
| `POST /api/v1/ops/package-segment` | source bytes/path + time range (`start_seconds`/`duration_seconds` on the source media timeline; actual boundaries snap to keyframes and are reported in the response headers) + output format (cmaf\|ts) -> one packaged media segment |
| `POST /api/v1/ops/init-segment` | source bytes/path (or stream config) + output format -> the init segment for that stream |
| `POST /api/v1/ops/transmux-segment` | segment + target container -> converted segment (TS<->CMAF) |
| `POST /api/v1/ops/encrypt-segment` | clear segment + `{scheme, key_id, key, iv}` -> encrypted segment (cenc\|cbcs) |
| `POST /api/v1/ops/decrypt-segment` | encrypted segment + key -> clear segment (raw-key DecryptionParams) |
| `POST /api/v1/ops/reencrypt-segment` | segment + old key + new key/scheme -> re-encrypted segment (decrypt+encrypt in one pipeline) |
| `POST /api/v1/ops/pssh` | `{drm_systems: [widevine, playready, common], key_ids: [...]}` -> PSSH box(es), base64 + hex (in-tree ProtectionSystemSpecificInfo code) |
| `POST /api/v1/ops/probe` | media bytes/path -> stream info JSON (container, codecs, resolution, duration, PIDs) |
| `POST /api/v1/ops/validate-segment` | segment -> `{valid, errors: [...]}` — TS sync/CC continuity (reusing the parse-health detection built for metrics) and MP4 box-structure checks |
| `POST /api/v1/ops/scte35` | TS segment -> JSON list of SCTE-35 splice events (reusing the fork's SCTE-35 parsing) |
| `GET/POST /api/v1/ops/manifest` | segment list + template params -> MPD or M3U8 text built on demand (no files touched) |

Concurrency: ops run on oatpp's worker threads; concurrent one-shot
Packager instances in one process are supported (no shared mutable state
beyond the metrics registry, whose counters are designed for concurrent
increment). A `--max_concurrent_ops` flag (default 2x cores) bounds them
with 429 on saturation.

## Phase 3 — Advanced ops

Same harness and endpoint conventions; each needs pipeline work beyond
configuration:

| Endpoint | In -> Out | The work |
|---|---|---|
| `POST /api/v1/ops/retimestamp-segment` | segment + offset_ms -> shifted segment | generalize the TS timestamp-offset machinery to a per-op parameter |
| `POST /api/v1/ops/trickplay-segment` | segment + factor -> reduced-rate trick segment | drive the in-tree trick_play handler in a one-shot graph |
| `POST /api/v1/ops/keyframe-map` | segment -> JSON keyframe offsets/sizes/timestamps | surface OnKeyFrame data as a collected result |
| `POST /api/v1/ops/convert-text` | WebVTT<->TTML segment conversion | one-shot graph over the in-tree text handlers |
| `POST /api/v1/ops/rewindow-manifest` | MPD/M3U8 + new bounds -> rewritten manifest | manifest parse + window rewrite (catch-up/startover) |
| `GET /api/v1/ops/iframe-playlist` | segment list -> EXT-X-I-FRAMES-ONLY playlist | built from keyframe-map data (native; replaces VxPackager's FFprobe approach, no FFmpeg dependency) |

## Error model (all phases)

JSON error body everywhere: `{"error": {"code": "<machine_code>",
"message": "...", "detail": "..."}}`. 400 validation, 401 bad/missing token,
404 unknown event/op, 409 duplicate event id, 422 media processing failure
(with the shaka Status message as detail), 429 op saturation, 503 spawn/pool
exhaustion. OpenAPI documents every error shape.

## Testing

Phase 1:
- Unit: EventManager against a fake child script (state transitions, drain
  escalation to SIGKILL, kill mode, crash -> FAILED with exit code, port
  pool exhaustion, log capture); DTO -> argv golden tests; token auth.
- Integration: start packager-api; full lifecycle over HTTP with the
  replay tool feeding UDP (create -> RUNNING -> metrics proxy shows
  segments climbing -> logs tail non-empty -> drain stop -> STOPPED);
  swagger/doc parses as valid OpenAPI 3 JSON.

Phase 2/3:
- Per-op unit tests through OpsPipeline with bear-640x360 test media:
  package-segment output re-probed and validated; encrypt->decrypt
  round-trip restores byte-identical media samples; pssh output matches
  the in-tree pssh tooling for the same inputs; validate-segment flags a
  deliberately corrupted segment; scte35 extracts the known cues from the
  fork's SCTE-35 test asset.
- Integration: JIT flow test — probe -> init-segment -> package-segment ->
  encrypt-segment -> validate-segment, all over HTTP.
- Concurrency: N parallel package-segment requests complete correctly
  (thread-safety of the one-shot harness).

## Metrics

The API process exposes its own `/metrics` via the existing MetricsService
when `--metrics_port` is set: `shaka_api_requests_total{route,code}`,
`shaka_api_events_running`, `shaka_api_ops_inflight`, plus the aggregated
unitary-op pipeline counters that libpackager already maintains. Event
subprocess metrics remain per-event on their own ports (scrape directly or
via the proxy endpoint).

## Rollout

- Three sequential implementation plans (one per phase), each executed with
  the subagent-driven workflow; phase 1 merges before phase 2 starts.
- Docs: `docs/source/documentation/webapi.rst` (endpoint reference is the
  generated OpenAPI; the rst covers deployment, flags, security posture)
  plus a README in packager/webapi/ mirroring VxPackager's.
- Fork-only feature; upstream-merge friction is confined to
  packager/third_party registration and the new packager/webapi/ directory
  (no upstream file is modified except third_party/CMakeLists.txt).

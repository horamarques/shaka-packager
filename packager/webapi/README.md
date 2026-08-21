# Shaka Packager Web API

`packager-api` is a web service that provides HTTP/REST control over Shaka Packager events. It spawns and manages packager processes for packaging as a service.

## Building

```bash
cmake --build build -t packager-api
```

The executable is `build/packager/webapi/packager-api`.

## Quick Start

Start the API server (defaults to `127.0.0.1:8088`):

```bash
./build/packager/webapi/packager-api \
  --packager_bin ./build/packager/packager \
  --event_log_dir /tmp/packager-logs
```

### Creating a Packaging Event (UDP Live Example)

Create a UDP live event that packages an incoming multicast stream to DASH/HLS:

```bash
curl -X POST http://127.0.0.1:8088/api/v1/events \
  -H "Content-Type: application/json" \
  -d '{
    "event_id": "my-live-event",
    "streams": [
      {
        "input": "udp://127.0.0.1:5000?timeout=100000000",
        "stream": "video",
        "init_segment": "/tmp/video_init.mp4",
        "segment_template": "/tmp/video_$Number$.m4s"
      }
    ],
    "mpd_output": "/tmp/output.mpd",
    "segment_duration": 6,
    "stop_timeout_seconds": 10
  }'
```

Response (201 Created):

```json
{
  "event_id": "my-live-event",
  "state": "STARTING",
  "pid": 12345,
  "metrics_port": 19100,
  "log_path": "/tmp/packager-logs/my-live-event.log",
  "created_unix": 1692547200,
  "argv": [...]
}
```

### Checking Event Status

```bash
curl http://127.0.0.1:8088/api/v1/events/my-live-event
```

Wait for `state` to become `RUNNING`, then send data to the UDP port.

### Scraping Event Metrics (Prometheus)

Proxy one Prometheus metrics scrape from the event's packager process:

```bash
curl http://127.0.0.1:8088/api/v1/events/my-live-event/metrics
```

Returns plaintext Prometheus format (prefix `shaka_`). Example metrics:

- `shaka_udp_datagrams_received_total`
- `shaka_segments_emitted_total`
- `shaka_live_buffer_depth_seconds`

See [Metrics options](../../docs/source/options/metrics_options.rst) for the full list.

### Stopping an Event

Gracefully drain and stop (wait up to `stop_timeout_seconds`):

```bash
curl -X DELETE http://127.0.0.1:8088/api/v1/events/my-live-event
```

Kill immediately:

```bash
curl -X DELETE "http://127.0.0.1:8088/api/v1/events/my-live-event?mode=kill"
```

Response (202 Accepted) includes the event's `state: STOPPING` or `STOPPED` with exit code.

## Endpoints

| Method | Path | Summary |
|--------|------|---------|
| `GET` | `/health` | Liveness check and packager version |
| `GET` | `/swagger/ui` | OpenAPI UI (interactive docs) |
| `POST` | `/api/v1/events` | Create and start a packaging event |
| `GET` | `/api/v1/events` | List all events |
| `GET` | `/api/v1/events/{event_id}` | Get one event's status |
| `DELETE` | `/api/v1/events/{event_id}` | Stop an event (drain or kill) |
| `GET` | `/api/v1/events/{event_id}/logs` | Tail the event's stderr log |
| `GET` | `/api/v1/events/{event_id}/metrics` | Proxy event metrics endpoint |

The OpenAPI document is available at `/swagger/doc` (returns 302 redirect to `/api-docs/oas-3.0.0.json`).

## Flags

| Flag | Type | Default | Description |
|------|------|---------|-------------|
| `--api_port` | int32 | 8088 | HTTP port for the web API |
| `--api_bind_address` | string | `0.0.0.0` | Bind address (use `127.0.0.1` for local-only) |
| `--api_token` | string | `` | Static bearer token for `/api/v1/*` endpoints (see Security below) |
| `--metrics_port` | int32 | 0 | Prometheus endpoint port for the API process itself (0 disables) |
| `--packager_bin` | string | packager next to packager-api | Path to the packager binary spawned per event |
| `--event_metrics_port_range` | string | `19100-19199` | Inclusive port range allocated to per-event Prometheus endpoints |
| `--event_log_dir` | string | `/tmp` | Directory for per-event stderr log files |

## Security

**By default, the API is unauthenticated.** The following endpoints are always open (not gated by `--api_token`):

- `GET /health`
- `GET /swagger/doc` and `GET /swagger/ui`

All other endpoints (`/api/v1/*`) require authentication when `--api_token` is set.

**Authentication:** Pass `Authorization: Bearer <token>` header on every request to `/api/v1/*` endpoints. For example:

```bash
curl -H "Authorization: Bearer my-secret-token" \
  http://127.0.0.1:8088/api/v1/events
```

**Deployment recommendations:**

- Bind to a management interface or `127.0.0.1` behind a proxy in production.
- For public exposure, front the API with a reverse proxy (nginx, Envoy, etc.) that enforces TLS, rate limiting, and access control.
- The `/swagger/ui` and `/swagger/doc` endpoints are the authoritative living reference; use them for endpoint discovery and schema validation.

## Metrics (API Process)

When `--metrics_port` is set, the API process exposes Prometheus metrics on `/metrics`:

| Metric | Labels | Description |
|--------|--------|-------------|
| `shaka_api_requests_total` | `route`, `code` | Total requests by endpoint and HTTP status code |
| `shaka_api_events_running` | | Events in STARTING or RUNNING state |

Example:

```bash
curl http://127.0.0.1:9000/metrics  # if --metrics_port 9000
```

Returns:

```
# HELP shaka_api_requests_total API requests by route and status code.
# TYPE shaka_api_requests_total counter
shaka_api_requests_total{code="201",route="create_event"} 1
shaka_api_requests_total{code="200",route="list_events"} 2
# HELP shaka_api_events_running Events currently in STARTING or RUNNING state.
# TYPE shaka_api_events_running gauge
shaka_api_events_running 1
```

## Event Create Request Body

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `event_id` | string | no | auto-generated | Unique event identifier (up to 64 chars, alphanumeric and hyphens) |
| `streams` | array | yes | | Stream specifications (each has `input`, `stream`, `init_segment`, `segment_template`, `output`) |
| `mpd_output` | string | no | | DASH MPD manifest output path |
| `hls_master_playlist_output` | string | no | | HLS master playlist output path |
| `hls_playlist_type` | string | no | | `LIVE` or `EVENT` (HLS playlist type) |
| `segment_duration` | int32 | no | 6 | Segment duration in seconds |
| `time_shift_buffer_depth` | int32 | no | 0 | Time-shift buffer depth in seconds (DASH) |
| `encryption` | object | no | | Encryption config (`scheme`, `keys`, `iv`, `clear_lead`) |
| `extra_args` | array | no | | Extra command-line arguments passed to packager (e.g., `--hls_playlist_type LIVE`) |
| `stop_timeout_seconds` | int32 | no | 10 | Timeout in seconds for graceful drain on stop |

## Event States

- `STARTING`: Process spawned, awaiting packager readiness
- `RUNNING`: Packager process active and packaging
- `STOPPING`: Drain or kill in progress
- `STOPPED`: Packager exited (check `exit_code`)
- `FAILED`: Spawn or configuration error

## Errors

API errors (4xx, 5xx) return JSON with a structured error body:

```json
{
  "error": {
    "code": "invalid_request",
    "message": "streams required",
    "detail": "field validation failed"
  }
}
```

Error codes:

| Code | HTTP | Description |
|------|------|-------------|
| `invalid_request` | 400 | Request body validation failed (missing or malformed fields) |
| `unauthorized` | 401 | Missing or invalid `Authorization: Bearer` token (when `--api_token` is set) |
| `not_found` | 404 | Event does not exist |
| `duplicate_event` | 409 | Event ID already exists |
| `resource_exhausted` | 503 | Metrics port range exhausted or other resource limit |
| `internal` | 500 | Packager spawn error or other internal failure |

Always check the `code` field to handle errors programmatically; the `message` and `detail` fields are human-readable and may change.

## Logs and Debugging

Retrieve the event's packager stderr:

```bash
curl "http://127.0.0.1:8088/api/v1/events/my-live-event/logs?tail=100"
```

The `tail` query parameter (default 100) limits the number of lines returned. Logs are stored in `--event_log_dir` with filename `{event_id}.log`.

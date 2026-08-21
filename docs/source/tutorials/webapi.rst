Web API (packager-api)
======================

`packager-api` is a web service that provides HTTP/REST control over Shaka Packager. It manages packaging events as a service, spawning and controlling packager processes via a clean JSON API.

Quick Start
-----------

Start the service::

    packager-api \
      --packager_bin ./packager \
      --event_log_dir /tmp/packager-logs

Then create a packaging event::

    curl -X POST http://127.0.0.1:8088/api/v1/events \
      -H "Content-Type: application/json" \
      -d '{
        "event_id": "my-live",
        "streams": [{
          "input": "udp://127.0.0.1:5000?timeout=100000000",
          "stream": "video",
          "init_segment": "/tmp/video_init.mp4",
          "segment_template": "/tmp/video_$Number$.m4s"
        }],
        "mpd_output": "/tmp/output.mpd",
        "segment_duration": 6
      }'

The response includes the event's status::

    {
      "event_id": "my-live",
      "state": "STARTING",
      "pid": 12345,
      "metrics_port": 19100,
      "log_path": "/tmp/packager-logs/my-live.log",
      "created_unix": 1692547200
    }

Check status and wait for ``RUNNING``, then send data to the UDP port. Stop with::

    curl -X DELETE http://127.0.0.1:8088/api/v1/events/my-live

Endpoints and Methods
---------------------

The API exposes these endpoints (examples assume ``http://127.0.0.1:8088``):

* ``GET /health`` — Liveness check and packager version (always open)
* ``GET /swagger/ui`` — Interactive OpenAPI documentation (always open)
* ``GET /swagger/doc`` — OpenAPI schema redirect (always open)
* ``POST /api/v1/events`` — Create and start an event
* ``GET /api/v1/events`` — List all events
* ``GET /api/v1/events/{event_id}`` — Get event status
* ``DELETE /api/v1/events/{event_id}`` — Stop an event (graceful drain by default; `?mode=kill` for immediate)
* ``GET /api/v1/events/{event_id}/logs`` — Tail the event's stderr log (`?tail=N` to limit lines)
* ``GET /api/v1/events/{event_id}/metrics`` — Proxy the event's Prometheus metrics endpoint

Configuration Flags
-------------------

``--api_port <port>``

    HTTP port for the web API. Default: 8088.

``--api_bind_address <address>``

    Bind address. Default: ``0.0.0.0``. Use ``127.0.0.1`` to listen only locally.

``--api_token <token>``

    Static bearer token for ``/api/v1/*`` endpoints. When set, all API routes
    (except ``/health``, ``/swagger/*``) require ``Authorization: Bearer <token>``.
    Default: empty (no authentication).

``--metrics_port <port>``

    Prometheus endpoint port for the API process itself (``/metrics``).
    0 (default) disables. See :ref:`api-metrics` below.

``--packager_bin <path>``

    Path to the packager binary spawned per event. If empty, defaults to
    ``packager`` in the same directory as ``packager-api``.

``--event_metrics_port_range <min>-<max>``

    Inclusive port range allocated to per-event Prometheus endpoints
    (one port per packager process). Default: 19100-19199.

``--event_log_dir <path>``

    Directory for per-event stderr log files. Default: /tmp.

Event Creation
--------------

Send a JSON POST to ``/api/v1/events`` with fields from ``EventCreateDto``:

* ``event_id`` (string, optional) — Unique identifier; auto-generated if omitted
* ``streams`` (array, required) — Stream specs, each with ``input``, ``stream``, ``init_segment``, ``segment_template``, ``output``
* ``mpd_output`` (string, optional) — DASH manifest output path
* ``hls_master_playlist_output`` (string, optional) — HLS master playlist path
* ``hls_playlist_type`` (string, optional) — ``LIVE`` or ``EVENT``
* ``segment_duration`` (int32, default: 6) — Segment duration in seconds
* ``time_shift_buffer_depth`` (int32, default: 0) — DVR buffer depth (DASH, seconds)
* ``encryption`` (object, optional) — Encryption config (``scheme``, ``keys``, ``iv``, ``clear_lead``)
* ``extra_args`` (array, optional) — Additional packager command-line flags
* ``stop_timeout_seconds`` (int32, default: 10) — Graceful drain timeout on stop

Example — UDP live stream to DASH::

    {
      "event_id": "sports-live",
      "streams": [
        {
          "input": "udp://239.1.1.1:5000?interface=192.168.1.10&timeout=100000000",
          "stream": "video",
          "init_segment": "/output/video_init.mp4",
          "segment_template": "/output/video_$Number$.m4s"
        },
        {
          "input": "udp://239.1.1.2:5001?interface=192.168.1.10&timeout=100000000",
          "stream": "audio",
          "init_segment": "/output/audio_init.mp4",
          "segment_template": "/output/audio_$Number$.m4s"
        }
      ],
      "mpd_output": "/output/manifest.mpd",
      "hls_master_playlist_output": "/output/master.m3u8",
      "hls_playlist_type": "LIVE",
      "segment_duration": 6,
      "extra_args": ["--hls_playlist_type", "LIVE"]
    }

Event Lifecycle
---------------

Events transition through states:

* ``STARTING`` — Process spawned; packager initializing
* ``RUNNING`` — Packager active and ready for input
* ``STOPPING`` — Drain or kill in progress
* ``STOPPED`` — Packager exited (check ``exit_code``)
* ``FAILED`` — Spawn or configuration error; check ``log_path``

Query status with ``GET /api/v1/events/{event_id}`` to monitor transitions.

Terminal events (``STOPPED``/``FAILED``) are retained up to a cap (default 100)
and then evicted oldest-first.

Errors
------

API errors (4xx, 5xx) return JSON with a structured error body::

    {
      "error": {
        "code": "invalid_request",
        "message": "streams required",
        "detail": "field validation failed"
      }
    }

Error codes:

* ``invalid_request`` (400) — Request body validation failed (missing or malformed fields)
* ``unauthorized`` (401) — Missing or invalid ``Authorization: Bearer`` token (when ``--api_token`` is set)
* ``not_found`` (404) — Event does not exist
* ``duplicate_event`` (409) — Event ID already exists
* ``resource_exhausted`` (503) — Metrics port range exhausted or other resource limit
* ``internal`` (500) — Packager spawn error or other internal failure

Always check the ``code`` field to handle errors programmatically; the ``message`` and ``detail`` fields are human-readable and may change.

Metrics and Monitoring
----------------------

.. _api-metrics:

**API process metrics** (if ``--metrics_port`` is set)::

    curl http://127.0.0.1:9000/metrics

Returns Prometheus text format:

* ``shaka_api_requests_total{route,code}`` — Requests by endpoint and HTTP status
* ``shaka_api_events_running`` — Count of events in STARTING or RUNNING state

**Event packager metrics** (per-event Prometheus endpoint)::

    curl http://127.0.0.1:8088/api/v1/events/sports-live/metrics

This proxies one scrape of the packager process's ``/metrics`` endpoint, including
all standard Shaka Packager metrics (UDP input health, segment output, live buffer
depth, manifest writes, etc.). See :doc:`/options/metrics_options` for details.

Security
--------

**Authentication**

By default, the API is unauthenticated. Endpoints ``/health``, ``/swagger/*`` are
always open.

To require a bearer token, set ``--api_token``::

    packager-api --api_token "my-secret-key"

Then all ``/api/v1/*`` requests must include::

    Authorization: Bearer my-secret-key

**Deployment**

* In closed networks (e.g., Kubernetes internal), binding to ``0.0.0.0`` is safe.
* For internet exposure, bind to ``127.0.0.1`` and front with a reverse proxy
  (nginx, Envoy, etc.) that enforces TLS, rate limiting, and authorization.
* Enable the API metrics endpoint (``--metrics_port``) only on a management
  interface, not on the same port as the API itself.

The OpenAPI document at ``/swagger/doc`` and the interactive UI at ``/swagger/ui``
are the authoritative endpoint reference; use them for endpoint discovery, parameter
validation, and request/response examples.

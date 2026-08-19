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
  ``shaka_ts_tei_packets_total``, ``shaka_ts_unsupported_streams_total``
  (elementary streams ignored as unsupported at PMT registration) and
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

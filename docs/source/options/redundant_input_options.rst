Redundant UDP input options
^^^^^^^^^^^^^^^^^^^^^^^^^^^

Syntax::

    redundant://<udp-url>|<udp-url>[&<param>=<value>]...

Each ``<udp-url>`` is a complete ``udp://<ip>:<port>[?<udp-options>]`` (see
UDP file options above). Legs are separated by ``|``; global parameters are
appended after the last leg with ``&``. Legs without an explicit ``timeout``
UDP option receive a 100 ms receive timeout automatically.

:mode:

    ``merge`` (default) — first-arrival dedup across all legs; a leg dying is
    hitless. Requires legs to carry the *same multiplex* (bit-identical TS
    packets, SMPTE 2022-7 style).

    ``failover`` — emit only the active leg; switch to the lowest-index
    healthy leg when the active leg is silent longer than
    ``failover_timeout_ms``. No automatic switch-back.

:dedup_window_ms:

    Merge-mode dedup window in milliseconds (default 200). Must exceed the
    worst-case arrival skew between legs.

:dedup_window_pkts:

    Merge-mode dedup window bound in packets (default 4096). The window is
    bounded by both limits.

:failover_timeout_ms:

    Silence threshold in milliseconds after which a leg is considered
    unhealthy (default 200).

A per-minute ``redundant_input:`` summary is logged with per-leg packet,
duplicate-drop, resync and continuity-counter-error counts plus global
switch count, maximum observed leg skew and dedup-window evictions. A
``max_skew_ms`` approaching ``dedup_window_ms`` or a growing
``window_evictions`` value means the window is undersized.

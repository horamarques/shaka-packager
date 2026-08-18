# Redundant MPEG-TS Input — Feature Design

**Date:** 2026-08-18
**Plan:** ../plans/2026-08-18-redundant-ts-input.md
**Scope:** Same-mux dual-path UDP input (SMPTE 2022-7 style). Not in scope:
independent-encoder failover (requires IDR-aligned splicing; belongs upstream
of the packager or at origin failover).

## Feature summary

A new input scheme lets one packager instance consume a live TS channel from
two (or more) UDP legs carrying the *same multiplex* over diverse network
paths, producing an uninterrupted — ideally hitless — byte stream for the
demuxer. All redundancy logic lives at the `File` layer; nothing downstream
changes.

## URL grammar

```
redundant://<leg>[|<leg>]...[&<param>=<value>]...

<leg>   := udp://<host>:<port>[?<udp-options>]     (existing UdpOptions syntax)
params  := mode=merge|failover        (default: merge)
           dedup_window_ms=<int>      (default: 200)
           dedup_window_pkts=<int>    (default: 4096)
           failover_timeout_ms=<int>  (default: 200)
```

Example:

```
input=redundant://udp://239.1.1.1:5000?interface=10.0.0.1|udp://239.2.2.2:5000?interface=10.0.1.1&mode=merge&dedup_window_ms=200
```

- Legs separated by `|` (illegal inside our UDP URLs, so unambiguous).
- Global params appended after the last leg with `&`.
- 2 legs minimum for redundancy, N legs supported by construction.

## Components

```
packager/file/redundant_input_merger.{h,cc}   pure logic, no sockets
packager/file/redundant_input_merger_unittest.cc
packager/file/redundant_udp_file.{h,cc}       File subclass, threads+sockets
```

### RedundantInputMerger (task 2 — socket-free core)

Inputs: per-leg `OnBytes(leg_index, data, size, arrival_time)` calls.
Output: ordered TS packets via an emit callback.

Responsibilities:
1. **Framing** — accumulate bytes per leg, cut into 188-byte packets, resync
   by scanning for 0x47 with 188-stride confirmation (2 consecutive syncs)
   after any misalignment.
2. **Dedup merge (mode=merge)** — first-arrival wins. Key: 64-bit hash of the
   full 188-byte packet. Window bounded by `dedup_window_ms` (stream arrival
   time) AND `dedup_window_pkts`; oldest entries evicted. Packets whose key is
   in the window are dropped; new keys are emitted immediately and recorded.
3. **Failover (mode=failover)** — emit only from the active leg. Standby legs
   are framed and health-tracked but not emitted; last ~100 ms of emitted
   packet hashes kept in a ring. On switchover, standby packets already in the
   ring are skipped; emission resumes at the first unseen packet.
4. **Health tracking (both modes; drives failover and observability)** —
   per leg: last-arrival age, sync-loss events, per-PID CC-error rate over a
   sliding window. A leg is UNHEALTHY when silence > `failover_timeout_ms`,
   or sync cannot be re-established, or CC error rate exceeds threshold.

Health state machine per leg:

```
        packets flowing               silence > timeout / sync lost
HEALTHY ────────────────▶ HEALTHY   ────────────────────────────▶ UNHEALTHY
   ▲                                                                 │
   └──────────── first well-framed packet after resync ◀─────────────┘
```

Active-leg selection (failover mode): prefer lowest-index HEALTHY leg;
switch only when the active leg goes UNHEALTHY (no flap-back on recovery —
recovered leg becomes standby), eliminating oscillation.

### RedundantUdpFile (task 3)

- Owns N `UdpFile` legs; one reader thread per leg (pattern: ThreadedIoFile)
  so every multicast membership stays joined and kernel buffers drain.
- Reader threads push into the merger with arrival timestamps; merger emit
  callback writes to an `IoCache`; `Read()` blocks on the cache.
- `Open()` succeeds if at least one leg opens; failed legs are retried in the
  background every 5 s.
- EOF semantics: live UDP has no EOF; `Read()` returns 0 only on `Close()`.

## Edge cases (specified behavior)

| Case | Behavior |
|------|----------|
| Null packets (PID 0x1FFF) byte-identical across time | May be deduped inside the window — acceptable (stuffing). |
| Legitimate identical packet recurrence | Requires same payload AND same CC (mod 16); impossible within ≤200 ms for media PIDs; PSI repeats (~100 ms cadence × 16 CC cycle ≥ 1.6 s) fall outside the window. |
| Leg latency skew > dedup window | Late leg contributes nothing (all dupes are misses → double emission is prevented by window sizing; operator alarm via skew counter). |
| Both legs die | `Read()` blocks; health counters expose the outage; demuxer behavior unchanged from today's single dead UDP input. |
| Mid-packet byte loss on a leg | Framing resync (0x47 stride scan); lost packets on ONE leg are healed by the other leg in merge mode. |
| RTP-encapsulated legs (true 2022-7) | Future: framing layer strips RTP and keys on sequence numbers; merger unchanged. |

## Prerequisite parser hardening (task 1)

`PidState::PushTsPacket` (packager/media/formats/mp2t/mp2t_media_parser.cc)
returns `false` on a CC mismatch, which propagates as a fatal parse error —
one lost packet kills a live pipeline. New behavior: log at WARNING with the
PID and CC values, reset that PID's section parser (dropping the in-flight
PES packet/section so no corrupt frame is emitted), and return `true`.
No flag: the old fail-fast behavior protects nothing in live use and file
inputs are unaffected (no loss). Unit test: a TS with an induced packet gap
still parses; frames on unaffected PIDs are complete; the damaged access unit
is dropped, subsequent access units on the damaged PID are complete.

## Observability (task 4)

Per-leg counters, logged as one INFO line per minute and queryable in tests:

```
redundant_input: leg=0 pkts=1234567 dropped_dup=61728 resyncs=0 cc_errors=0 state=HEALTHY active=yes
redundant_input: leg=1 pkts=1234102 dropped_dup=1172839 resyncs=2 cc_errors=14 state=HEALTHY active=no
redundant_input: switches=0 skew_ms_p50=12 skew_ms_max=45 window_evictions=0
```

## Test matrix (tasks 2 & 5)

| Test | Mode | Assertion |
|------|------|-----------|
| Identical legs | merge | Output == single-leg reference (byte-exact) |
| Skewed legs (±150 ms) | merge | Byte-exact; skew counters populated |
| Kill leg A mid-stream | merge | Byte-exact, zero gap |
| 1% random loss on each leg (disjoint) | merge | Byte-exact (each leg heals the other) |
| Loss on BOTH legs (same packet) | merge | Gap passes through; parser survives (task 1) |
| Kill active leg | failover | ≤ failover_timeout_ms loss; switch counter=1; no flap |
| Leg flap (down 1 s, up, down) | failover | Exactly 1 switch (no flap-back) |
| Mid-packet resync | both | Framing recovers within 2 packets |
| Soak 1 h with periodic kills | merge | Hash(output) == hash(reference) |

## Rollout

Feature is opt-in via the `redundant://` scheme; zero impact on existing
inputs. Ship order: task 1 (standalone robustness win) → 2 → 3 → 4+5.

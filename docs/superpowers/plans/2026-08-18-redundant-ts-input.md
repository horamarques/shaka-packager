# Redundant MPEG-TS Input for Live TV Channels

**Date:** 2026-08-18
**Scope:** Scenario A — same mux delivered over two network paths (SMPTE 2022-7
style dual multicast legs). Independent-encoder redundancy (scenario B) is out
of scope: it requires IDR-aligned splicing and belongs upstream of the packager
or at origin failover.

## Goal

`packager` consumes a live TS channel from two UDP legs and produces an
uninterrupted output when either leg dies, degrades, or drops packets —
ideally hitless (no discontinuity at all).

## Design summary

Implement redundancy entirely at the `File` layer as a new scheme, so the
Demuxer, Mp2tMediaParser, SCTE-35, and LL-HLS paths are untouched:

```
input=redundant://udp://239.1.1.1:5000?interface=10.0.0.1|udp://239.2.2.2:5000?interface=10.0.1.1&failover_timeout_ms=200
```

- `GetFileTypePrefix` splits on the first `://` → `redundant://` registers in
  `kFileTypeInfo` (packager/file/file.cc) like the other schemes.
- Legs are separated by `|` (not a legal character in the UDP URLs we accept).
- Trailing query params after the last leg configure the merger.

### Architecture

```
UdpFile(leg A) ── reader thread A ──▶ ┌────────────┐
                                      │   Merger    │──▶ IoCache ──▶ Read()
UdpFile(leg B) ── reader thread B ──▶ └────────────┘         (Demuxer pulls)
```

- One reader thread per leg (pattern: ThreadedIoFile), each draining its
  socket continuously so both multicast memberships stay active and kernel
  buffers never back up.
- Readers chunk input into 188-byte TS packets (resync on 0x47 when needed)
  and stamp arrival time.
- The merger emits a single ordered packet stream into an `IoCache`
  (packager/file/io_cache.h — existing blocking ring buffer); `Read()` simply
  drains the cache.

### Mode 1 — always-on dedup merge (hitless; the target mode)

Both legs feed the merger at all times. First arrival wins:

- Dedup key: hash of the full 188-byte packet (header included — CC makes
  consecutive packets distinct).
- Bounded dedup window (default 200 ms of stream time or 4096 packets,
  whichever is smaller) absorbs differential path latency.
- A leg dying is a non-event: its packets simply stop contributing.

Edge cases (documented behavior):
- Null packets (PID 0x1FFF) may be byte-identical across time and can be
  swallowed by the window — acceptable, they are stuffing.
- Byte-identical recurrence of real packets requires the same payload AND the
  same CC value (CC cycles mod 16 per PID); with a ≤200 ms window this cannot
  occur for media PIDs and PSI repeats (PAT/PMT ~100 ms cadence, CC cycle
  ≥1.6 s) stay outside the window.

### Mode 2 — active/standby failover (fallback when legs are not co-timed)

If operators cannot guarantee bit-identical legs, `mode=failover`:

- Emit only from the active leg; track standby health.
- Health per leg: last-packet arrival age (silence timeout, default 200 ms),
  0x47 sync integrity, per-PID CC error rate.
- On failure: switch to standby, resync to the next sync byte, and use a ring
  of recently emitted packet hashes (~100 ms) to skip already-emitted packets.
- A switch after real packet loss can still produce a CC jump — which is why
  the parser hardening step below is required regardless of mode.

### Required parser hardening (prerequisite)

`PidState::PushTsPacket` (packager/media/formats/mp2t/mp2t_media_parser.cc)
currently treats a continuity-counter mismatch as a fatal parse error
(`return false`). For live inputs this turns any single lost packet into a
dead pipeline. Change to: log, reset the section parser for that PID (drop the
in-flight PES/section), and continue. Gate the old fatal behavior behind a
flag if upstream-compatibility is a concern. This fixes a real live-TV
robustness gap even without redundancy.

## Implementation steps

1. **Parser hardening** — make TS CC discontinuities non-fatal (drop and
   resync the affected PID). Unit test: truncated/lossy TS still yields all
   complete frames after the gap. (~½ day)
2. **`RedundantInputSource` abstraction + merger core** — pure logic, no
   sockets: packet framing, dedup window, health state machine, failover.
   Unit-test with injected fake legs (vectors of packets with timestamps),
   covering: identical legs, skewed legs (±150 ms), leg death, leg flap,
   packet loss on one leg, loss on both, resync mid-packet. (~2 days)
3. **`RedundantUdpFile`** — File subclass wiring N `UdpFile` legs + reader
   threads + IoCache to the merger; URL parsing (`|` legs, query params);
   factory registration. (~1 day)
4. **Flags & docs** — merger params via URL query only (no global flags
   needed); document the scheme in docs/source/options and a live-TV tutorial
   section with a 2022-7-style example. (~½ day)
5. **Observability** — per-leg counters (packets, dedup drops, switches,
   resyncs), one INFO summary line per minute + VLOG detail; counters exposed
   for tests. (~½ day)
6. **Integration + soak tooling** — `packager/tools/redundant_ts/`: script
   that replays a TS file onto two local UDP legs with configurable loss/
   latency/kill, plus a packager_test.py case using two loopback senders
   (skipped on CI if sockets unavailable). Soak: 1 h run with periodic leg
   kills; assert zero output gaps in mode 1. (~1 day)

Total: ~5–6 days including tests.

## Acceptance criteria

- Mode 1: killing either leg at any moment produces byte-identical output to
  a single healthy leg (verified by hash over a soak run).
- Mode 2: leg kill produces ≤ failover_timeout_ms of loss, pipeline survives,
  LL-HLS output resumes within one partial segment.
- No changes required in Demuxer/parser call sites (File layer only), except
  the CC-hardening in step 1.
- All existing suites remain green.

## Risks / notes

- Differential leg latency beyond the dedup window degrades mode 1 to
  effective single-leg; window is configurable (`dedup_window_ms`).
- RTP-encapsulated inputs (true SMPTE 2022-7) are a future extension: the
  merger keys on RTP sequence numbers instead of packet hashes; the
  architecture above already isolates that in the framing layer.
- The packager process itself remains a single point of failure; this feature
  addresses input-network redundancy only. Process-level redundancy stays an
  origin/CDN concern.

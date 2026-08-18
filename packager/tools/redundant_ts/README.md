# Redundant TS input test tooling

`replay_ts.py` replays an MPEG-TS file onto one or more local UDP legs to
exercise the `redundant://` input scheme (see the live-streaming tutorial).

## Quick hitless-merge check

Terminal 1 — packager consuming both legs:

```sh
packager \
  'input=redundant://udp://127.0.0.1:5001|udp://127.0.0.1:5002,stream=video,init_segment=out/init.mp4,segment_template=out/v_$Number$.m4s,playlist_name=v.m3u8' \
  --hls_master_playlist_output out/master.m3u8 --hls_playlist_type LIVE \
  --segment_duration 1
```

Terminal 2 — feed both legs and kill leg 0 halfway:

```sh
python3 replay_ts.py bear-640x360.ts --ports 5001 5002 --kill 0:0.5
```

Expected: full-duration output, no `TS discontinuity` warnings — the merge is
hitless.

## Soak run (acceptance: byte-exact against a reference)

1. Reference: package the same file from local input:
   `packager 'input=bear.ts,stream=video,...'` and hash the concatenated
   media segments.
2. Soak: loop the file for an hour with alternating 5-second leg kills every
   30 seconds:

   ```sh
   python3 replay_ts.py bear.ts --ports 5001 5002 --duration 3600 --kill-every 30
   ```

3. Hash the soak run's segments over one file pass and compare with the
   reference. In merge mode with at least one healthy leg at all times the
   hashes must match; the per-minute `redundant_input:` log lines report
   per-leg drops, switches, max skew and window evictions.

Loss/skew simulation: `--loss 0.01` (independent per-leg datagram loss),
`--skew-ms 30` (delay on legs after the first). Disjoint per-leg loss is
healed by the merge; only simultaneous loss of the same packet on all legs
produces a gap, which the TS parser now survives (dropped access unit,
resync at the next unit start).

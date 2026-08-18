#!/usr/bin/env python3
# Copyright 2026 Google LLC. All rights reserved.
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file or at
# https://developers.google.com/open-source/licenses/bsd
"""Replays an MPEG-TS file onto one or more UDP legs for testing the
redundant:// input scheme.

Examples:
  # Dual healthy legs, 7 TS packets per datagram, ~1000 datagrams/s:
  replay_ts.py bear.ts --ports 5001 5002

  # Kill leg 0 halfway through (hitless-merge test):
  replay_ts.py bear.ts --ports 5001 5002 --kill 0:0.5

  # 1%% independent random loss per leg, leg 1 delayed 30ms (skew):
  replay_ts.py bear.ts --ports 5001 5002 --loss 0.01 --skew-ms 30

  # Soak: loop the file for an hour, killing alternating legs every 30s:
  replay_ts.py bear.ts --ports 5001 5002 --duration 3600 --kill-every 30
"""

import argparse
import random
import socket
import sys
import time

TS_PACKET_SIZE = 188
PACKETS_PER_DATAGRAM = 7  # 1316 bytes, the conventional TS-over-UDP payload.


def parse_args():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('ts_file')
  parser.add_argument('--host', default='127.0.0.1')
  parser.add_argument('--ports', nargs='+', type=int, required=True,
                      help='One port per leg.')
  parser.add_argument('--pps', type=float, default=1000.0,
                      help='Datagrams per second per leg.')
  parser.add_argument('--loss', type=float, default=0.0,
                      help='Independent per-leg datagram loss probability.')
  parser.add_argument('--skew-ms', type=float, default=0.0,
                      help='Extra delay applied to every leg after the '
                           'first (crude path-skew simulation).')
  parser.add_argument('--kill', default=None,
                      help='LEG:FRACTION - stop sending to LEG after this '
                           'fraction of the stream (e.g. 0:0.5).')
  parser.add_argument('--duration', type=float, default=0.0,
                      help='Loop the file for this many seconds (soak).')
  parser.add_argument('--kill-every', type=float, default=0.0,
                      help='In soak mode, kill alternating legs for 5s '
                           'every N seconds.')
  parser.add_argument('--seed', type=int, default=1)
  return parser.parse_args()


def main():
  args = parse_args()
  rng = random.Random(args.seed)
  data = open(args.ts_file, 'rb').read()
  if len(data) % TS_PACKET_SIZE:
    sys.exit('input is not 188-byte aligned')
  datagrams = [data[i:i + TS_PACKET_SIZE * PACKETS_PER_DATAGRAM]
               for i in range(0, len(data),
                              TS_PACKET_SIZE * PACKETS_PER_DATAGRAM)]
  sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
  legs = [(args.host, p) for p in args.ports]

  kill_leg, kill_at = -1, 2.0
  if args.kill:
    kill_leg, kill_at = args.kill.split(':')
    kill_leg, kill_at = int(kill_leg), float(kill_at)

  interval = 1.0 / args.pps
  start = time.monotonic()
  passes = 0
  while True:
    for i, dgram in enumerate(datagrams):
      now = time.monotonic()
      frac = i / len(datagrams)
      for leg, addr in enumerate(legs):
        if leg == kill_leg and frac >= kill_at:
          continue  # This leg is dead.
        if args.kill_every > 0:
          # Soak mode: kill alternating legs for 5s windows.
          cycle = int((now - start) / args.kill_every)
          in_window = (now - start) % args.kill_every < 5.0
          if in_window and cycle % len(legs) == leg:
            continue
        if args.loss > 0 and rng.random() < args.loss:
          continue
        if leg > 0 and args.skew_ms > 0:
          time.sleep(args.skew_ms / 1000.0)
        sock.sendto(dgram, addr)
      time.sleep(interval)
    passes += 1
    if args.duration <= 0 or time.monotonic() - start >= args.duration:
      break
  print('sent %d pass(es) of %d datagrams to %d leg(s)' %
        (passes, len(datagrams), len(legs)))


if __name__ == '__main__':
  main()

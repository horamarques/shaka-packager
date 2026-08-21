#!/usr/bin/env python3
# Copyright 2026 Google LLC. All rights reserved.
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file or at
# https://developers.google.com/open-source/licenses/bsd
"""End-to-end tests for packager-api (phase 1: event control)."""

import json
import os
import socket
import subprocess
import tempfile
import time
import unittest
import urllib.error
import urllib.request

API_BIN = os.environ['PACKAGER_API_BIN']
PACKAGER_BIN = os.environ['PACKAGER_BIN']
SRC_DIR = os.environ['PACKAGER_SRC_DIR']


def free_port(kind):
  probe = socket.socket(socket.AF_INET, kind)
  probe.bind(('127.0.0.1', 0))
  port = probe.getsockname()[1]
  probe.close()
  return port


def http(method, url, body=None, token=None):
  request = urllib.request.Request(url, method=method)
  if token:
    request.add_header('Authorization', 'Bearer ' + token)
  data = None
  if body is not None:
    request.add_header('Content-Type', 'application/json')
    data = json.dumps(body).encode('utf8')
  try:
    with urllib.request.urlopen(request, data=data, timeout=10) as response:
      return response.status, response.read().decode('utf8')
  except urllib.error.HTTPError as error:
    return error.code, error.read().decode('utf8')


class WebApiTest(unittest.TestCase):

  def setUp(self):
    self.tmp_dir = tempfile.mkdtemp()
    self.api_port = free_port(socket.SOCK_STREAM)
    self.base = 'http://127.0.0.1:%d' % self.api_port
    self.api = subprocess.Popen([
        API_BIN,
        '--api_port', str(self.api_port),
        '--api_bind_address', '127.0.0.1',
        '--packager_bin', PACKAGER_BIN,
        '--event_log_dir', self.tmp_dir,
        '--event_metrics_port_range', '20500-20599',
    ], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    # Wait for the server to accept connections.
    for _ in range(50):
      try:
        status, _ = http('GET', self.base + '/health')
        if status == 200:
          return
      except (ConnectionError, urllib.error.URLError):
        time.sleep(0.2)
    self.fail('packager-api did not come up; stderr:\n%s'
              % self.api.stderr.peek().decode('utf8', 'replace'))

  def tearDown(self):
    self.api.terminate()
    self.api.communicate(timeout=30)

  def wait_for_state(self, event_id, state, deadline=15):
    last = None
    for _ in range(int(deadline / 0.5)):
      status, body = http('GET', self.base + '/api/v1/events/' + event_id)
      self.assertEqual(200, status, body)
      last = json.loads(body)
      if last['state'] == state:
        return last
      time.sleep(0.5)
    self.fail('event %s never reached %s; last: %r' % (event_id, state, last))

  def testHealthAndSwagger(self):
    status, body = http('GET', self.base + '/health')
    self.assertEqual(200, status)
    self.assertIn('packager_version', json.loads(body))
    status, body = http('GET', self.base + '/swagger/doc')
    self.assertEqual(200, status)
    doc = json.loads(body)  # must be valid JSON
    self.assertTrue('openapi' in doc or 'swagger' in doc)
    paths = doc['paths']
    self.assertIn('/api/v1/events', paths)

  def testValidationAndUnknownEvent(self):
    status, body = http('POST', self.base + '/api/v1/events',
                        {'streams': []})
    self.assertEqual(400, status)
    self.assertEqual('invalid_request', json.loads(body)['error']['code'])
    status, body = http('GET', self.base + '/api/v1/events/nope')
    self.assertEqual(404, status)

  def testFullEventLifecycle(self):
    udp_port = free_port(socket.SOCK_DGRAM)
    create = {
        'event_id': 'e2e',
        'streams': [{
            # timeout is in microseconds (udp_options.cc); 100s gives the
            # demuxer's first read enough time to block until wait_for_state
            # confirms RUNNING and replay_ts.py starts sending. A short
            # timeout here makes packager time out and exit FAILED before
            # any data is ever sent.
            'input': 'udp://127.0.0.1:%d?timeout=100000000' % udp_port,
            'stream': 'video',
            'init_segment': self.tmp_dir + '/video_init.mp4',
            'segment_template': self.tmp_dir + '/video_$Number$.m4s',
        }],
        'mpd_output': self.tmp_dir + '/output.mpd',
        'segment_duration': 1,
        'stop_timeout_seconds': 5,
    }
    status, body = http('POST', self.base + '/api/v1/events', create)
    self.assertEqual(201, status, body)
    self.assertEqual('e2e', json.loads(body)['event_id'])

    # Duplicate id is rejected.
    status, body = http('POST', self.base + '/api/v1/events', create)
    self.assertEqual(409, status, body)

    self.wait_for_state('e2e', 'RUNNING')

    # Feed it a short burst of TS.
    replay = os.path.join(SRC_DIR, 'packager', 'tools', 'redundant_ts',
                          'replay_ts.py')
    ts_file = os.path.join(SRC_DIR, 'packager', 'media', 'test', 'data',
                           'bear-640x360.ts')
    subprocess.check_call(['python3', replay, ts_file,
                           '--ports', str(udp_port), '--pps', '300'])
    time.sleep(2)

    # Metrics proxy shows the event's own counters.
    status, body = http('GET', self.base + '/api/v1/events/e2e/metrics')
    self.assertEqual(200, status, body)
    self.assertIn('shaka_udp_datagrams_received_total', body)

    # Logs endpoint returns something.
    status, body = http('GET', self.base + '/api/v1/events/e2e/logs?tail=50')
    self.assertEqual(200, status)

    # List shows it.
    status, body = http('GET', self.base + '/api/v1/events')
    self.assertEqual(200, status)
    self.assertEqual(1, len(json.loads(body)['events']))

    # Drain stop -> STOPPED with an exit code.
    status, body = http('DELETE', self.base + '/api/v1/events/e2e')
    self.assertEqual(202, status, body)
    final = self.wait_for_state('e2e', 'STOPPED')
    self.assertIn('exit_code', final)

  def testBearerTokenAuth(self):
    # Restart the API with a token.
    self.api.terminate()
    self.api.communicate(timeout=30)
    self.api_port = free_port(socket.SOCK_STREAM)
    self.base = 'http://127.0.0.1:%d' % self.api_port
    self.api = subprocess.Popen([
        API_BIN, '--api_port', str(self.api_port),
        '--api_bind_address', '127.0.0.1',
        '--packager_bin', PACKAGER_BIN,
        '--event_log_dir', self.tmp_dir,
        '--api_token', 'sesame',
    ], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    for _ in range(50):
      try:
        if http('GET', self.base + '/health')[0] == 200:
          break
      except (ConnectionError, urllib.error.URLError):
        time.sleep(0.2)
    self.assertEqual(401, http('GET', self.base + '/api/v1/events')[0])
    self.assertEqual(200, http('GET', self.base + '/api/v1/events',
                               token='sesame')[0])
    self.assertEqual(200, http('GET', self.base + '/health')[0])


if __name__ == '__main__':
  unittest.main(verbosity=2)

#!/usr/bin/env python3
"""The node's web UI on this PC, for trying pages without a board.

Serves batman-adv-lyra/web; the CGIs that talk to merld (merl, chat) go to a merld running on
this PC against test/fake_unit.py (see `just web-sim`), the board-only ones (status, internet,
power, mesh-compass) answer with fixed sample data.

  web_sim.py --sock /tmp/merld-sim/sock [--port 8000]
Open http://localhost:8000/ (or from an emulator: http://10.0.2.2:8000/).
"""
import argparse
import http.server
import json
import os
import subprocess
import time
from urllib.parse import parse_qs

HERE = os.path.dirname(os.path.abspath(__file__))
WEB = os.path.join(HERE, '../../batman-adv-lyra/web')
MERLCTL = os.path.join(HERE, '../build/host/merlctl')


def samples():
    now = int(time.time())
    return {
        '/cgi-bin/status': {
            'hostname': 'luckfox', 'kernel': '6.1.99-erl', 'uptime': 5025, 'load': 0.1,
            'mem_total_kb': 89344, 'mem_avail_kb': 40000, 'mesh': {'ip': '10.10.0.2/24'},
            'neighbours': [], 'originators': [], 'gateways': [],
        },
        '/cgi-bin/internet': {
            'role': 'client', 'online': False, 'latency_ms': None, 'dns': False, 'checked': now,
            'uplink': None, 'gateway': None, 'announced': False, 'nameservers': [],
            'clock': {'synced': False, 'server': None}, 'updated': now,
        },
        '/cgi-bin/power': {
            'ok': True, 'bus_v': 7.90, 'shunt_mv': 18.2, 'current_ma': 182.0, 'avg_current_ma': 180.4,
            'power_mw': 1437.8, 'bus': '/dev/i2c-1', 'address': '0x40', 'address_config': 'auto',
            'shunt_ohm': 0.1, 'max_current_a': 0.8, 'average_samples': 64, 'interval_ms': 1000,
            'cells': 2, 'capacity_mah': 2600, 'updated': now, 'service': 'active', 'now': now,
        },
        # one simulated mesh node with its own (calibrated) heading; the other peers' headings
        # come from the peer table (their ERL broadcasts, fake_unit.py --peers 3)
        '/cgi-bin/mesh-compass': {
            'ok': True, 'updated': now, 'nodes': [
                {'ip': '10.10.0.4', 'address': '0xC704', 'heading': 300.0, 'cardinal': 'NW'},
            ],
        },
    }


class Handler(http.server.SimpleHTTPRequestHandler):
    sock = None

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=WEB, **kwargs)

    def reply(self, body):
        data = body.encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def merlctl(self, *args):
        return subprocess.run([MERLCTL, '-s', self.sock, *args], capture_output=True, text=True).stdout

    def do_GET(self):
        path = self.path.split('?')[0]
        if path == '/cgi-bin/merl':
            return self.reply(self.merlctl('status'))
        if path == '/cgi-bin/chat':
            return self.reply(self.merlctl('chat', 'list', '0', '200'))
        fixed = samples().get(path)
        if fixed is not None:
            return self.reply(json.dumps(fixed))
        if path.startswith('/cgi-bin/'):
            return self.reply('{"ok":false,"error":"not simulated"}')
        return super().do_GET()

    def do_POST(self):
        path = self.path.split('?')[0]
        form = parse_qs(self.rfile.read(int(self.headers.get('Content-Length', 0))).decode())
        f = lambda k: form.get(k, [''])[0]  # noqa: E731
        if path == '/cgi-bin/chat':
            if f('action') == 'clear':
                return self.reply(self.merlctl('chat', 'clear'))
            return self.reply(self.merlctl('chat', 'send', f('to'), f('text')))
        if path == '/cgi-bin/merl' and f('action') == 'peers-clear':
            return self.reply(self.merlctl('peers', 'clear'))
        if path == '/cgi-bin/phone':    # the app's phone GPS / compass
            if f('action') == 'position':
                return self.reply(self.merlctl('phone', 'position', f('lon'), f('lat'), f('acc') or '0'))
            if f('action') == 'compass':
                return self.reply(self.merlctl('phone', 'compass', f('heading')))
            if f('action') == 'gps':
                return self.reply(self.merlctl('phone', 'gps', f('state')))
        return self.reply('{"ok":false,"error":"not simulated"}')

    def log_message(self, *args):
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--sock', default='/tmp/merld-sim/sock', help="merld's control socket")
    parser.add_argument('--port', type=int, default=8000)
    args = parser.parse_args()
    Handler.sock = args.sock
    print(f'web UI on http://localhost:{args.port}/ (merld socket {args.sock})')
    http.server.ThreadingHTTPServer(('0.0.0.0', args.port), Handler).serve_forever()


if __name__ == '__main__':
    main()

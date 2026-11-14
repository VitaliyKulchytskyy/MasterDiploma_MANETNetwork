#!/usr/bin/env python3
"""End-to-end chat test over the real system: two Lyra boards on adb, each with its RP2040
emergency unit, talking over LoRa (ERL).

For both directions (A -> B, B -> A) it sends a direct and a broadcast message with merlctl on
the sending board, then checks
  - the sender's stored status ("sent" = its unit accepted the message for the radio),
  - that the message arrives in the receiver's chat history (merld on the other board),
  - that it arrives with the right sender, receiver and broadcast flag.

If an emergency unit's USB debug port is on this PC (--radio-log, default /dev/ttyACM0 when
present), its log is recorded during each case: whether the unit put the packet on the air or
heard it tells a radio problem from a board / merld problem.

  chat_e2e.py [--a SERIAL] [--b SERIAL] [--rounds N] [--timeout 20] [--radio-log /dev/ttyACM0 | none]
Boards are found automatically when exactly two Lyra boards are on adb.
"""
import argparse
import json
import os
import random
import string
import subprocess
import sys
import termios
import threading
import time
import tty


def adb(serial, *cmd, timeout=30):
    out = subprocess.run(['adb', '-s', serial, 'shell', *cmd], capture_output=True, text=True, timeout=timeout)
    return out.stdout


def merlctl(serial, *args, timeout=30):
    # one argument per word; the message text is quoted for the board's shell
    line = ' '.join("'" + a.replace("'", "'\\''") + "'" for a in args)
    raw = adb(serial, f'merlctl {line}', timeout=timeout)
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        return {'ok': False, 'error': f'no JSON from merlctl: {raw.strip()[:200]}'}


def lyra_boards():
    out = subprocess.run(['adb', 'devices'], capture_output=True, text=True).stdout.splitlines()[1:]
    serials = [l.split()[0] for l in out if l.strip().endswith('device')]
    return [s for s in serials if 'Ubuntu' in adb(s, 'cat /etc/os-release 2>/dev/null', timeout=10)]


class RadioLog:
    """Records an emergency unit's USB debug output (D_printf) in the background."""

    def __init__(self, path):
        self.lines = []
        self.lock = threading.Lock()
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(self.fd)
        attrs = termios.tcgetattr(self.fd)
        attrs[4] = attrs[5] = termios.B115200
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        self.stop = False
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        pending = b''
        while not self.stop:
            try:
                pending += os.read(self.fd, 65536)
            except BlockingIOError:
                time.sleep(0.05)
                continue
            *complete, pending = pending.split(b'\n')
            with self.lock:
                self.lines += [(time.time(), l.decode(errors='replace').strip()) for l in complete]

    def since(self, t0):
        with self.lock:
            return [l for t, l in self.lines if t >= t0]


def state(serial):
    reply = merlctl(serial, 'status')
    return reply.get('state') or {}


def wait_for(fn, timeout, step=1.0):
    end = time.time() + timeout
    while time.time() < end:
        value = fn()
        if value:
            return value
        time.sleep(step)
    return fn()


def run_case(name, sender, receiver, to, expect_broadcast, timeout, radio):
    token = ''.join(random.choices(string.ascii_lowercase + string.digits, k=6))
    text = f'[e2e {name} {token}]'
    rx_last = merlctl(receiver['serial'], 'chat', 'list', '0', '1').get('last_id', 0)
    t0 = time.time()

    reply = merlctl(sender['serial'], 'chat', 'send', to, text)
    message_id = reply.get('message_id')

    def sent_status():
        for m in merlctl(sender['serial'], 'chat', 'list', '0', '20').get('messages', []):
            if m['id'] == message_id and m['status'] != 'pending':
                return m['status']
        return None

    status = wait_for(sent_status, 10) if message_id else None

    def arrived():
        for m in merlctl(receiver['serial'], 'chat', 'list', str(rx_last), '50').get('messages', []):
            if m['dir'] == 'in' and m['text'] == text:
                return m
        return None

    got = wait_for(arrived, timeout) if status == 'sent' else None
    latency = time.time() - t0

    problems = []
    if not reply.get('ok'):
        problems.append(f"send refused: {reply.get('error') or reply.get('answer')}")
    elif status != 'sent':
        problems.append(f'sender status: {status}')
    elif not got:
        problems.append(f'not received within {timeout} s')
    else:
        if got['from'] != sender['address']:
            problems.append(f"from {got['from']}, expected {sender['address']}")
        if got['to'] != to_address(to, sender):
            problems.append(f"to {got['to']}, expected {to_address(to, sender)}")
        if got['broadcast'] != expect_broadcast:
            problems.append(f"broadcast flag {got['broadcast']}, expected {expect_broadcast}")

    radio_lines = []
    if radio:
        time.sleep(0.5)
        radio_lines = [l for l in radio.since(t0) if 'type=0x5' in l or '0x5000' in l]
    return {'name': name, 'text': text, 'ok': not problems, 'problems': problems,
            'latency': latency if got else None, 'radio': radio_lines}


def to_address(to, sender):
    if to != 'broadcast':
        return to
    address, mask = int(sender['address'], 16), int(sender['mask'], 16)
    return f'0x{(address | (~mask & 0xFFFF)):04X}'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--a')
    parser.add_argument('--b')
    parser.add_argument('--timeout', type=float, default=20, help="seconds to wait for a message to arrive")
    parser.add_argument('--rounds', type=int, default=1, help="run the 4 cases this many times (delivery rate)")
    parser.add_argument('--radio-log', default='/dev/ttyACM0' if os.path.exists('/dev/ttyACM0') else 'none')
    args = parser.parse_args()

    serials = [args.a, args.b] if args.a and args.b else lyra_boards()
    if len(serials) != 2:
        sys.exit(f'need exactly two Lyra boards on adb (found {len(serials)}); pass --a/--b')

    boards = []
    for s in serials:
        st = state(s)
        net = st.get('network')
        fw = st.get('firmware') or {}
        if st.get('link') != 'up' or not net:
            sys.exit(f"{s}: emergency unit link is {st.get('link')!r}, network {net}: fix that first")
        boards.append({'serial': s, 'address': net['address'], 'mask': net['subnet_mask'],
                       'fw': f"v{fw.get('version', '?')} #{fw.get('build', '?')}"})
    a, b = boards
    for label, board in (('A', a), ('B', b)):
        print(f"{label}: {board['serial']}  ERL {board['address']}/{board['mask']}  firmware {board['fw']}")

    radio = None
    if args.radio_log != 'none':
        try:
            radio = RadioLog(args.radio_log)
            print(f'radio log: {args.radio_log}')
        except OSError as e:
            print(f'radio log {args.radio_log} unavailable: {e}')
    print()

    cases = [
        ('A->B direct', a, b, b['address'], False),
        ('A->B broadcast', a, b, 'broadcast', True),
        ('B->A direct', b, a, a['address'], False),
        ('B->A broadcast', b, a, 'broadcast', True),
    ]
    results = []
    for name, sender, receiver, to, bcast in [c for _ in range(args.rounds) for c in cases]:
        r = run_case(name, sender, receiver, to, bcast, args.timeout, radio)
        results.append(r)
        mark = 'PASS' if r['ok'] else 'FAIL'
        extra = f"{r['latency']:.1f} s" if r['latency'] is not None else '; '.join(r['problems'])
        print(f'{mark}  {name:<16} {extra}')
        for line in r['radio']:
            print(f'        radio: {line}')
        time.sleep(2)       # let the radios settle between cases

    failed = [r for r in results if not r['ok']]
    print()
    for name, *_ in cases:
        runs = [r for r in results if r['name'] == name]
        ok = [r for r in runs if r['ok']]
        avg = sum(r['latency'] for r in ok) / len(ok) if ok else 0
        print(f"{name:<16} {len(ok)}/{len(runs)} delivered" + (f", avg {avg:.1f} s" if ok else ''))
    print(f'{len(results) - len(failed)}/{len(results)} passed')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()

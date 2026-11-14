#!/usr/bin/env python3
"""
Interactive CLI for the MERL protocol (main unit <-> emergency unit), see README.md.

Connect a USB-serial adapter to the emergency unit's UART_MAIN pins (GP0/GP1 by default),
NOT to the USB CDC port that carries the debug log. Requires pyserial.

One-shot usage:
    ./merl_cli.py --port /dev/ttyUSB0 network get
    ./merl_cli.py --port /dev/ttyUSB0 network set 0xCF01 0xFFE0
    ./merl_cli.py --port /dev/ttyUSB0 peers get
    ./merl_cli.py --port /dev/ttyUSB0 peers clear
    ./merl_cli.py --port /dev/ttyUSB0 position get
    ./merl_cli.py --port /dev/ttyUSB0 message send 0xCF1F "Hi!"
    ./merl_cli.py --port /dev/ttyUSB0 listen
    ./merl_cli.py --port /dev/ttyUSB0 raw 0x1000
    ./merl_cli.py --port /dev/ttyUSB0 raw 0x5000 --payload 0000C71F48690D0A

Interactive mode (also does listening for passthrough messages in the background):
    ./merl_cli.py --port /dev/ttyUSB0

See merl_test.py for the automated test suite.
"""

import argparse
import cmd
import shlex
import struct
import sys
import threading
import time

from merl_link import (
    Ansi, CMD_NETWORK_GET, CMD_NETWORK_SET, CMD_PEERS_CLEAR, CMD_PEERS_GET, CMD_POSITION_GET,
    CMD_POSITION_SET, CMD_USER_MESSAGE_SEND, ERL_MAX_PAYLOAD,
    PEER_SIZE, Link, LinkError, color, command_name, describe_pushed, request,
)


class CliError(Exception):
    pass


def parse_int(text: str, what: str) -> int:
    try:
        return int(text, 0)
    except ValueError:
        raise CliError(f"{what} must be an integer, e.g. 1234 or 0xCF01: {text!r}")


def parse_address(text: str, what: str = "address") -> int:
    value = parse_int(text, what)
    if not 0 <= value <= 0xFFFF:
        raise CliError(f"{what} must fit into 16 bits: 0x{value:X}")
    return value


def format_peer(raw: bytes) -> str:
    # peer_t: uint32_t address; int8_t rssi; uint32_t uptime; int32_t longitude; int32_t latitude;
    #         uint32_t config: 24; int16_t compass_x, compass_y, compass_z
    # the 24-bit config bitfield only takes 3 bytes in the packed struct, not 4 - PEER_SIZE is 26, not 27
    address, rssi, uptime, longitude, latitude = struct.unpack('<IbIii', raw[:17])
    config = int.from_bytes(raw[17:20], 'little')
    compass_x, compass_y, compass_z = struct.unpack('<hhh', raw[20:26])
    compass = "unset" if (compass_x, compass_y, compass_z) == (0, 0, 0) else f"({compass_x}, {compass_y}, {compass_z})"
    return (f"address=0x{address:08X} rssi={rssi} dBm uptime={uptime}s "
            f"longitude={format_coordinate(longitude)} latitude={format_coordinate(latitude)} "
            f"config=0x{config:06X} compass={compass}")


def do_network_get(link: Link, _args: list[str]) -> None:
    answer = link.require_answer(request(CMD_NETWORK_GET))
    address, mask = struct.unpack('>HH', answer.payload)
    broadcast = (address & mask) | (~mask & 0xFFFF)
    print(f"address=0x{address:04X} mask=0x{mask:04X} broadcast=0x{broadcast:04X}")


def do_network_set(link: Link, args: list[str]) -> None:
    if len(args) != 2:
        raise CliError("usage: network set <address> <mask>")
    address, mask = parse_address(args[0]), parse_address(args[1], "mask")
    link.require_answer(request(CMD_NETWORK_SET, struct.pack('>HH', address, mask)))
    print(color("ok", Ansi.GREEN))


def do_peers_get(link: Link, _args: list[str]) -> None:
    answer = link.require_answer(request(CMD_PEERS_GET))
    if len(answer.payload) % PEER_SIZE != 0:
        raise CliError(f"unexpected payload size {len(answer.payload)}, not a multiple of {PEER_SIZE}")

    count = len(answer.payload) // PEER_SIZE
    if count == 0:
        print("(no peers)")
    for i in range(count):
        print(format_peer(answer.payload[i * PEER_SIZE:(i + 1) * PEER_SIZE]))


def do_peers_clear(link: Link, _args: list[str]) -> None:
    link.require_answer(request(CMD_PEERS_CLEAR))
    print(color("ok", Ansi.GREEN))


def format_coordinate(microdegrees: int) -> str:
    return f"{microdegrees / 1_000_000:.6f}"


def do_position_get(link: Link, _args: list[str]) -> None:
    answer = link.require_answer(request(CMD_POSITION_GET))
    time_, longitude, latitude = struct.unpack('>Iii', answer.payload)
    fix = "manually set" if time_ == 0 else f"GPS fix, {time_} ms since UTC midnight"
    print(f"longitude={format_coordinate(longitude)} latitude={format_coordinate(latitude)}  ({fix})")


def do_position_set(link: Link, args: list[str]) -> None:
    if len(args) != 2:
        raise CliError("usage: position set <longitude> <latitude>   (degrees, e.g. 30.523456 50.450100)")
    longitude = round(float(args[0]) * 1_000_000)
    latitude = round(float(args[1]) * 1_000_000)
    link.require_answer(request(CMD_POSITION_SET, struct.pack('>ii', longitude, latitude)))
    print(color("ok", Ansi.GREEN))


def do_message_send(link: Link, args: list[str]) -> None:
    if len(args) < 2:
        raise CliError('usage: message send <receiver> "<text>"')
    receiver = parse_address(args[0], "receiver")
    text = " ".join(args[1:]).encode()
    if not text.endswith(b'\r\n'):
        text += b'\r\n'
    if len(text) > ERL_MAX_PAYLOAD:
        raise CliError(f"message is {len(text)} bytes, the limit is {ERL_MAX_PAYLOAD}")

    link.require_answer(request(CMD_USER_MESSAGE_SEND, struct.pack('>I', receiver) + text))
    print(color("ok", Ansi.GREEN))


def do_raw(link: Link, args: list[str]) -> None:
    if not args:
        raise CliError("usage: raw <command> [--payload <hex>]")

    command = parse_int(args[0], "command")
    payload = b''
    rest = args[1:]
    if rest:
        if len(rest) != 2 or rest[0] != '--payload':
            raise CliError("usage: raw <command> [--payload <hex>]")
        try:
            payload = bytes.fromhex(rest[1])
        except ValueError:
            raise CliError(f"payload is not valid hex: {rest[1]!r}")

    answer = link.exchange(request(command, payload))
    if answer is None:
        raise CliError("no answer")
    print(f"{command_name(answer.command)} len={len(answer.payload)} payload={answer.payload.hex(' ')}")


COMMANDS = {
    ('network', 'get'): do_network_get,
    ('network', 'set'): do_network_set,
    ('peers', 'get'): do_peers_get,
    ('peers', 'clear'): do_peers_clear,
    ('position', 'get'): do_position_get,
    ('position', 'set'): do_position_set,
    ('message', 'send'): do_message_send,
    ('raw',): do_raw,
}


def print_pushed_message(link: Link, on_message) -> bool:
    """Acknowledge one request the device sent on its own (a chat message received over ERL,
    or a passthrough), if any, and report it. Returns whether a frame was found."""
    frame = link.read_frame(0)
    if frame is None:
        return False
    text = describe_pushed(frame)
    if text is not None:
        on_message(text)
        link.acknowledge(frame)
    return True


class MerlShell(cmd.Cmd):
    intro = "MERL interactive shell. Type help or ? for the commands, Ctrl-D to quit."
    prompt = "merl> "

    def __init__(self, link: Link):
        super().__init__()
        self.link = link
        self._stop_listener = threading.Event()
        self._listener = threading.Thread(target=self._listen, daemon=True)
        self._listener.start()

    def _listen(self) -> None:
        # runs between commands typed by the user; exchange()/require_answer() briefly
        # pause it (see _pause_listener) so it does not race the reply to a request
        while not self._stop_listener.is_set():
            with self._listener_lock():
                try:
                    print_pushed_message(self.link, lambda text: print(
                        f"\n{color(f'[radio] {text}', Ansi.CYAN, Ansi.BOLD)}\n{self.prompt}", end=''))
                except Exception:
                    pass
            time.sleep(0.1)

    def _listener_lock(self):
        return _NullContext()

    def _dispatch(self, prefix: tuple, args: list[str]) -> None:
        handler = COMMANDS.get(prefix)
        if handler is None:
            print(f"unknown command: {' '.join(prefix)}")
            return
        try:
            handler(self.link, args)
        except (CliError, LinkError) as error:
            print(color(f"error: {error}", Ansi.RED))

    def do_network(self, line: str) -> None:
        """network get | network set <address> <mask>"""
        args = shlex.split(line)
        if not args:
            print("usage: network get | network set <address> <mask>")
            return
        self._dispatch(('network', args[0]), args[1:])

    def do_peers(self, line: str) -> None:
        """peers get | peers clear"""
        args = shlex.split(line)
        if not args:
            print("usage: peers get | peers clear")
            return
        self._dispatch(('peers', args[0]), args[1:])

    def do_position(self, line: str) -> None:
        """position get | position set <longitude> <latitude>"""
        args = shlex.split(line)
        if not args:
            print("usage: position get | position set <longitude> <latitude>")
            return
        self._dispatch(('position', args[0]), args[1:])

    def do_message(self, line: str) -> None:
        """message send <receiver> <text>"""
        args = shlex.split(line)
        if not args:
            print('usage: message send <receiver> "<text>"')
            return
        self._dispatch(('message', args[0]), args[1:])

    def do_raw(self, line: str) -> None:
        """raw <command> [--payload <hex>]"""
        self._dispatch(('raw',), shlex.split(line))

    def do_quit(self, _line: str) -> bool:
        """Exit the shell."""
        return True

    def do_EOF(self, _line: str) -> bool:
        print()
        return True

    def postcmd(self, stop: bool, line: str) -> bool:
        if stop:
            self._stop_listener.set()
        return stop


class _NullContext:
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def run_one_shot(link: Link, args: argparse.Namespace) -> int:
    prefix = (args.group,) if args.group == 'raw' else (args.group, args.action)
    handler = COMMANDS.get(prefix)
    if handler is None:
        print(f"unknown command: {' '.join(p for p in prefix if p)}", file=sys.stderr)
        return 1

    try:
        handler(link, args.args)
    except (CliError, LinkError) as error:
        print(color(f"error: {error}", Ansi.RED), file=sys.stderr)
        return 1
    return 0


def run_listen(link: Link, seconds: float) -> int:
    print(f"listening for {seconds} s ..." if seconds > 0 else "listening, Ctrl-C to stop ...")
    deadline = time.monotonic() + seconds if seconds > 0 else None
    try:
        while deadline is None or time.monotonic() < deadline:
            if not print_pushed_message(link, lambda text: print(color(text, Ansi.CYAN, Ansi.BOLD))):
                time.sleep(0.05)
    except KeyboardInterrupt:
        pass
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Interactive CLI for the MERL protocol", formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument('--port', required=True, help="serial port of the MERL UART, e.g. /dev/ttyUSB0")
    parser.add_argument('--baudrate', type=int, default=115200)
    parser.add_argument('--timeout', type=float, default=0.5, help="seconds to wait for an answer")
    parser.add_argument('-v', '--verbose', action='store_true', help="print every byte sent and received")

    commands = parser.add_subparsers(dest='group')
    for name, help_text in [('network', "get/set the network address and subnet mask"),
                             ('peers', "get/clear the peer table"),
                             ('position', "get/set the GPS position")]:
        sub = commands.add_parser(name, help=help_text)
        sub.add_argument('action')
        sub.add_argument('args', nargs='*')

    message = commands.add_parser('message', help="send a user message over the radio")
    message.add_argument('action')
    message.add_argument('args', nargs='*')

    raw = commands.add_parser('raw', help="send a raw request, e.g. raw 0x1000")
    raw.add_argument('args', nargs=argparse.REMAINDER)

    listen = commands.add_parser('listen', help="print the user messages passed through from the radio")
    listen.add_argument('seconds', type=float, nargs='?', default=0, help="0 = until Ctrl-C")

    args = parser.parse_args()

    with Link(args.port, args.baudrate, args.timeout, args.verbose) as link:
        if args.group is None:
            MerlShell(link).cmdloop()
            return 0
        if args.group == 'listen':
            return run_listen(link, args.seconds)
        if args.group == 'raw':
            return run_one_shot(link, argparse.Namespace(group='raw', action=None, args=args.args))
        return run_one_shot(link, args)


if __name__ == '__main__':
    sys.exit(main())

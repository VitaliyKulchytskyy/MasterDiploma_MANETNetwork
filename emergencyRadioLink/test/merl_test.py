#!/usr/bin/env python3
"""
Test tool for the MERL protocol (main unit <-> emergency unit), see README.md.

Connect a USB-serial adapter to the emergency unit's UART_MAIN pins (GP0/GP1 by default),
NOT to the USB CDC port that carries the debug log. Requires pyserial.

Usage:
    ./merl_test.py --port /dev/ttyUSB0                    run the test suite
    ./merl_test.py --port /dev/ttyUSB0 --set-network      also test NETWORK_SET (writes the flash twice)
    ./merl_test.py --port /dev/ttyUSB0 --list             list the tests
    ./merl_test.py --port /dev/ttyUSB0 -k user            run the tests with "user" in the name
    ./merl_test.py --port /dev/ttyUSB0 send 0x1000        send one request and print the answer
    ./merl_test.py --port /dev/ttyUSB0 send 0x5000 --payload 0000C71F48690D0A
    ./merl_test.py --port /dev/ttyUSB0 listen 60          print user messages coming from the radio

See merl_cli.py for interactive, everyday use of the protocol.
"""

import argparse
import struct
import sys
import time
from typing import Callable

from merl_link import (
    ANSWER, Ansi, CMD_ERROR, CMD_NETWORK_SET, CMD_OK, CMD_OK_BODY, CMD_OVERLOAD, CMD_PEERS_CLEAR,
    CMD_PEERS_GET, CMD_POSITION_GET, CMD_POSITION_SET, CMD_UNIMPLEMENTED, CMD_UNKNOWN,
    CMD_USER_MESSAGE_PASSTHROUGH, CMD_USER_MESSAGE_SEND, ERL_MAX_PAYLOAD, Frame, Link,
    MAX_PAYLOAD, PEER_SIZE, RX_TIMEOUT_MS, color, describe_pushed, request,
)


class Failure(Exception):
    pass


TESTS: list[tuple[str, Callable, bool]] = []


def test(name: str, needs_flash_write: bool = False):
    def decorator(func):
        TESTS.append((name, func, needs_flash_write))
        return func
    return decorator


def check(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


def expect_answer(link: Link, frame: Frame) -> Frame:
    answer = link.exchange(frame)
    check(answer is not None, f"no answer to command 0x{frame.command:04X}")
    check(answer.direction == ANSWER, f"answer has direction {chr(answer.direction)!r}")
    return answer


def expect_status(link: Link, frame: Frame, status: int) -> None:
    answer = expect_answer(link, frame)
    check(answer.command == status, f"expected 0x{status:04X}, got 0x{answer.command:04X}")
    check(answer.payload == struct.pack('>H', frame.command),
          f"status must carry the request command, got {answer.payload.hex(' ')}")


def get_network(link: Link) -> tuple[int, int]:
    answer = expect_answer(link, request(0x1000))
    check(answer.command == CMD_OK_BODY, f"expected 0x0001, got 0x{answer.command:04X}")
    check(len(answer.payload) == 4, f"expected 4 bytes of params, got {len(answer.payload)}")
    return struct.unpack('>HH', answer.payload)


def broadcast_address(link: Link) -> int:
    address, mask = get_network(link)
    return (address & mask) | (~mask & 0xFFFF)


@test("network: get")
def _(link, args):
    address, mask = get_network(link)
    print(f"    address=0x{address:04X} mask=0x{mask:04X}")


@test("network: set with a wrong payload size is rejected")
def _(link, args):
    before = get_network(link)
    expect_status(link, request(CMD_NETWORK_SET, b'\x00\x01\x02'), CMD_ERROR)
    expect_status(link, request(CMD_NETWORK_SET, b''), CMD_ERROR)
    check(get_network(link) == before, "params changed by a rejected request")


@test("network: set and get back, then restore", needs_flash_write=True)
def _(link, args):
    address, mask = get_network(link)
    new = (address ^ 0x0001, mask)
    try:
        expect_status(link, request(CMD_NETWORK_SET, struct.pack('>HH', *new)), CMD_OK)
        check(get_network(link) == new, "new params were not applied")
    finally:
        expect_status(link, request(CMD_NETWORK_SET, struct.pack('>HH', address, mask)), CMD_OK)
    check(get_network(link) == (address, mask), "original params were not restored")


@test("peers: get returns whole peer records")
def _(link, args):
    answer = expect_answer(link, request(CMD_PEERS_GET))
    check(answer.command == CMD_OK_BODY, f"expected 0x0001, got 0x{answer.command:04X}")
    check(len(answer.payload) % PEER_SIZE == 0, f"{len(answer.payload)} bytes is not a multiple of {PEER_SIZE}")
    print(f"    {len(answer.payload) // PEER_SIZE} peer(s)")


@test("peers: clear empties the table")
def _(link, args):
    expect_status(link, request(CMD_PEERS_CLEAR), CMD_OK)
    answer = expect_answer(link, request(CMD_PEERS_GET))
    check(answer.command == CMD_OK_BODY and answer.payload == b'', "table is not empty after clear")


@test("position: get without a fix is an error")
def _(link, args):
    # only meaningful right after boot / peers clear, before any fix or manual set;
    # once a fix or a manual position exists there is no way to un-set it over MERL
    answer = link.exchange(request(CMD_POSITION_GET))
    if answer is not None and answer.command == CMD_OK_BODY:
        print("    (device already has a position, skipping)")
        return
    check(answer is not None, "no answer to POSITION_GET")
    check(answer.command == CMD_ERROR, f"expected 0x0100, got 0x{answer.command:04X}")


@test("position: set and get back")
def _(link, args):
    longitude, latitude = 30_523456, 50_450100    # microdegrees; Kyiv-ish, arbitrary
    expect_status(link, request(CMD_POSITION_SET, struct.pack('>ii', longitude, latitude)), CMD_OK)

    answer = expect_answer(link, request(CMD_POSITION_GET))
    check(answer.command == CMD_OK_BODY, f"expected 0x0001, got 0x{answer.command:04X}")
    check(len(answer.payload) == 12, f"expected 12 bytes, got {len(answer.payload)}")
    time_, got_longitude, got_latitude = struct.unpack('>Iii', answer.payload)
    check(time_ == 0, f"time should be 0 for a manually set position, got {time_}")
    check((got_longitude, got_latitude) == (longitude, latitude),
          f"expected ({longitude}, {latitude}), got ({got_longitude}, {got_latitude})")


@test("position: set with a wrong payload size is rejected")
def _(link, args):
    expect_status(link, request(CMD_POSITION_SET, bytes(7)), CMD_ERROR)
    expect_status(link, request(CMD_POSITION_SET, b''), CMD_ERROR)


@test("unknown command")
def _(link, args):
    expect_status(link, request(0x9999), CMD_UNKNOWN)
    expect_status(link, request(0x0001, b'\x01'), CMD_UNKNOWN)


@test("user message: send")
def _(link, args):
    receiver = args.receiver if args.receiver is not None else broadcast_address(link)
    payload = struct.pack('>I', receiver) + b'Hi!\r\n'
    expect_status(link, request(CMD_USER_MESSAGE_SEND, payload), CMD_OK)


@test("user message: longest message (128 bytes) is accepted")
def _(link, args):
    receiver = args.receiver if args.receiver is not None else broadcast_address(link)
    message = b'a' * (ERL_MAX_PAYLOAD - 2) + b'\r\n'
    expect_status(link, request(CMD_USER_MESSAGE_SEND, struct.pack('>I', receiver) + message), CMD_OK)


@test("user message: invalid messages are rejected")
def _(link, args):
    receiver = struct.pack('>I', broadcast_address(link))
    cases = {
        "no \\r\\n at the end": receiver + b'Hi!!!',
        "only \\r": receiver + b'Hi!\r',
        "only \\n": receiver + b'Hi!\n',
        "no message": receiver,
        "no address": b'',
        "address above 16 bit": struct.pack('>I', 0x0001C71F) + b'Hi\r\n',
        "message longer than 128 bytes": receiver + b'a' * (ERL_MAX_PAYLOAD - 1) + b'\r\n',
    }
    for name, payload in cases.items():
        print(f"    {name}")
        expect_status(link, request(CMD_USER_MESSAGE_SEND, payload), CMD_ERROR)


@test("framing: a request with a bad checksum gets no answer")
def _(link, args):
    raw = bytearray(request(0x1000).encode())
    raw[-1] ^= 0xFF
    link.flush()
    link.write(bytes(raw))
    check(link.wait_answer() is None, "device answered a frame with a bad checksum")
    get_network(link)   # and it still works afterwards


@test("framing: garbage in front of a request is skipped")
def _(link, args):
    link.flush()
    link.write(b'\x00\xFF\x13 hello ' + request(0x1000).encode())
    answer = link.wait_answer()
    check(answer is not None and answer.command == CMD_OK_BODY, "request behind garbage was not answered")


@test("framing: request sent in pieces is assembled")
def _(link, args):
    raw = request(0x1000).encode()
    link.flush()
    for i in range(0, len(raw), 2):
        link.write(raw[i:i + 2])
        time.sleep(0.02)                    # well below the receive timeout
    answer = link.wait_answer()
    check(answer is not None and answer.command == CMD_OK_BODY, "split request was not answered")


@test("framing: half a request is dropped after the timeout")
def _(link, args):
    raw = request(0x1000).encode()
    link.flush()
    link.write(raw[:4])
    time.sleep(RX_TIMEOUT_MS / 1000 * 3)
    link.write(raw[4:])                     # the rest arrives too late
    check(link.wait_answer() is None, "device answered a request that was cut by the timeout")
    get_network(link)                       # the parser is back in sync


@test("framing: two requests in a row are both answered")
def _(link, args):
    raw = request(0x1000).encode()
    link.flush()
    link.write(raw + raw)
    check(link.wait_answer() is not None, "first request was not answered")
    check(link.wait_answer() is not None, "second request was not answered")


@test("framing: answer frames from the main unit are ignored")
def _(link, args):
    link.flush()
    link.write(Frame(ANSWER, CMD_OK, struct.pack('>H', CMD_USER_MESSAGE_PASSTHROUGH)).encode())
    check(link.wait_answer() is None, "device replied to an answer frame")
    get_network(link)


@test("framing: maximum payload size")
def _(link, args):
    # a request that is not a known command, with the biggest allowed payload
    expect_status(link, request(0x9999, bytes(MAX_PAYLOAD)), CMD_UNKNOWN)


def run_suite(link: Link, args) -> int:
    selected = [(n, f, w) for n, f, w in TESTS if not args.k or args.k.lower() in n.lower()]
    passed = failed = skipped = 0

    for name, func, needs_flash_write in selected:
        if needs_flash_write and not args.set_network:
            print(f"{color('SKIP', Ansi.YELLOW)}  {name}   (use --set-network)")
            skipped += 1
            continue

        print(f"{color('RUN', Ansi.CYAN)}   {name}")
        try:
            func(link, args)
            print(f"{color('PASS', Ansi.GREEN, Ansi.BOLD)}  {name}")
            passed += 1
        except Failure as error:
            print(f"{color('FAIL', Ansi.RED, Ansi.BOLD)}  {name}: {color(str(error), Ansi.RED)}")
            failed += 1

    summary = f"{passed} passed, {failed} failed, {skipped} skipped"
    print(f"\n{color(summary, Ansi.BOLD, Ansi.RED if failed else Ansi.GREEN)}")
    return 1 if failed else 0


def run_send(link: Link, args) -> int:
    payload = bytes.fromhex(args.payload)
    answer = link.exchange(request(int(args.command, 0), payload))
    if answer is None:
        print("no answer")
        return 1
    print(answer)
    return 0


def run_listen(link: Link, args) -> int:
    """Print the user messages the device passes through from the radio and acknowledge them."""
    print(f"listening for {args.seconds} s ...")
    deadline = time.monotonic() + args.seconds
    while (remaining := deadline - time.monotonic()) > 0:
        frame = link.read_frame(remaining)
        if frame is None:
            break
        text = describe_pushed(frame)
        if text is not None:
            print(text)
            link.acknowledge(frame)
        else:
            print(frame)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="MERL protocol test tool", formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument('--port', help="serial port of the MERL UART, e.g. /dev/ttyUSB0")
    parser.add_argument('--baudrate', type=int, default=115200)
    parser.add_argument('--timeout', type=float, default=0.5, help="seconds to wait for an answer")
    parser.add_argument('--receiver', type=lambda v: int(v, 0), help="ERL address for the user message tests (default: subnet broadcast)")
    parser.add_argument('--set-network', action='store_true', help="also run the tests that write the network params to flash")
    parser.add_argument('-k', metavar='TEXT', help="run only the tests whose name contains TEXT")
    parser.add_argument('-v', '--verbose', action='store_true', help="print every byte sent and received")
    parser.add_argument('--list', action='store_true', help="list the tests and exit")

    commands = parser.add_subparsers(dest='action')
    send = commands.add_parser('send', help="send one request and print the answer")
    send.add_argument('command', help="command, e.g. 0x1000")
    send.add_argument('--payload', default='', help="payload as hex, e.g. 0000C71F48690D0A")
    listen = commands.add_parser('listen', help="print the user messages passed through from the radio")
    listen.add_argument('seconds', type=float, nargs='?', default=30)

    args = parser.parse_args()

    if args.list:
        for name, _, needs_flash_write in TESTS:
            print(name + ("   (needs --set-network)" if needs_flash_write else ""))
        return 0
    if not args.port:
        parser.error("--port is required")

    with Link(args.port, args.baudrate, args.timeout, args.verbose) as link:
        if args.action == 'send':
            return run_send(link, args)
        if args.action == 'listen':
            return run_listen(link, args)
        return run_suite(link, args)


if __name__ == '__main__':
    sys.exit(main())

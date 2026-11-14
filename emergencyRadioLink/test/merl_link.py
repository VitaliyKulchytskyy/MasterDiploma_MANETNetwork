"""
MERL protocol (main unit <-> emergency unit) wire format and serial link, see README.md.
Shared by merl_test.py and merl_cli.py.
"""

import os
import struct
import sys
import time
from dataclasses import dataclass
from typing import Optional

import serial


class Ansi:
    """ANSI escape codes for terminal formatting. Combine with color(), e.g. color(text, Ansi.BOLD, Ansi.RED)."""
    RESET = "\x1b[0m"
    BOLD = "\x1b[1m"
    DIM = "\x1b[2m"
    RED = "\x1b[31m"
    GREEN = "\x1b[32m"
    YELLOW = "\x1b[33m"
    CYAN = "\x1b[36m"


# Disabled when stdout is redirected (a file, a pipe) or NO_COLOR is set, per https://no-color.org/
_COLOR_ENABLED = sys.stdout.isatty() and not os.environ.get('NO_COLOR')


def color(text: str, *codes: str) -> str:
    """Wrap text in the given Ansi codes, or return it unchanged if color output is disabled."""
    if not _COLOR_ENABLED or not codes:
        return text
    return "".join(codes) + text + Ansi.RESET


SYNC = ord('*')
VERSION = ord('a')
REQUEST = ord('>')
ANSWER = ord('<')
CRC8_POLY = 0x4A
HEADER_SIZE = 6
MAX_PAYLOAD = 255 - HEADER_SIZE     # crc8_calc() takes a uint8_t length
ERL_MAX_PAYLOAD = 128
RX_TIMEOUT_MS = 100                 # MERL_PROTOCOL_RX_TIMEOUT_MS in the firmware
PEER_SIZE = 26                      # sizeof(peer_t)

CMD_OK = 0x0000
CMD_OK_BODY = 0x0001
CMD_ERROR = 0x0100
CMD_UNKNOWN = 0x0101
CMD_UNIMPLEMENTED = 0x0102
CMD_OVERLOAD = 0x0110
CMD_NETWORK_GET = 0x1000
CMD_NETWORK_SET = 0x1001
CMD_PEERS_GET = 0x2000
CMD_PEERS_CLEAR = 0x2001
CMD_RADIO_STATS_GET = 0x2002
CMD_LINK_STATS_GET = 0x2003
CMD_POSITION_GET = 0x3000
CMD_POSITION_SET = 0x3001
CMD_COMPASS_GET = 0x3002
CMD_COMPASS_SET = 0x3003
CMD_GPS_STATUS = 0x3004
CMD_DEVICE_VERSION = 0x4000
CMD_DEVICE_FEATURES = 0x4001
CMD_USER_MESSAGE_SEND = 0x5000
CMD_USER_MESSAGE_PASSTHROUGH = 0x5001

COMMAND_NAMES = {
    CMD_OK: "OK", CMD_OK_BODY: "OK_BODY", CMD_ERROR: "ERROR", CMD_UNKNOWN: "UNKNOWN",
    CMD_UNIMPLEMENTED: "UNIMPLEMENTED", CMD_OVERLOAD: "OVERLOAD",
    CMD_NETWORK_GET: "NETWORK_GET", CMD_NETWORK_SET: "NETWORK_SET",
    CMD_PEERS_GET: "PEERS_GET", CMD_PEERS_CLEAR: "PEERS_CLEAR",
    CMD_RADIO_STATS_GET: "RADIO_STATS_GET", CMD_LINK_STATS_GET: "LINK_STATS_GET",
    CMD_POSITION_GET: "POSITION_GET", CMD_POSITION_SET: "POSITION_SET",
    CMD_COMPASS_GET: "COMPASS_GET", CMD_COMPASS_SET: "COMPASS_SET", CMD_GPS_STATUS: "GPS_STATUS",
    CMD_DEVICE_VERSION: "DEVICE_VERSION", CMD_DEVICE_FEATURES: "DEVICE_FEATURES",
    CMD_USER_MESSAGE_SEND: "USER_MESSAGE_SEND", CMD_USER_MESSAGE_PASSTHROUGH: "USER_MESSAGE_PASSTHROUGH",
}


# DEVICE_FEATURES ids (merl.h MERL_FEATURE_*)
FEATURE_NAMES = {0x00: "GPS", 0x01: "COMPASS", 0x02: "LINK_STATS"}


def command_name(command: int) -> str:
    return COMMAND_NAMES.get(command, f"0x{command:04X}")


def crc8(data: bytes, poly: int = CRC8_POLY) -> int:
    """Same as crc8_calc() in the firmware: init 0, reflected, shifting right."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ poly if crc & 1 else crc >> 1
    return crc


@dataclass
class Frame:
    direction: int
    command: int
    payload: bytes

    def encode(self) -> bytes:
        raw = struct.pack('>BBBHB', SYNC, VERSION, self.direction, self.command, len(self.payload)) + self.payload
        return raw + bytes([crc8(raw)])

    def __str__(self) -> str:
        arrow = '>' if self.direction == REQUEST else '<'
        return f"[{arrow}] command={command_name(self.command)} len={len(self.payload)} payload={self.payload.hex(' ')}"


def request(command: int, payload: bytes = b'') -> Frame:
    return Frame(REQUEST, command, payload)


def describe_pushed(frame: Frame) -> Optional[str]:
    """Text for a request the device sent on its own, None if it is not one.
    USER_MESSAGE_SEND device -> main: [receiver 2B][sender 2B][text \\r\\n]."""
    if frame.direction != REQUEST:
        return None
    if frame.command == CMD_USER_MESSAGE_SEND and len(frame.payload) >= 4:
        receiver, sender = struct.unpack('>HH', frame.payload[:4])
        text = frame.payload[4:].rstrip(b'\r\n').decode(errors='replace')
        return f"message 0x{sender:04X} -> 0x{receiver:04X}: {text!r}"
    if frame.command == CMD_USER_MESSAGE_PASSTHROUGH:
        return f"passthrough: {frame.payload.hex(' ')}"
    return None


class LinkError(Exception):
    pass


class Link:
    """Frame level access to the serial port."""

    def __init__(self, port: str, baudrate: int = 115200, timeout: float = 0.5, verbose: bool = False):
        self.serial = serial.Serial(port, baudrate, timeout=0.02)
        self.timeout = timeout
        self.verbose = verbose
        self.pushed: list[Frame] = []   # requests the device sent on its own (passthrough messages)
        self.buffer = bytearray()       # bytes received but not parsed yet

    def close(self) -> None:
        self.serial.close()

    def __enter__(self) -> "Link":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def flush(self) -> None:
        time.sleep(0.05)
        self.serial.reset_input_buffer()
        self.buffer.clear()

    def write(self, data: bytes) -> None:
        if self.verbose:
            print(f"    -> {data.hex(' ')}")
        self.serial.write(data)
        self.serial.flush()

    def read_frame(self, timeout: Optional[float] = None) -> Optional[Frame]:
        """Read the next valid frame. Garbage and frames with a bad checksum are skipped."""
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        buffer = self.buffer

        while time.monotonic() < deadline:
            buffer += self.serial.read(64)

            while True:
                start = buffer.find(bytes([SYNC]))
                if start < 0:
                    buffer.clear()
                    break
                del buffer[:start]
                if len(buffer) < HEADER_SIZE:
                    break

                size = HEADER_SIZE + buffer[5] + 1
                if buffer[1] != VERSION or buffer[2] not in (REQUEST, ANSWER):
                    del buffer[0]
                    continue
                if len(buffer) < size:
                    break

                raw = bytes(buffer[:size])
                if crc8(raw[:-1]) != raw[-1]:
                    del buffer[0]
                    continue

                del buffer[:size]
                frame = Frame(raw[2], struct.unpack('>H', raw[3:5])[0], raw[HEADER_SIZE:-1])
                if self.verbose:
                    print(f"    <- {raw.hex(' ')}   {frame}")
                return frame

        return None

    def exchange(self, frame: Frame) -> Optional[Frame]:
        """Send a request and wait for its answer. Requests sent by the device meanwhile
        (user messages passed through from the radio) are acknowledged and stored."""
        self.flush()
        self.write(frame.encode())
        return self.wait_answer()

    def wait_answer(self) -> Optional[Frame]:
        deadline = time.monotonic() + self.timeout
        while (remaining := deadline - time.monotonic()) > 0:
            answer = self.read_frame(remaining)
            if answer is None:
                return None
            if answer.direction == ANSWER:
                return answer
            self.acknowledge(answer)
        return None

    def acknowledge(self, pushed: Frame) -> None:
        self.pushed.append(pushed)
        self.write(Frame(ANSWER, CMD_OK, struct.pack('>H', pushed.command)).encode())

    def require_answer(self, frame: Frame) -> Frame:
        """Like exchange(), but raises LinkError instead of returning None."""
        answer = self.exchange(frame)
        if answer is None:
            raise LinkError(f"no answer to {command_name(frame.command)}")
        if answer.command in (CMD_ERROR, CMD_UNKNOWN, CMD_UNIMPLEMENTED, CMD_OVERLOAD):
            raise LinkError(f"device answered {command_name(answer.command)} to {command_name(frame.command)}")
        return answer

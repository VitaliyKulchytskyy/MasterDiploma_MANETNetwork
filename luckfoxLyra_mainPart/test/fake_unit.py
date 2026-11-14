#!/usr/bin/env python3
"""
Simulated emergency unit (RP2040) for testing merld without hardware.

Opens a pseudo-terminal, links it to the given path and answers MERL requests the way
emergencyRadioLink/src/protocol/merl_handler.c does. Every --chat seconds it forwards a chat
message "received over ERL" (USER_MESSAGE_SEND with address = receiver << 16 | sender, like
merl_handler_forward_user_message), alternating broadcast and direct; every --push seconds it
sends a raw USER_MESSAGE_PASSTHROUGH. It checks that the master acknowledges both.

    ./fake_unit.py /tmp/ttyFAKE
    merld -d /tmp/ttyFAKE ...
"""

import argparse
import math
import os
import random
import struct
import sys
import time
import tty

# merl_link.py (wire format, CRC8) comes from the firmware's test tools; it imports pyserial,
# which a pty does not need
sys.modules.setdefault('serial', type(sys)('serial'))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../emergencyRadioLink/test'))
from merl_link import (ANSWER, CMD_COMPASS_GET, CMD_COMPASS_SET, CMD_GPS_STATUS, CMD_DEVICE_FEATURES, CMD_DEVICE_VERSION, CMD_ERROR, CMD_NETWORK_GET, CMD_NETWORK_SET, CMD_OK, CMD_OK_BODY,  # noqa: E402
                       CMD_PEERS_CLEAR, CMD_PEERS_GET, CMD_POSITION_GET, CMD_POSITION_SET, CMD_UNIMPLEMENTED, CMD_UNKNOWN,
                       CMD_USER_MESSAGE_PASSTHROUGH, CMD_USER_MESSAGE_SEND, HEADER_SIZE, REQUEST, Frame,
                       command_name, crc8, CMD_RADIO_STATS_GET, CMD_LINK_STATS_GET)


def make_peers(count: int, center, own_address: int):
    """The device itself plus `count` peers scattered up to ~2.5 km around center (lat, lon),
    RSSI falling with distance like a LoRa link; the last peer has no GPS fix yet."""
    rng = random.Random(7)
    lat0, lon0 = center
    peers = [[own_address, 0, 120, int(lon0 * 1e6), int(lat0 * 1e6), 0]]
    for i in range(count):
        dist = rng.uniform(150, 2500)                       # metres
        angle = rng.uniform(0, 2 * math.pi)
        dlat = dist * math.cos(angle) / 111320
        dlon = dist * math.sin(angle) / (111320 * math.cos(math.radians(lat0)))
        rssi = int(max(-125, min(-45, -60 - 25 * math.log10(dist / 100) + rng.uniform(-6, 6))))
        fix = i < count - 1
        peers.append([own_address + 2 + i, rssi, rng.randint(60, 20000),
                      int((lon0 + dlon) * 1e6) if fix else 0, int((lat0 + dlat) * 1e6) if fix else 0, 0])
    return peers


def peer_compass(i: int, count: int):
    """A peer's broadcast compass: each faces its own direction; the last one has no reading (0s)."""
    if i == count - 1:
        return 0, 0, 0
    a = math.radians(40 + 110 * i)
    return int(1800 * math.cos(a)), int(1800 * math.sin(a)), -450


class Unit:
    def __init__(self, peers: int = 1, center=(50.4501, 30.5234), old_firmware: bool = False,
                 features=('gps', 'compass', 'link-stats')):
        self.network = (0xC701, 0xFFE0)
        self.old_firmware = old_firmware    # no DEVICE_VERSION / DEVICE_FEATURES (answers UNKNOWN)
        self.compass_set = None             # (x, y, z) from COMPASS_SET, else a slowly turning unit
        self.features = set(features)       # compiled-in: 'gps', 'compass' (DEVICE_FEATURES)
        self.center = center
        self.position = None           # (time, lon, lat) or None: no fix
        if peers > 1:
            if 'gps' in self.features:
                self.position = (45296000, int(center[1] * 1e6), int(center[0] * 1e6))
            self.peers = make_peers(peers, center, self.network[0])
        else:
            self.peers = [
                [0xC701, 0, 120, 0, 0, 0],                  # the device itself, rssi 0
                [0xC70A, -87, 3600, 30523456, 50450100, 0],
            ]
        self.sent = []
        self.boot = time.time()

    def compass(self):
        """Raw QMC5883-like axes: set with COMPASS_SET, else the unit turns 10 degrees a second."""
        if self.compass_set:
            return self.compass_set
        if 'compass' not in self.features:
            return 0, 0, 0
        a = math.radians((time.time() * 10) % 360)
        return int(2000 * math.cos(a)), int(2000 * math.sin(a)), -500

    def tick(self):
        """Peers keep sending: uptimes grow, RSSI wobbles a little."""
        for peer in self.peers:
            peer[2] += 1
            if peer[1] != 0:
                peer[1] = max(-127, min(-30, peer[1] + random.randint(-2, 2)))

    def answer(self, frame: Frame) -> Frame:
        cmd, p = frame.command, frame.payload
        status = lambda code: Frame(ANSWER, code, struct.pack('>H', cmd))  # noqa: E731
        if cmd == CMD_NETWORK_GET:
            return Frame(ANSWER, CMD_OK_BODY, struct.pack('>HH', *self.network))
        if cmd == CMD_NETWORK_SET:
            if len(p) != 4:
                return status(CMD_ERROR)
            self.network = struct.unpack('>HH', p)
            return status(CMD_OK)
        if cmd == CMD_PEERS_GET:
            self.tick()
            # peer_t: 26 bytes with the compass axes (a peer's rides on its PACKET_TYPE_POSITION, our
            # own entry our current reading); old firmware: 20 bytes, no compass
            body = b''
            for i, peer in enumerate(self.peers):
                entry = struct.pack('<IbIii', *peer[:5]) + peer[5].to_bytes(3, 'little')
                if not self.old_firmware:
                    entry += struct.pack('<hhh', *(self.compass() if i == 0 else peer_compass(i, len(self.peers))))
                body += entry
            return Frame(ANSWER, CMD_OK_BODY, body)
        if cmd == CMD_PEERS_CLEAR:
            self.peers = []
            return status(CMD_OK)
        if cmd == CMD_POSITION_GET:
            if self.position is None:
                return status(CMD_ERROR)
            return Frame(ANSWER, CMD_OK_BODY, struct.pack('>Iii', *self.position))
        if cmd == CMD_POSITION_SET:
            if len(p) != 8:
                return status(CMD_ERROR)
            self.position = (0,) + struct.unpack('>ii', p)
            return status(CMD_OK)
        if cmd == CMD_GPS_STATUS and not self.old_firmware:
            if 'gps' not in self.features:
                return status(CMD_UNIMPLEMENTED)
            # a receiver slowly finding satellites: [fix][used][in view][hdop x100 BE]
            used = int(time.time() / 10) % 9
            return Frame(ANSWER, CMD_OK_BODY, struct.pack('>BBBH', 1 if used >= 4 else 0, used, 12, 90 + 40 * (8 - used) if used else 0))
        if cmd == CMD_DEVICE_VERSION and not self.old_firmware:
            # major, minor, build number (BE), __DATE__ and __TIME__ without the NUL
            return Frame(ANSWER, CMD_OK_BODY, struct.pack('>BBI', 1, 0, 42) + b'Sep  7 2026' + b'14:23:01')
        if cmd == CMD_DEVICE_FEATURES and not self.old_firmware:
            return Frame(ANSWER, CMD_OK_BODY, bytes(i for i, f in ((0x00, 'gps'), (0x01, 'compass'), (0x02, 'link-stats'))
                                                    if f in self.features))
        if cmd in (CMD_RADIO_STATS_GET, CMD_LINK_STATS_GET) and 'link-stats' not in self.features:
            return status(CMD_UNKNOWN)
        if cmd == CMD_RADIO_STATS_GET:
            # counters growing with time since "boot": a heartbeat every 5 s from each peer, a few bad ones
            t = time.time() - self.boot
            peers = max(1, len(self.peers) - 1)
            rx = int(t / 5 * peers * 1.3)
            return Frame(ANSWER, CMD_OK_BODY, struct.pack('>11Ibbb', int(t * 1000), rx, rx // 40, rx // 90, 0, 0,
                                                          int(t / 5 * 1.3), 0, int(t / 5 * 1.3 * 180), rx // 30, 0,
                                                          -112 + random.randint(-2, 2), -117, -113))
        if cmd == CMD_LINK_STATS_GET:
            t = time.time() - self.boot
            body = b''
            for peer in self.peers[1:8]:
                hb = int(t / 5) + 1
                missed = hb // 12                           # about one heartbeat in twelve lost
                rx = int((hb - missed) * 1.3)
                snr = max(-20.0, min(10.0, (peer[1] + 111) * 0.9))   # SNR follows the peer's RSSI
                body += struct.pack('>HIIIbbiIii', peer[0], rx, hb - missed, missed, peer[1], int(snr * 4),
                                    random.randint(-900, 900), random.randint(0, 5000), peer[1] * rx, int(snr * 4) * rx)
            return Frame(ANSWER, CMD_OK_BODY, body)
        # built without a compass: a reading set over MERL is still kept (and broadcast), like a
        # position set without GPS (POSITION_SET above); until one is set, gets answer ERROR
        if cmd == CMD_COMPASS_GET and 'compass' not in self.features and not self.compass_set:
            return status(CMD_ERROR)
        if cmd == CMD_COMPASS_GET and not self.old_firmware:
            return Frame(ANSWER, CMD_OK_BODY, struct.pack('>hhh', *self.compass()))
        if cmd == CMD_COMPASS_SET and not self.old_firmware:
            if len(p) != 6:
                return status(CMD_ERROR)
            self.compass_set = struct.unpack('>hhh', p)
            return status(CMD_OK)
        if cmd == CMD_USER_MESSAGE_SEND:
            if len(p) < 6 or not p.endswith(b'\r\n'):
                return status(CMD_ERROR)
            self.sent.append((struct.unpack('>I', p[:4])[0], p[4:]))
            return status(CMD_OK)
        return status(CMD_UNKNOWN)


def frames(fd, buffer: bytearray):
    """Parse complete frames out of buffer (new bytes appended from fd)."""
    try:
        buffer += os.read(fd, 1024)
    except BlockingIOError:
        pass
    while True:
        start = buffer.find(b'*')
        if start < 0:
            buffer.clear()
            return
        del buffer[:start]
        if len(buffer) < HEADER_SIZE:
            return
        size = HEADER_SIZE + buffer[5] + 1
        if len(buffer) < size:
            return
        raw = bytes(buffer[:size])
        if crc8(raw[:-1]) != raw[-1]:
            del buffer[0]
            continue
        del buffer[:size]
        yield Frame(raw[2], struct.unpack('>H', raw[3:5])[0], raw[HEADER_SIZE:-1])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('link', help="path of the symlink to create for the pty")
    parser.add_argument('--push', type=float, default=0, help="send a passthrough message every N seconds, 0 = never")
    parser.add_argument('--chat', type=float, default=0, help="forward a chat message every N seconds, 0 = never")
    parser.add_argument('--peers', type=int, default=1,
                        help="simulate N peers with GPS positions around --center (default: 1 fixed peer)")
    parser.add_argument('--features', default='gps,compass,link-stats',
                        help="compiled-in features, comma-separated: gps, compass, link-stats (empty: none)")
    parser.add_argument('--old-firmware', action='store_true',
                        help="answer DEVICE_VERSION / DEVICE_FEATURES with UNKNOWN, like firmware before them")
    parser.add_argument('--center', default='50.4501,30.5234', help="lat,lon of the simulated device")
    parser.add_argument('--sos', action='store_true',
                        help="after 3 s: an SOS from a listed peer, and one with coordinates from a peer not in the table")
    args = parser.parse_args()

    master, slave = os.openpty()
    tty.setraw(slave)
    os.set_blocking(master, False)
    if os.path.lexists(args.link):
        os.unlink(args.link)
    os.symlink(os.ttyname(slave), args.link)
    print(f"fake unit on {os.ttyname(slave)} -> {args.link}", flush=True)

    unit = Unit(args.peers, tuple(float(v) for v in args.center.split(',')), args.old_firmware,
                [f for f in args.features.split(',') if f])
    buffer = bytearray()
    next_push = time.monotonic() + args.push if args.push else None
    next_chat = time.monotonic() + args.chat if args.chat else None
    next_sos = time.monotonic() + 3 if args.sos else None
    pushes = acks = chats = chat_acks = 0
    try:
        while True:
            for frame in frames(master, buffer):
                if frame.direction == REQUEST:
                    reply = unit.answer(frame)
                    os.write(master, reply.encode())
                    print(f"{command_name(frame.command)} {frame.payload.hex()} -> {command_name(reply.command)}",
                          flush=True)
                elif frame.command == CMD_OK and frame.payload == struct.pack('>H', CMD_USER_MESSAGE_PASSTHROUGH):
                    acks += 1
                    print(f"passthrough acknowledged ({acks}/{pushes})", flush=True)
                elif frame.command == CMD_OK and frame.payload == struct.pack('>H', CMD_USER_MESSAGE_SEND):
                    chat_acks += 1
                    print(f"chat message acknowledged ({chat_acks}/{chats})", flush=True)
                else:
                    print(f"unexpected answer {frame}", flush=True)
            if next_chat and time.monotonic() >= next_chat:
                chats += 1
                own, mask = unit.network
                others = [p[0] for p in unit.peers if p[0] != own] or [own + 1]
                sender = others[chats % len(others)]
                broadcast = chats % 2 == 1
                receiver = (own | (~mask & 0xFFFF)) if broadcast else own
                text = (f"Hello everyone from 0x{sender:04X} #{chats}" if broadcast
                        else f"Hi 0x{own:04X}, direct from 0x{sender:04X} #{chats} \u2014 \"ok?\"").encode()
                payload = struct.pack('>I', receiver << 16 | sender) + text + b'\r\n'
                os.write(master, Frame(REQUEST, CMD_USER_MESSAGE_SEND, payload).encode())
                next_chat += args.chat
            if next_sos and time.monotonic() >= next_sos:
                next_sos = None
                own, mask = unit.network
                broadcast = own | (~mask & 0xFFFF)
                listed = [p for p in unit.peers if p[0] != own]
                lat, lon = unit.center
                sos = []
                if listed:      # a peer with a position in the table: "SOS!" without coordinates
                    sos.append((listed[-1][0], "SOS! Need help"))
                # not in the table (out of range): only its message says where it is
                sos.append(((own & mask) | 0x1E, f"SOS! Need help at {lat - 0.004:.5f}, {lon - 0.006:.5f}, heading 200\u00b0 SW"))
                for sender, text in sos:
                    payload = struct.pack('>I', broadcast << 16 | sender) + text.encode() + b'\r\n'
                    os.write(master, Frame(REQUEST, CMD_USER_MESSAGE_SEND, payload).encode())
                    time.sleep(0.3)
            if next_push and time.monotonic() >= next_push:
                pushes += 1
                text = f"Radio message {pushes}: \"hi\"\r\n".encode()
                os.write(master, Frame(REQUEST, CMD_USER_MESSAGE_PASSTHROUGH, text).encode())
                next_push += args.push
            time.sleep(0.005)
    except KeyboardInterrupt:
        pass
    finally:
        os.unlink(args.link)
    return 0


if __name__ == '__main__':
    sys.exit(main())

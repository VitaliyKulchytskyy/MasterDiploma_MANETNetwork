"""fake_unit.py with the scenario's peers, a steady GPS fix and compass."""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, '/home/karlenko/dev/diploma/luckfoxLyra_mainPart/test')
import fake_unit  # noqa: E402
import scenario as sc  # noqa: E402


def make_peers(count, center, own):
    lat0, lon0 = sc.CENTER
    peers = [[sc.OWN, 0, 6300, int(lon0 * 1e6), int(lat0 * 1e6), 0]]
    for addr, dist, bearing, up, _h, fix in sc.PEERS:
        lat, lon = sc.offset(dist, bearing)
        peers.append([addr, sc.rssi(dist), up, int(lon * 1e6) if fix else 0, int(lat * 1e6) if fix else 0, 0])
    return peers


def peer_compass(i, count):
    h = sc.PEERS[i - 1][4] if 1 <= i <= len(sc.PEERS) else None
    return sc.raw_compass(h) if h is not None else (0, 0, 0)


class Unit(fake_unit.Unit):
    def __init__(self, *a, **k):
        super().__init__(*a, **k)
        self.network = (sc.OWN, 0xFFE0)
        self.compass_set = sc.raw_compass(35)       # this unit faces NE
        self.position = (45296000, int(sc.CENTER[1] * 1e6), int(sc.CENTER[0] * 1e6))

    def tick(self):
        for p in self.peers:
            p[2] += 1                               # uptimes grow, RSSI stays as the scenario says

    def answer(self, frame):
        if frame.command == fake_unit.CMD_GPS_STATUS:
            # 9 of 14 satellites in the fix, HDOP 0.9
            return fake_unit.Frame(fake_unit.ANSWER, fake_unit.CMD_OK_BODY, struct.pack('>BBBH', 1, 9, 14, 90))
        return super().answer(frame)


fake_unit.make_peers = make_peers
fake_unit.peer_compass = peer_compass
fake_unit.Unit = Unit
sys.exit(fake_unit.main())

"""Demo scenario for the thesis screenshots: a search in Sosnovyi Bir park, Cherkasy."""
import math

CENTER = (49.463889, 32.030278)         # lat, lon: Sosnovyi Bir, this node
OWN = 0xC701
BROADCAST = 0xC71F

# ERL (LoRa) peers: address, distance m, bearing deg, uptime s, heading deg (None: no compass),
# has GPS fix
PEERS = [
    (0xC702, 600, 320, 5400, 75, True),       # at the forest edge, north-west
    (0xC703, 2600, 255, 5100, 210, True),     # deep in the forest (sends the fresh SOS)
    (0xC705, 3100, 290, 4800, 300, True),     # deep in the forest (the earlier SOS)
    (0xC706, 1300, 205, 3900, None, True),    # south, by the road
    (0xC708, 4400, 270, 2100, 180, True),     # the far west of the forest
    (0xC709, 1900, 260, 900, None, False),    # just switched on, no GPS fix yet
]


def where(address):
    for addr, dist, bearing, *_ in PEERS:
        if addr == address:
            return offset(dist, bearing)


def offset(dist, bearing):
    lat0, lon0 = CENTER
    dlat = dist * math.cos(math.radians(bearing)) / 111320
    dlon = dist * math.sin(math.radians(bearing)) / (111320 * math.cos(math.radians(lat0)))
    return lat0 + dlat, lon0 + dlon


def rssi(dist):
    # log-distance path loss, like a LoRa link in a pine forest
    return int(round(-60 - 25 * math.log10(dist / 100)))


def raw_compass(heading):
    a = math.radians(heading)
    return int(1800 * math.cos(a)), int(1800 * math.sin(a)), -450

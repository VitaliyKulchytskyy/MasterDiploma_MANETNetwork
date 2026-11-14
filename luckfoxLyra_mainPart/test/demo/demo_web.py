"""web_sim.py with the scenario's node, mesh, internet and battery."""
import json
import os
import random
import sys
import time

sys.path.insert(0, '/home/karlenko/dev/diploma/luckfoxLyra_mainPart/test')
import web_sim  # noqa: E402

MACS = {2: 'c8:fe:0f:5e:56:60', 3: 'c8:fe:0f:5e:57:1a', 4: 'c8:fe:0f:5e:58:03'}


def samples():
    now = int(time.time())
    return {
        '/cgi-bin/status': {
            'hostname': 'luckfox', 'kernel': '6.1.99-erl', 'uptime': 6300, 'load': 0.34,
            'mem_total_kb': 89344, 'mem_avail_kb': 41250,
            'mesh': {'if': 'wlxc8fe0f5e5622', 'mode': 'IBSS', 'ssid': 'LyraMesh', 'channel': 6, 'freq': 2437,
                     'txpower_dbm': 10, 'ip': '10.10.0.1/24', 'bat0_mac': '9a:2a:7f:66:00:cd', 'gw_mode': 'server'},
            'neighbours': [
                {'mac': MACS[2], 'if': 'wlxc8fe0f5e5622', 'last_seen': 0.21},
                {'mac': MACS[3], 'if': 'wlxc8fe0f5e5622', 'last_seen': 0.64},
            ],
            'originators': [
                {'mac': MACS[2], 'ip': '10.10.0.2', 'last_seen': 0.21, 'tq': 239, 'nexthop': MACS[2], 'direct': True, 'if': 'wlxc8fe0f5e5622'},
                {'mac': MACS[3], 'ip': '10.10.0.3', 'last_seen': 0.64, 'tq': 187, 'nexthop': MACS[3], 'direct': True, 'if': 'wlxc8fe0f5e5622'},
                {'mac': MACS[4], 'ip': '10.10.0.4', 'last_seen': 1.12, 'tq': 121, 'nexthop': MACS[2], 'direct': False, 'if': 'wlxc8fe0f5e5622'},
            ],
            'gateways': [],
        },
        '/cgi-bin/internet': {
            'role': 'server', 'online': True, 'latency_ms': 48, 'dns': True, 'checked': now - 12,
            'uplink': {'dev': 'usb1', 'kind': 'phone-eth', 'address': '192.168.123.100/24', 'via': '192.168.123.129'},
            'gateway': None, 'announced': True, 'nameservers': ['192.168.123.129'],
            'clock': {'synced': True, 'server': 'ntp.ubuntu.com'}, 'updated': now,
        },
        '/cgi-bin/power': {
            'ok': True, 'bus_v': 7.94, 'shunt_mv': 21.3, 'current_ma': 213.0, 'avg_current_ma': 208.6,
            'power_mw': 1691.2, 'bus': '/dev/i2c-1', 'address': '0x40', 'address_config': 'auto',
            'shunt_ohm': 0.1, 'max_current_a': 0.8, 'average_samples': 64, 'interval_ms': 1000,
            'cells': 2, 'capacity_mah': 2600, 'updated': now, 'service': 'active', 'now': now,
        },
        '/cgi-bin/mesh-compass': {
            'ok': True, 'updated': now, 'nodes': [
                {'ip': '10.10.0.2', 'address': '0xC702', 'heading': 75.0, 'cardinal': 'E'},
                {'ip': '10.10.0.3', 'address': '0xC703', 'heading': 210.0, 'cardinal': 'SW'},
            ],
        },
    }


CHANNELS = list(range(1, 14)) + [36, 40, 44, 48, 52, 56, 60, 64] + list(range(100, 145, 4)) + [149, 153, 157, 161, 165]

# Wi-Fi networks around the node at the park's edge: (ssid, channel, dBm)
NETWORKS = [
    ("Kyivstar_5A1C", 1, -66), ("TP-Link_7F2E", 1, -81), ("", 3, -90),
    ("Sosnovyi_Bir_Guest", 6, -71), ("DIRECT-HP-Print", 6, -87), ("Lanet_2G", 9, -79),
    ("ASUS_B8", 11, -62), ("Vodafone_Home", 11, -85),
    ("Kyivstar_5A1C_5G", 36, -77), ("TP-Link_7F2E_5G", 44, -86), ("Lanet_5G", 149, -89),
]


def mhz(c):
    return 2484 if c == 14 else 2407 + 5 * c if c < 14 else 5000 + 5 * c


def sweeps(now):
    """25 sweeps, one every 30 s up to now; each network heard most of the time, signals wobble."""
    rng = random.Random(3)
    out = []
    for k in range(25):
        t = now - (24 - k) * 30
        bss = [{"bssid": "02:ca:fe:ca:ca:40", "freq": 2437, "signal": 0, "ssid": "LyraMesh", "ibss": True}]
        for i, (ssid, ch, dbm) in enumerate(NETWORKS):
            if rng.random() < 0.9:
                bss.append({"bssid": f"a4:2b:b0:{i:02x}:{ch:02x}:1c", "freq": mhz(ch),
                            "signal": round(dbm + rng.gauss(0, 3)), "ssid": ssid, "ibss": False})
        out.append({"t": t, "ms": 3100 + rng.randint(0, 120), "bss": bss})
    return out


# A real sweep history and channel list saved from a board (curl its /cgi-bin/scan and
# /cgi-bin/radio into board_scan.json / board_radio.json); without them, the made-up sweeps above
HERE = os.path.dirname(os.path.abspath(__file__))


def board_file(name):
    try:
        with open(os.path.join(HERE, name)) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def board_sweeps(now):
    """The board's own sweeps, moved in time so the newest is now (the board's clock may be off)."""
    d = board_file("board_scan.json")
    if not d or not d.get("sweeps"):
        return None
    shift = now - d["sweeps"][-1]["t"]
    return [dict(s, t=s["t"] + shift) for s in d["sweeps"]]


def samples_all():
    d = samples()
    now = int(time.time())
    board_radio = board_file("board_radio.json") or {}
    d['/cgi-bin/radio'] = {"ok": True, "interface": "wlxc8fe0f5e5622", "channel": 6, "freq": 2437, "country": "PA",
                           "txpower_cap_dbm": None, "txpower_max_dbm": 20,
                           "channels": board_radio.get("channels", CHANNELS)}
    d['/cgi-bin/scan'] = {"ok": True, "sweeps": board_sweeps(now) or sweeps(now)}
    d['/cgi-bin/exp'] = exp_status(now)
    return d


# Test page: the DEMO link-experiment runs (exp_demo.py, a propagation model, not measurements) as kept runs,
# and one more at 900 m in progress
EXP_DEMO = '/home/karlenko/dev/diploma/experiment_demo/erl-exp-DEMO-runs.json'


def exp_status(now):
    import exp_demo
    E = exp_demo.E
    try:
        runs = json.load(open(EXP_DEMO))['runs']
    except (OSError, ValueError):
        runs = []
    shown = [r for r in runs if r['distance_m_manual'] <= 700]          # the walk so far
    cur = exp_demo.run(900, 'low', now - 118)                           # started two minutes ago
    for k in ('replies', 'lost', 'samples'):
        cur['wifi'][k] = [x for x in cur['wifi'][k] if (x[-1] if isinstance(x, list) else x['t']) <= now]
    cur['erl']['sent'] = [x for x in cur['erl']['sent'] if x[1] <= now]
    cur['erl']['acks'] = {k: v for k, v in cur['erl']['acks'].items() if v <= now}
    cur['erl']['rssi'] = [x for x in cur['erl']['rssi'] if x[0] <= now]
    cur['positions'] = [p for p in cur['positions'] if p['t'] <= now]
    cur['finished'] = None
    here = cur['positions'][-1]['own'] if cur['positions'] else None
    return {
        'ok': True, 'running': True, 'now': now, 'responder': 'active', 'erl': '0xC701',
        'current': {'id': cur['id'], 'label': 'P10', 'mode': 'low', 'links': ['wifi', 'erl'], 'started': cur['started'],
                    'updated': now, 'finished': None, 'params': cur['params'], 'summary': E.summarize(cur),
                    'wifi_sent': 0, 'wifi_error': cur['wifi']['error'], 'erl_sent': len(cur['erl']['sent']),
                    'erl_error': None, 'positions': cur['positions'][-1:]},
        'erl_peers': [{'address': '0xC702', 'rssi': round(exp_demo.lora_rssi(900)), 'uptime': 5400,
                       'latitude': exp_demo.sc.CENTER[0], 'longitude': exp_demo.sc.CENTER[1]}],
        'mesh_peers': [], 'position': here,
        'runs': [dict(id=r['id'], **r['summary']) for r in shown],
    }


web_sim.samples = samples_all
sys.argv = [sys.argv[0]] + sys.argv[1:]
web_sim.main()

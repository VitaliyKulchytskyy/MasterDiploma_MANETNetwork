#!/usr/bin/env python3
"""DEMO link-experiment data from a propagation model — NOT measurements.

Produces runs in erl-exp's format (for the Test page screenshot and for trying exp_analyze.py) along a walk
from the fixed node at Sosnovyi Bir into the park. The model, without amplifiers:

  Wi-Fi 2.4 GHz  20 dBm, 2 dBi antennas: -16 dBm at 1 m, path-loss exponent 2.6 (park, 1.5 m high);
                 loss 50 % at 500 m (theoretical range), 10 % at ~450 m; the peer drops out past ~600 m
  LoRa 433 MHz   SF11, 500 kHz, 17 dBm, 2 dBi: -4 dBm at 1 m, exponent 4.0 (near the ground, forest);
                 RSSI reaches the SF11 sensitivity (~-128.5 dBm, SNR limit -17.5 dB) at 1.3 km (theoretical
                 range); the echo needs both directions

  exp_demo.py out-dir
writes erl-exp-DEMO-runs.json (with "demo": true) and erl-exp-DEMO.csv (the node's CSV format).
"""
import csv, importlib.util, json, math, os, random, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scenario as sc  # noqa: E402

spec = importlib.util.spec_from_file_location(
    'erl_exp', '/home/karlenko/dev/diploma/batman-adv-lyra/erl-exp.py')
E = importlib.util.module_from_spec(spec); spec.loader.exec_module(E)

random.seed(20260928)
BEARING = 250                        # the walk: from the park's edge west-south-west into the forest
POINTS = [25, 100, 200, 300, 400, 450, 500, 550, 700, 900, 1100, 1200, 1300, 1400, 1500]
T0 = 1790589600                      # 28.09.2026 13:00 local (the afternoon of the walk)

WIFI_P1M, WIFI_N, WIFI_D50, WIFI_W = -16.0, 2.6, 500.0, 22.0
LORA_P1M, LORA_N = -4.0, 4.0
LORA_NOISE = -174 + 10 * math.log10(500e3) + 6        # thermal noise in 500 kHz, noise figure 6 dB
SNR_LIMIT_SF11 = -17.5


def wifi_rssi(d):
    return WIFI_P1M - 10 * WIFI_N * math.log10(d)


def wifi_loss(d, high):
    p = 1 / (1 + math.exp((WIFI_D50 - d) / WIFI_W)) + 0.004
    return min(1.0, p + (0.012 if high else 0.0))     # more contention at 20 pings/s


def lora_rssi(d):
    return LORA_P1M - 10 * LORA_N * math.log10(d)


def lora_one_way(d, high):
    margin = lora_rssi(d) + random.gauss(0, 1.5) - LORA_NOISE - SNR_LIMIT_SF11
    p = 1 / (1 + math.exp(-margin / 0.9))
    return p * (0.96 if high else 0.995)               # collisions with the other node's traffic


def run(d, mode, t):
    high = mode == 'high'
    prm = E.MODES[mode]
    lat, lon = sc.offset(d, BEARING)
    rid = '20260928-%s' % __import__('time').strftime('%H%M%S', __import__('time').localtime(t))
    rec = {'id': rid, 'label': 'P%d' % (POINTS.index(d) + 1), 'mode': mode, 'links': ['wifi', 'erl'], 'params': prm,
           'distance_m_manual': float(d), 'started': t, 'finished': None, 'stopped': False,
           'node': {'hostname': 'luckfox', 'erl': '0xC701', 'mesh_if': 'wlxc8fe0f5e5622', 'channel': 6, 'freq': 2437},
           'positions': [], 'wifi': None, 'erl': None, 'demo': True}
    dur = max(prm['wifi_count'] * prm['wifi_interval'], prm['erl_count'] * prm['erl_interval']) + 8
    # Wi-Fi
    if d <= 600:
        replies, lost, samples = [], [], []
        pl = wifi_loss(d, high)
        base = 1.6 + 30 * pl + (0.9 if high else 0)          # retransmissions near the edge, queueing at 20/s
        for seq in range(1, prm['wifi_count'] + 1):
            ts = t + seq * prm['wifi_interval']
            if random.random() < pl:
                lost.append([seq, ts])
            else:
                replies.append([seq, round(max(0.6, random.gauss(base, 0.25 * base)), 3), ts])
        for k in range(int(prm['wifi_count'] * prm['wifi_interval'])):
            rssi = round(wifi_rssi(d) + random.gauss(0, 2.2))
            tq = max(0, min(255, round(255 * (1 - pl) ** 2 + random.gauss(0, 6))))
            rate = 65.0 if rssi > -65 else 39.0 if rssi > -72 else 19.5 if rssi > -78 else 6.5 if rssi > -84 else 1.0
            samples.append({'t': t + k, 'station': {'signal': rssi, 'signal_avg': rssi, 'tx_bitrate': rate,
                                                    'rx_bitrate': rate, 'tx_retries': int(40 * pl * k),
                                                    'tx_failed': int(4 * pl * k)},
                            'noise': None, 'tq': tq, 'last_seen': round(random.uniform(0.05, 0.3 + 2 * pl), 2)})
        rec['wifi'] = {'peer_mac': 'c8:fe:0f:5e:56:60', 'sent': prm['wifi_count'], 'replies': replies, 'lost': lost,
                       'samples': samples, 'error': None,
                       'summary': {'transmitted': prm['wifi_count'], 'received': len(replies)}}
    else:
        rec['wifi'] = {'peer_mac': None, 'sent': 0, 'replies': [], 'lost': [], 'samples': [], 'summary': None,
                       'error': 'no mesh peer in range'}
    # ERL
    sent, acks, rssis = [], {}, []
    for seq in range(1, prm['erl_count'] + 1):
        ts = t + (seq - 1) * prm['erl_interval']
        sent.append([seq, ts, True])
        if random.random() < lora_one_way(d, high) and random.random() < lora_one_way(d, high):
            acks[str(seq)] = ts + max(0.5, random.gauss(1.15 + (0.45 if high else 0), 0.28))
    for k in range(0, int(dur), 5):                        # merld refreshes the peer table every 5 s
        rssis.append([t + k, round(lora_rssi(d) + random.gauss(0, 2.0))])
    rec['erl'] = {'peer': '0xC702', 'sent': sent, 'acks': acks, 'rssi': rssis, 'send_errors': 0, 'error': None}
    for k in range(0, int(dur), 5):
        rec['positions'].append({'t': t + k, 'own': {'latitude': lat + random.gauss(0, 2e-5),
                                                     'longitude': lon + random.gauss(0, 3e-5), 'source': 'unit'},
                                 'peer': {'latitude': sc.CENTER[0], 'longitude': sc.CENTER[1]}})
        rec['positions'][-1]['distance_m'] = E.haversine_m(rec['positions'][-1]['own'], rec['positions'][-1]['peer'])
    rec['finished'] = t + dur
    rec['summary'] = E.summarize(rec)
    return rec


def build():
    runs, t = [], T0
    for d in POINTS:
        for mode in ('low', 'high'):
            r = run(d, mode, t)
            runs.append(r)
            t = r['finished'] + 60                         # a minute to note the point and start again
        t += 240                                           # walking to the next point
    return runs


if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(out, exist_ok=True)
    runs = build()
    json.dump({'node': 'luckfox', 'demo': True,
               'model': 'DEMO data from a propagation model, not measurements (see exp_demo.py)', 'runs': runs},
              open(os.path.join(out, 'erl-exp-DEMO-runs.json'), 'w'), ensure_ascii=False)
    with open(os.path.join(out, 'erl-exp-DEMO.csv'), 'w', newline='') as f:
        csv.writer(f).writerows(E.csv_rows(runs))
    print(len(runs), 'demo runs ->', out)

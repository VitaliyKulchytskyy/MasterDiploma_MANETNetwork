#!/usr/bin/env python3
"""Link experiments between two nodes: how the Wi-Fi mesh and the ERL (LoRa) link behave at a distance.

One node stays put (it runs `erl-exp responder`, enabled as a service), the other is carried to test points
and runs one measurement per point from the Test page (cgi-bin/exp):

  Wi-Fi mesh  batctl ping to the peer's originator (loss, RTT per packet), and every second iw station
              (signal, bitrates, retries), iw survey (noise, if the driver reports it) and batctl o (TQ)
  ERL         numbered test messages "EXP <run> <seq>/<n>" to the peer's ERL address; the peer's responder
              answers each with "EXP-ACK <run> <seq>": delivery both ways and the round trip on this node's
              clock (no clock sync needed). Every second: the peer's RSSI from merld's peer table and both
              positions (GPS distance)

Load modes (thesis table 2.7):
  low   Wi-Fi 100 pings every 1 s        ERL 10 messages every 30 s
  high  Wi-Fi 1000 pings every 50 ms     ERL 30 messages every 2 s

Usage:
  erl-exp run --label P1 [--distance-m 250] [--mode low|high] [--links wifi,erl] [--peer-mac M] [--peer-erl A]
  erl-exp stop | status | runs | csv | rx | delete <run-id> | time | responder

Runs are kept as JSON in /var/lib/erl-exp (on the SD card, synced after every run); the one in progress is
mirrored to /run/erl-exp/current.json every second for the page.
"""
import argparse, csv, io, json, math, os, re, signal, statistics, subprocess, sys, threading, time

DATA = '/var/lib/erl-exp'
RUN = '/run/erl-exp'
CURRENT = RUN + '/current.json'
PIDFILE = RUN + '/run.pid'
RX_LOG = DATA + '/rx.jsonl'           # responder: every test message it received (one-way delivery)
STATE = '/run/merl/state.json'

MODES = {
    'low':  {'wifi_count': 100,  'wifi_interval': 1.0,  'erl_count': 10, 'erl_interval': 30.0},
    'high': {'wifi_count': 1000, 'wifi_interval': 0.05, 'erl_count': 30, 'erl_interval': 2.0},
}
FRESH_S = 3.0                         # Wi-Fi sample counts only if the peer's last OGM is this recent
ACK_WAIT_S = 20                       # after the last ERL message: time for the last answers
EXP_RE = re.compile(r'^EXP (\S+) (\d+)/(\d+)$')
ACK_RE = re.compile(r'^EXP-ACK (\S+) (\d+)$')

stop = threading.Event()


def sh(*cmd, timeout=10):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout).stdout
    except (subprocess.TimeoutExpired, OSError):
        return ''


def merlctl(*args, timeout=5):
    out = sh('merlctl', *args, timeout=timeout)
    try:
        return json.loads(out)
    except ValueError:
        return {'ok': False, 'error': out.strip()[:200]}


def mesh_if():
    for line in sh('batctl', 'if').splitlines():
        return line.split(':')[0].strip()
    return None


def originators():
    """batctl o -H: [{mac, last_seen, tq, nexthop}] of the nodes the mesh knows"""
    out = []
    for line in sh('batctl', 'o', '-H').splitlines():
        m = re.match(r'\s*\*?\s*([0-9a-f:]{17})\s+([\d.]+)s\s+\(\s*(\d+)\)\s+([0-9a-f:]{17})', line)
        if m:
            out.append({'mac': m.group(1), 'last_seen': float(m.group(2)), 'tq': int(m.group(3)),
                        'nexthop': m.group(4)})
    return out


def read_state():
    try:
        with open(STATE) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def own_erl(state):
    return (state.get('network') or {}).get('address')


def erl_peers(state):
    me = own_erl(state)
    return [p for p in (state.get('peers') or []) if p.get('address') != me]


def haversine_m(a, b):
    if not a or not b or a.get('latitude') is None or b.get('latitude') is None:
        return None
    r = 6371000.0
    p1, p2 = math.radians(a['latitude']), math.radians(b['latitude'])
    dp, dl = p2 - p1, math.radians(b['longitude'] - a['longitude'])
    h = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(h))


def station(iface, mac):
    """iw station get: signal (dBm), signal avg, tx/rx bitrate (Mbit/s), tx retries/failed"""
    out = sh('iw', 'dev', iface, 'station', 'get', mac)
    if not out:
        return None
    s = {}
    for key, pat in (('signal', r'\tsignal:\s*(-?\d+)'), ('signal_avg', r'\tsignal avg:\s*(-?\d+)'),
                     ('tx_bitrate', r'\ttx bitrate:\s*([\d.]+)'), ('rx_bitrate', r'\trx bitrate:\s*([\d.]+)'),
                     ('tx_retries', r'\ttx retries:\s*(\d+)'), ('tx_failed', r'\ttx failed:\s*(\d+)'),
                     ('tx_packets', r'\ttx packets:\s*(\d+)'), ('rx_packets', r'\trx packets:\s*(\d+)')):
        m = re.search(pat, out)
        if m:
            s[key] = float(m.group(1)) if '.' in m.group(1) else int(m.group(1))
    return s


def noise(iface):
    """the in-use channel's noise floor from iw survey (None: the driver does not report it)"""
    for block in sh('iw', 'dev', iface, 'survey', 'dump').split('Survey data from')[1:]:
        if '[in use]' in block:
            m = re.search(r'noise:\s*(-?\d+)', block)
            return int(m.group(1)) if m else None
    return None


def phy_snr(iface):
    """per-path SNR (dB) of the last frame from our mesh cell, from rtw88's debugfs phy_info
    (the RTL8812AU reports it with the patched rtw_88xxa); None if the driver does not"""
    try:
        with open('/sys/class/net/%s/phy80211/name' % iface) as f:
            phy = f.read().strip()
        with open('/sys/kernel/debug/ieee80211/%s/rtw88/phy_info' % phy) as f:
            m = re.search(r'\[Rx SNR\] = \{([^}]*)\}', f.read())
    except OSError:
        return None
    if not m:
        return None
    paths = [int(x) for x in m.group(1).split(',')][:2]         # the RTL8812AU has paths A and B
    return paths if any(paths) else None


def iw_channel(iface):
    m = re.search(r'channel (\d+) \((\d+) MHz\)', sh('iw', 'dev', iface, 'info'))
    return (int(m.group(1)), int(m.group(2))) if m else (None, None)


def write_json(path, obj):
    tmp = path + '.tmp'
    with open(tmp, 'w') as f:
        json.dump(obj, f, ensure_ascii=False)
    os.replace(tmp, path)


# ---------------------------------------------------------------------------------------------- one run

def run(args):
    os.makedirs(DATA, exist_ok=True)
    os.makedirs(RUN, exist_ok=True)
    if os.path.exists(PIDFILE):
        try:
            os.kill(int(open(PIDFILE).read()), 0)
            sys.exit('a run is already in progress')
        except (OSError, ValueError):
            pass
    with open(PIDFILE, 'w') as f:
        f.write(str(os.getpid()))
    signal.signal(signal.SIGTERM, lambda *_: stop.set())

    mode = MODES[args.mode]
    links = [l for l in args.links.split(',') if l]
    t0 = time.time()
    run_id = time.strftime('%Y%m%d-%H%M%S', time.localtime(t0))
    tag = run_id[-6:]                                   # short tag inside ERL messages (hhmmss)
    iface = mesh_if()
    state = read_state()
    rec = {
        'id': run_id, 'label': args.label, 'mode': args.mode, 'links': links, 'params': mode,
        'distance_m_manual': args.distance_m, 'started': t0, 'finished': None, 'stopped': False,
        'node': {'hostname': os.uname().nodename, 'erl': own_erl(state), 'mesh_if': iface},
        'wifi': None, 'erl': None, 'positions': [],
    }
    if iface:
        ch, freq = iw_channel(iface)
        rec['node'].update({'channel': ch, 'freq': freq})

    def progress():
        rec['updated'] = time.time()
        write_json(CURRENT, rec)

    threads = []
    lock = threading.Lock()

    # -- Wi-Fi: batctl ping plus a 1 s sampler ------------------------------------------------------------
    if 'wifi' in links:
        origs = originators()
        mac = args.peer_mac or (max(origs, key=lambda o: o['tq'])['mac'] if origs else None)
        w = rec['wifi'] = {'peer_mac': mac, 'sent': 0, 'replies': [], 'lost': [], 'samples': [],
                           'summary': None, 'error': None if mac else 'no mesh peer in range'}

        # batctl ping takes whole seconds only (-i 0.05 runs at 1 ping/s), so a faster mode runs several
        # pings side by side, started evenly within a second: 20 workers at 1/s = one ping every 50 ms.
        # stdbuf: batctl's output into a pipe is block-buffered, so replies would arrive in 4 KB lumps
        # and whatever is in the buffer is lost when the run is stopped. Stop sends SIGINT, on which
        # batctl prints its statistics.
        workers = max(1, round(1 / mode['wifi_interval'])) if mode['wifi_interval'] < 1 else 1
        interval = max(1, round(mode['wifi_interval']))
        per_worker = -(-mode['wifi_count'] // workers)
        w['workers'] = workers
        w['worker_stats'] = [None] * workers           # per worker: {'max_seq', 'transmitted', 'received'}
        procs = []

        def ping(k):
            stop.wait(k / workers)
            if not mac or stop.is_set():
                return
            p = subprocess.Popen(['stdbuf', '-oL', 'batctl', 'ping', '-c', str(per_worker), '-i', str(interval),
                                  '-t', '1', mac], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            with lock:
                procs.append(p)
                w['worker_stats'][k] = {'max_seq': 0, 'transmitted': None, 'received': None}
            for line in p.stdout:
                m = re.search(r'icmp_seq[= ](\d+).*?time[= ]([\d.]+) ms', line)
                with lock:
                    ws = w['worker_stats'][k]
                    if m:
                        seq = int(m.group(1))
                        w['replies'].append([seq, float(m.group(2)), time.time(), k])
                        ws['max_seq'] = max(ws['max_seq'], seq)
                    else:
                        u = re.search(r'icmp_seq[= ]?(\d+)', line)
                        if u and ('Unreachable' in line or 'time' in line.lower()):
                            w['lost'].append([int(u.group(1)), time.time(), k])
                            ws['max_seq'] = max(ws['max_seq'], int(u.group(1)))
                    s = re.search(r'(\d+) packets transmitted, (\d+) received', line)
                    if s:
                        ws['transmitted'], ws['received'] = int(s.group(1)), int(s.group(2))
                    w['sent'] = wifi_sent(w)
                    if all(x and x['transmitted'] is not None for x in w['worker_stats']):
                        w['summary'] = {'transmitted': sum(x['transmitted'] for x in w['worker_stats']),
                                        'received': sum(x['received'] for x in w['worker_stats'])}
            p.wait()

        def stopper():
            stop.wait()
            with lock:
                for p in procs:
                    if p.poll() is None:
                        p.send_signal(signal.SIGINT)

        for k in range(workers):
            tp = threading.Thread(target=ping, args=(k,)); tp.start(); threads.append(tp)
        threading.Thread(target=stopper, daemon=True).start()

        def sample():
            while not stop.is_set():
                o = next((x for x in originators() if x['mac'] == mac), None)
                s = {'t': time.time(), 'station': station(iface, mac) if mac else None,
                     'noise': noise(iface), 'snr_paths': phy_snr(iface), 'tq': o['tq'] if o else None,
                     'last_seen': o['last_seen'] if o else None}
                with lock:
                    w['samples'].append(s)
                stop.wait(1.0)

        ts = threading.Thread(target=sample, daemon=True); ts.start()

    # -- ERL: numbered messages, answered by the peer's responder ----------------------------------------
    if 'erl' in links:
        peers = erl_peers(state)
        peer = args.peer_erl or (peers[0]['address'] if peers else None)
        e = rec['erl'] = {'peer': peer, 'sent': [], 'acks': {}, 'rssi': [], 'link': [], 'radio': [],
                          'send_errors': 0, 'error': None if peer else 'no ERL peer known yet'}

        def send():
            if not peer:
                return
            n = mode['erl_count']
            last_id = (merlctl('chat', 'list', '0', '1') or {}).get('last_id', 0)
            watcher_done = threading.Event()

            def watch():
                nonlocal last_id
                while not watcher_done.is_set():
                    r = merlctl('chat', 'list', str(last_id), '100')
                    for m in r.get('messages') or []:
                        last_id = max(last_id, m['id'])
                        a = ACK_RE.match(m.get('text', ''))
                        if m.get('dir') == 'in' and a and a.group(1) == tag:
                            with lock:
                                e['acks'].setdefault(a.group(2), time.time())
                    watcher_done.wait(0.2)
            tw = threading.Thread(target=watch, daemon=True); tw.start()
            for seq in range(1, n + 1):
                if stop.is_set():
                    break
                t = time.time()
                r = merlctl('chat', 'send', peer, 'EXP %s %d/%d' % (tag, seq, n))
                with lock:
                    e['sent'].append([seq, t, bool(r.get('ok'))])
                    if not r.get('ok'):
                        e['send_errors'] += 1
                if seq < n:
                    stop.wait(max(0.0, mode['erl_interval'] - (time.time() - t)))
            end = time.time() + ACK_WAIT_S
            while not stop.is_set() and time.time() < end and len(e['acks']) < len(e['sent']):
                stop.wait(0.5)
            watcher_done.set()

        def sample():
            # the peer table (RSSI), and with firmware that has MERL_FEATURE_LINK_STATS the per-peer link
            # counters (SNR, heartbeats) and the radio's own (CRC errors, noise floor, time on air)
            while not stop.is_set():
                st = read_state()
                now = time.time()
                p = next((x for x in erl_peers(st) if x.get('address') == peer), None)
                lk = next((x for x in (st.get('links') or []) if x.get('address') == peer), None)
                with lock:
                    if p:
                        e['rssi'].append([now, p.get('rssi')])
                    if lk:
                        e['link'].append(dict(lk, t=now))
                    if st.get('radio'):
                        e['radio'].append(dict(st['radio'], t=now))
                stop.wait(1.0)

        te = threading.Thread(target=send); te.start(); threads.append(te)
        tr = threading.Thread(target=sample, daemon=True); tr.start()

    # -- positions every 5 s, progress every second --------------------------------------------------------
    last_pos = 0
    while any(t.is_alive() for t in threads):
        if time.time() - last_pos >= 5:
            st = read_state()
            own = st.get('position')
            peer_addr = (rec.get('erl') or {}).get('peer')
            pp = next((p for p in erl_peers(st) if p.get('address') == peer_addr), None) if peer_addr else None
            pos = {'t': time.time(), 'own': own,
                   'peer': {'latitude': pp.get('latitude'), 'longitude': pp.get('longitude')} if pp else None}
            pos['distance_m'] = haversine_m(own, pos['peer'])
            with lock:
                rec['positions'].append(pos)
            last_pos = time.time()
        with lock:
            progress()
        time.sleep(1.0)

    stop.set()
    rec['finished'] = time.time()
    rec['stopped'] = os.path.exists(RUN + '/stop')       # ended by the Stop button, not by itself
    rec['summary'] = summarize(rec)
    write_json('%s/run-%s.json' % (DATA, run_id), rec)
    progress()
    os.sync()
    for f in (PIDFILE, RUN + '/stop'):
        try:
            os.remove(f)
        except OSError:
            pass


# ---------------------------------------------------------------------------------------------- summaries

def stats(xs):
    xs = [x for x in xs if x is not None]
    if not xs:
        return None
    return {'n': len(xs), 'mean': statistics.mean(xs), 'sd': statistics.pstdev(xs) if len(xs) > 1 else 0.0,
            'median': statistics.median(xs), 'min': min(xs), 'max': max(xs)}


def wilson(k, n, z=1.96):
    """share k/n with its 95 % confidence interval"""
    if not n:
        return None
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return {'k': k, 'n': n, 'p': p, 'lo': max(0.0, c - h), 'hi': min(1.0, c + h)}


def link_summary(link, radio):
    """firmware link statistics over a run: differences between the first and the last sample"""
    out = {}
    if len(link) >= 2:
        a, b = link[0], link[-1]
        n = b['rx_packets'] - a['rx_packets']
        if n > 0:
            out['snr_db_mean'] = (b['snr_sum_db'] - a['snr_sum_db']) / n
            out['rssi_dbm_mean_fw'] = (b['rssi_sum'] - a['rssi_sum']) / n
        # per packet values, one per new packet (for SD and plots)
        snr = [x['snr_db'] for prev, x in zip(link, link[1:]) if x['rx_packets'] > prev['rx_packets']]
        out['snr_db'] = stats(snr)
        got = b['hb_received'] - a['hb_received']
        lost = b['hb_missed'] - a['hb_missed']
        if got + lost > 0 and got >= 0 and lost >= 0:
            out['hb_loss'] = wilson(lost, got + lost)       # the peer's heartbeats: its direction to us
        out['packets'] = n
        out['freq_error_hz'] = stats([x['freq_error_hz'] for x in link])
    if len(radio) >= 2:
        a, b = radio[0], radio[-1]
        if b['uptime_ms'] >= a['uptime_ms']:
            out['radio'] = {k: b[k] - a[k] for k in ('rx_ok', 'rx_crc_error', 'rx_bad_frame', 'tx_ok', 'tx_airtime_ms',
                                                     'lbt_busy', 'tx_queue_drop')}
            span = (b['uptime_ms'] - a['uptime_ms']) / 1000
            out['radio']['airtime_pct'] = 100 * out['radio']['tx_airtime_ms'] / 1000 / span if span > 0 else None
        out['noise_dbm'] = stats([x['noise_avg_dbm'] for x in radio if x.get('noise_avg_dbm') is not None])
    return out


def wifi_counts(w):
    """(pings sent, replies) of a run. batctl's own statistics when every worker printed them; else per
    worker the highest sequence number seen (pings go out in sequence, so every lower number was sent):
    timeouts are not always printed as lines, and runs before 2026-10-05 had block-buffered output"""
    def worker(r):          # replies [seq, rtt, t, k], lost [seq, t, k]; before 2026-10-05 without k
        return r[-1] if len(r) > 2 and isinstance(r[-1], int) else 0
    got = len({(worker(r), r[0]) for r in w.get('replies', [])})
    if w.get('summary'):
        return w['summary']['transmitted'], w['summary']['received']
    top = {}
    for r in w.get('replies', []) + w.get('lost', []):
        top[worker(r)] = max(top.get(worker(r), 0), r[0])
    return max(sum(top.values()), got), got


def wifi_sent(w):
    return wifi_counts(w)[0]


def wifi_snr(samples):
    """Wi-Fi SNR per sample: the driver's own (best antenna path) if it reports one, else signal - noise"""
    out = []
    for x in samples:
        if x.get('snr_paths'):
            out.append(max(x['snr_paths']))
        elif x.get('station') and x['station'].get('signal') is not None and x.get('noise') is not None:
            out.append(x['station']['signal'] - x['noise'])
    return out


def summarize(rec):
    s = {'label': rec['label'], 'mode': rec['mode'], 'distance_m_manual': rec.get('distance_m_manual')}
    ds = [p['distance_m'] for p in rec.get('positions', []) if p.get('distance_m') is not None]
    s['distance_m_gps'] = statistics.median(ds) if ds else None
    w = rec.get('wifi')
    if w and not w.get('error'):
        sent, got = wifi_counts(w)
        # signal, SNR and TQ only while the peer is heard: the driver and batman-adv keep the last values
        # of a peer that went silent (last OGM older than the node-loss criterion, 3 s)
        fresh = [x for x in w['samples'] if x.get('last_seen') is not None and x['last_seen'] <= FRESH_S]
        st = [x['station'] for x in fresh if x.get('station')]
        sig = [x.get('signal') for x in st]
        nz = [x['noise'] for x in fresh if x.get('noise') is not None]
        snr = wifi_snr(fresh)
        s['wifi'] = {'loss': wilson(sent - got, sent), 'rtt_ms': stats([r[1] for r in w['replies']]),
                     'rssi_dbm': stats(sig), 'noise_dbm': stats(nz), 'snr_db': stats(snr),
                     'tq_pct': stats([x['tq'] * 100 / 255 for x in fresh if x.get('tq') is not None]),
                     'samples': len(w['samples']), 'fresh_samples': len(fresh),
                     'tx_bitrate': stats([x.get('tx_bitrate') for x in st])}
    e = rec.get('erl')
    if e and not e.get('error'):
        sent = len(e['sent'])
        acks = e['acks']
        rtt = [acks[str(seq)] - t for seq, t, _ in e['sent'] if str(seq) in acks]
        s['erl'] = {'delivery': wilson(len(acks), sent), 'rtt_s': stats(rtt),
                    'rssi_dbm': stats([r[1] for r in e['rssi'] if r[1] not in (None, 0)]),
                    'send_errors': e.get('send_errors', 0)}
        s['erl'].update(link_summary(e.get('link') or [], e.get('radio') or []))
    return s


def load_runs():
    runs = []
    if os.path.isdir(DATA):
        for name in sorted(os.listdir(DATA)):
            if name.startswith('run-') and name.endswith('.json'):
                try:
                    with open(os.path.join(DATA, name)) as f:
                        runs.append(json.load(f))
                except (OSError, ValueError):
                    pass
    return runs


def csv_rows(runs):
    def g(d, *ks):
        for k in ks:
            d = (d or {}).get(k) if isinstance(d, dict) else None
        return d
    head = ['id', 'label', 'mode', 'distance_m_manual', 'distance_m_gps',
            'wifi_sent', 'wifi_lost', 'wifi_loss_pct', 'wifi_loss_ci_lo', 'wifi_loss_ci_hi',
            'wifi_rtt_ms_mean', 'wifi_rtt_ms_sd', 'wifi_rssi_dbm_mean', 'wifi_rssi_dbm_sd',
            'wifi_snr_db_mean', 'wifi_tq_pct_mean',
            'erl_sent', 'erl_delivered', 'erl_delivery_pct', 'erl_delivery_ci_lo', 'erl_delivery_ci_hi',
            'erl_rtt_s_mean', 'erl_rtt_s_sd', 'erl_rssi_dbm_mean', 'erl_rssi_dbm_sd',
            'erl_snr_db_mean', 'erl_snr_db_sd', 'erl_hb_loss_pct', 'erl_hb_loss_ci_lo', 'erl_hb_loss_ci_hi',
            'erl_noise_dbm_mean', 'erl_crc_errors', 'erl_airtime_pct']
    rows = [head]
    for r in runs:
        s = r.get('summary') or summarize(r)
        pct = lambda x: None if x is None else round(100 * x, 2)
        rows.append([r['id'], r['label'], r['mode'], s.get('distance_m_manual'), s.get('distance_m_gps'),
                     g(s, 'wifi', 'loss', 'n'), g(s, 'wifi', 'loss', 'k'), pct(g(s, 'wifi', 'loss', 'p')),
                     pct(g(s, 'wifi', 'loss', 'lo')), pct(g(s, 'wifi', 'loss', 'hi')),
                     g(s, 'wifi', 'rtt_ms', 'mean'), g(s, 'wifi', 'rtt_ms', 'sd'),
                     g(s, 'wifi', 'rssi_dbm', 'mean'), g(s, 'wifi', 'rssi_dbm', 'sd'),
                     g(s, 'wifi', 'snr_db', 'mean'), g(s, 'wifi', 'tq_pct', 'mean'),
                     g(s, 'erl', 'delivery', 'n'), g(s, 'erl', 'delivery', 'k'), pct(g(s, 'erl', 'delivery', 'p')),
                     pct(g(s, 'erl', 'delivery', 'lo')), pct(g(s, 'erl', 'delivery', 'hi')),
                     g(s, 'erl', 'rtt_s', 'mean'), g(s, 'erl', 'rtt_s', 'sd'),
                     g(s, 'erl', 'rssi_dbm', 'mean'), g(s, 'erl', 'rssi_dbm', 'sd'),
                     g(s, 'erl', 'snr_db_mean'), g(s, 'erl', 'snr_db', 'sd'), pct(g(s, 'erl', 'hb_loss', 'p')),
                     pct(g(s, 'erl', 'hb_loss', 'lo')), pct(g(s, 'erl', 'hb_loss', 'hi')),
                     g(s, 'erl', 'noise_dbm', 'mean'), g(s, 'erl', 'radio', 'rx_crc_error'),
                     g(s, 'erl', 'radio', 'airtime_pct')])
    return rows


# ---------------------------------------------------------------------------------------------- responder

def responder():
    """answer every test message; log what arrived (one-way delivery at the fixed node)"""
    os.makedirs(DATA, exist_ok=True)
    last_id = None
    while True:
        if last_id is None:
            r = merlctl('chat', 'list', '0', '1')
            if r.get('ok'):
                last_id = r.get('last_id', 0)           # answer only what arrives from now on
            else:
                time.sleep(2)
                continue
        r = merlctl('chat', 'list', str(last_id), '100')
        for m in r.get('messages') or []:
            last_id = max(last_id, m['id'])
            x = EXP_RE.match(m.get('text', ''))
            if m.get('dir') == 'in' and x:
                a = merlctl('chat', 'send', m['from'], 'EXP-ACK %s %s' % (x.group(1), x.group(2)))
                with open(RX_LOG, 'a') as f:
                    f.write(json.dumps({'t': time.time(), 'id': m['id'], 'from': m['from'], 'tag': x.group(1),
                                        'seq': int(x.group(2)), 'n': int(x.group(3)),
                                        'ack_sent': bool(a.get('ok'))}) + '\n')
        time.sleep(0.2)


# ---------------------------------------------------------------------------------------------- CLI

def main():
    os.umask(0o022)                  # the web server runs CGIs with umask 0
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('--label', default='P')
    r.add_argument('--distance-m', type=float, default=None)
    r.add_argument('--mode', choices=MODES, default='low')
    r.add_argument('--links', default='wifi,erl')
    r.add_argument('--peer-mac')
    r.add_argument('--peer-erl')
    for c in ('stop', 'status', 'runs', 'csv', 'rx', 'time', 'responder'):
        sub.add_parser(c)
    d = sub.add_parser('delete')
    d.add_argument('run_id')
    a = ap.parse_args()

    if a.cmd == 'run':
        run(a)
    elif a.cmd == 'stop':
        os.makedirs(RUN, exist_ok=True)
        try:
            open(RUN + '/stop', 'w').close()
            os.kill(int(open(PIDFILE).read()), signal.SIGTERM)
            print('{"ok":true}')
        except (OSError, ValueError):
            print('{"ok":false,"error":"no run in progress"}')
    elif a.cmd == 'status':
        running = False
        try:
            os.kill(int(open(PIDFILE).read()), 0)
            running = True
        except (OSError, ValueError):
            pass
        cur = None
        try:
            cur = json.load(open(CURRENT))
            cur = {'id': cur['id'], 'label': cur['label'], 'mode': cur['mode'], 'links': cur['links'],
                   'started': cur['started'], 'updated': cur.get('updated'), 'finished': cur.get('finished'),
                   'params': cur['params'], 'summary': summarize(cur),
                   'wifi_sent': (cur.get('wifi') or {}).get('sent'), 'wifi_error': (cur.get('wifi') or {}).get('error'),
                   'erl_sent': len((cur.get('erl') or {}).get('sent') or []),
                   'erl_error': (cur.get('erl') or {}).get('error'), 'positions': cur.get('positions', [])[-1:]}
        except (OSError, ValueError, KeyError):
            pass
        st = read_state()
        origs = originators()
        resp = sh('systemctl', 'is-active', 'erl-exp-responder.service').strip()
        print(json.dumps({'ok': True, 'running': running, 'current': cur, 'now': time.time(),
                          'responder': resp, 'erl': own_erl(st), 'erl_peers': erl_peers(st),
                          'mesh_peers': origs, 'position': st.get('position'),
                          'runs': [dict(id=x['id'], **(x.get('summary') or summarize(x))) for x in load_runs()]},
                         ensure_ascii=False))
    elif a.cmd == 'runs':
        print(json.dumps({'node': os.uname().nodename, 'exported': time.time(), 'runs': load_runs()},
                         ensure_ascii=False))
    elif a.cmd == 'csv':
        buf = io.StringIO()
        csv.writer(buf).writerows(csv_rows(load_runs()))
        sys.stdout.write(buf.getvalue())
    elif a.cmd == 'rx':
        rows = []
        try:
            rows = [json.loads(l) for l in open(RX_LOG) if l.strip()]
        except (OSError, ValueError):
            pass
        print(json.dumps({'node': os.uname().nodename, 'rx': rows}, ensure_ascii=False))
    elif a.cmd == 'delete':
        if not re.fullmatch(r'\d{8}-\d{6}', a.run_id):
            sys.exit('bad run id')
        try:
            os.remove('%s/run-%s.json' % (DATA, a.run_id))
            os.sync()
            print('{"ok":true}')
        except OSError:
            print('{"ok":false,"error":"no such run"}')
    elif a.cmd == 'time':
        print('%.6f' % time.time())
    elif a.cmd == 'responder':
        responder()


if __name__ == '__main__':
    main()

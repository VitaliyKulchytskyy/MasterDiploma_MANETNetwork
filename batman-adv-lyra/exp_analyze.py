#!/usr/bin/env python3
"""Turn link-experiment downloads (Test page, erl-exp) into tables and plots for the thesis.

  exp_analyze.py runs.json [more-runs.json ...] [--rx rx.json] [-o out-dir]

runs.json   "Download data (JSON)" from the node that was carried to the test points
rx.json     "Received log (JSON)" from the fixed node: splits ERL delivery into the two directions
            (test message reached the fixed node / its echo came back)

Writes to out-dir (default ./exp-results):
  points.csv       one row per distance and load mode: every metric's mean, SD and 95 % intervals
  summary.json     the same, plus the verdicts against the thesis criteria (table 2.8)
  *.png            metric vs distance for both links, low and high load, with the criteria lines
"""
import argparse, collections, csv, importlib.util, json, math, os, statistics, sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('erl_exp', os.path.join(HERE, 'erl-exp.py'))
E = importlib.util.module_from_spec(spec); spec.loader.exec_module(E)

# thesis table 2.8
CRIT = {'wifi_loss_ok': 10.0, 'wifi_loss_lost': 50.0, 'wifi_rssi_good': -70, 'wifi_rssi_edge': -85,
        'wifi_tq_ok': 50.0, 'wifi_rtt_ok_ms': 100.0, 'erl_delivery_ok': 90.0, 'erl_latency_ok_s': 5.0}


def distance(run):
    s = run.get('summary') or E.summarize(run)
    d = s.get('distance_m_manual')
    return d if d is not None else s.get('distance_m_gps')


def pool(runs):
    """all runs at one distance and mode, pooled: counts add up, samples join"""
    w_sent = w_got = 0
    rtt, rssi, snr, tq = [], [], [], []
    e_sent = e_ack = 0
    ertt, erssi, esnr, noise = [], [], [], []
    hb_got = hb_lost = crc = rx_ok = 0
    no_peer = 0
    for r in runs:
        w = r.get('wifi')
        if w and w.get('error') == 'no mesh peer in range':
            no_peer += 1                    # the mesh had already dropped the peer: Wi-Fi link lost
        if w and not w.get('error'):
            sent, got = E.wifi_counts(w)
            w_sent += sent; w_got += got
            rtt += [x[1] for x in w['replies']]
            # only samples taken while the peer was heard (see erl-exp summarize)
            fresh = [s for s in w['samples'] if s.get('last_seen') is not None and s['last_seen'] <= E.FRESH_S]
            snr += E.wifi_snr(fresh)
            for s in fresh:
                st = s.get('station') or {}
                if st.get('signal') is not None:
                    rssi.append(st['signal'])
                if s.get('tq') is not None:
                    tq.append(s['tq'] * 100 / 255)
        e = r.get('erl')
        if e and not e.get('error'):
            e_sent += len(e['sent']); e_ack += len(e['acks'])
            ertt += [e['acks'][str(seq)] - t for seq, t, _ in e['sent'] if str(seq) in e['acks']]
            erssi += [x[1] for x in e['rssi'] if x[1] not in (None, 0)]
            ls = E.link_summary(e.get('link') or [], e.get('radio') or [])
            link = e.get('link') or []
            esnr += [x['snr_db'] for p0, x in zip(link, link[1:]) if x['rx_packets'] > p0['rx_packets']]
            if ls.get('hb_loss'):
                hb_lost += ls['hb_loss']['k']; hb_got += ls['hb_loss']['n'] - ls['hb_loss']['k']
            if ls.get('radio'):
                crc += ls['radio']['rx_crc_error']; rx_ok += ls['radio']['rx_ok']
            if ls.get('noise_dbm'):
                noise.append(ls['noise_dbm']['mean'])
    out = {'runs': len(runs)}
    if no_peer:
        out['wifi_no_peer_runs'] = no_peer
    if w_sent:
        out['wifi'] = {'loss': E.wilson(w_sent - w_got, w_sent), 'rtt_ms': E.stats(rtt), 'rssi_dbm': E.stats(rssi),
                       'snr_db': E.stats(snr), 'tq_pct': E.stats(tq)}
    if e_sent:
        out['erl'] = {'delivery': E.wilson(e_ack, e_sent), 'rtt_s': E.stats(ertt), 'rssi_dbm': E.stats(erssi),
                      'snr_db': E.stats(esnr), 'hb_loss': E.wilson(hb_lost, hb_got + hb_lost),
                      'crc_error': E.wilson(crc, crc + rx_ok), 'noise_dbm': E.stats(noise)}
    return out


def one_way(runs, rx):
    """ERL per direction: requests that reached the fixed node, echoes that came back of those"""
    got = collections.Counter((x['tag'], x['seq']) for x in rx)
    res = {}
    for r in runs:
        e = r.get('erl')
        if not e or e.get('error'):
            continue
        tag = r['id'][-6:]
        up = sum(1 for seq, _, _ in e['sent'] if (tag, seq) in got)
        res[r['id']] = {'sent': len(e['sent']), 'reached_fixed': up, 'echo_back': len(e['acks'])}
    return res


def g(d, *ks):
    for k in ks:
        d = d.get(k) if isinstance(d, dict) else None
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('runs', nargs='+')
    ap.add_argument('--rx')
    ap.add_argument('-o', '--out', default='exp-results')
    ap.add_argument('--lang', choices=('uk', 'en'), default='uk', help='plot labels (uk: for the thesis)')
    ap.add_argument('--skip', default='', help='labels to leave out, comma-separated (e.g. an indoor pre-test)')
    ap.add_argument('--full-only', action='store_true',
                    help='leave out runs ended by Stop (their counts cover only the part that ran)')
    a = ap.parse_args()
    runs = []
    for f in a.runs:
        d = json.load(open(f))
        runs += (d['runs'] if 'runs' in d else [d]) if isinstance(d, dict) else d
    skip = {x for x in a.skip.split(',') if x}
    runs = [r for r in runs if r['label'] not in skip and not (a.full_only and r.get('stopped'))]
    rx = []
    if a.rx:                                    # "Received log (JSON)" or the node's rx.jsonl itself
        txt = open(a.rx).read()
        try:
            d = json.loads(txt)
            rx = d['rx'] if isinstance(d, dict) else d
        except ValueError:
            rx = [json.loads(l) for l in txt.splitlines() if l.strip()]
    os.makedirs(a.out, exist_ok=True)

    groups = collections.defaultdict(list)
    for r in runs:
        d = distance(r)
        if d is not None:
            groups[(round(d), r['mode'])].append(r)
    points = []
    for (d, mode), rs in sorted(groups.items()):
        p = pool(rs); p.update(distance_m=d, mode=mode, labels=sorted({r['label'] for r in rs}))
        points.append(p)

    # CSV: one row per point
    head = ['distance_m', 'mode', 'runs', 'wifi_sent', 'wifi_loss_pct', 'wifi_loss_ci95',
            'wifi_rtt_ms_mean', 'wifi_rtt_ms_sd', 'wifi_rtt_ms_median', 'wifi_rssi_dbm_mean', 'wifi_rssi_dbm_sd',
            'wifi_snr_db_mean', 'wifi_tq_pct_mean', 'wifi_tq_pct_sd',
            'erl_sent', 'erl_delivery_pct', 'erl_delivery_ci95', 'erl_rtt_s_mean', 'erl_rtt_s_sd',
            'erl_rssi_dbm_mean', 'erl_rssi_dbm_sd', 'erl_snr_db_mean', 'erl_snr_db_sd', 'erl_hb_loss_pct',
            'erl_hb_loss_ci95', 'erl_crc_error_pct', 'erl_noise_dbm_mean']
    f2 = lambda x, n=2: '' if x is None else round(x, n)
    ci = lambda s: '' if not s else '%.1f–%.1f' % (100 * s['lo'], 100 * s['hi'])
    with open(os.path.join(a.out, 'points.csv'), 'w', newline='') as f:
        w = csv.writer(f); w.writerow(head)
        for p in points:
            w.writerow([p['distance_m'], p['mode'], p['runs'], g(p, 'wifi', 'loss', 'n'),
                        f2(100 * g(p, 'wifi', 'loss', 'p')) if g(p, 'wifi', 'loss') else '', ci(g(p, 'wifi', 'loss')),
                        f2(g(p, 'wifi', 'rtt_ms', 'mean')), f2(g(p, 'wifi', 'rtt_ms', 'sd')),
                        f2(g(p, 'wifi', 'rtt_ms', 'median')), f2(g(p, 'wifi', 'rssi_dbm', 'mean'), 1),
                        f2(g(p, 'wifi', 'rssi_dbm', 'sd'), 1), f2(g(p, 'wifi', 'snr_db', 'mean'), 1),
                        f2(g(p, 'wifi', 'tq_pct', 'mean'), 1), f2(g(p, 'wifi', 'tq_pct', 'sd'), 1),
                        g(p, 'erl', 'delivery', 'n'),
                        f2(100 * g(p, 'erl', 'delivery', 'p')) if g(p, 'erl', 'delivery') else '',
                        ci(g(p, 'erl', 'delivery')), f2(g(p, 'erl', 'rtt_s', 'mean')), f2(g(p, 'erl', 'rtt_s', 'sd')),
                        f2(g(p, 'erl', 'rssi_dbm', 'mean'), 1), f2(g(p, 'erl', 'rssi_dbm', 'sd'), 1),
                        f2(g(p, 'erl', 'snr_db', 'mean'), 1), f2(g(p, 'erl', 'snr_db', 'sd'), 1),
                        f2(100 * g(p, 'erl', 'hb_loss', 'p')) if g(p, 'erl', 'hb_loss') else '', ci(g(p, 'erl', 'hb_loss')),
                        f2(100 * g(p, 'erl', 'crc_error', 'p')) if g(p, 'erl', 'crc_error') else '',
                        f2(g(p, 'erl', 'noise_dbm', 'mean'), 1)])

    # verdicts against table 2.8: the farthest point where each link still meets its criteria
    def farthest(mode, ok):
        ds = [p['distance_m'] for p in points if p['mode'] == mode and ok(p)]
        return max(ds) if ds else None
    verdict = {}
    for mode in ('low', 'high'):
        verdict[mode] = {
            'wifi_usable_up_to_m': farthest(mode, lambda p: g(p, 'wifi', 'loss', 'p') is not None
                                            and 100 * p['wifi']['loss']['p'] <= CRIT['wifi_loss_ok']),
            'wifi_lost_from_m': min([p['distance_m'] for p in points if p['mode'] == mode
                                     and (p.get('wifi_no_peer_runs')       # the mesh had dropped the peer
                                          or g(p, 'wifi', 'loss', 'p') is not None
                                          and 100 * p['wifi']['loss']['p'] > CRIT['wifi_loss_lost'])] or [None],
                                    key=lambda x: math.inf if x is None else x),
            'erl_usable_up_to_m': farthest(mode, lambda p: g(p, 'erl', 'delivery', 'p') is not None
                                           and 100 * p['erl']['delivery']['p'] >= CRIT['erl_delivery_ok']
                                           and (g(p, 'erl', 'rtt_s', 'median') or 0) <= 2 * CRIT['erl_latency_ok_s']),
        }
    json.dump({'criteria': CRIT, 'points': points, 'verdict': verdict,
               'one_way_erl': one_way(runs, rx) if rx else None},
              open(os.path.join(a.out, 'summary.json'), 'w'), ensure_ascii=False, indent=1)

    # plots
    import matplotlib; matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    plt.rcParams['font.family'] = 'DejaVu Serif'
    style = {'low': dict(marker='o', color='#1f5fa8'), 'high': dict(marker='s', color='#e38a2e')}
    UK = {'Distance, m': 'Відстань, м', 'low load': 'мале навантаження', 'high load': 'велике навантаження',
          'Wi-Fi packet loss, %': 'Втрати пакетів Wi-Fi, %', 'Wi-Fi RSSI, dBm': 'RSSI Wi-Fi, дБм',
          'Wi-Fi SNR, dB': 'SNR Wi-Fi, дБ', 'Wi-Fi RTT, ms': 'Час обміну Wi-Fi, мс',
          'Wi-Fi link quality (TQ), %': 'Якість зв’язку Wi-Fi (TQ), %',
          'ERL messages echoed, %': 'Доставлені повідомлення ERL, %', 'ERL RSSI, dBm': 'RSSI ERL, дБм',
          'ERL round trip, s': 'Час обміну ERL, с', 'ERL SNR, dB': 'SNR ERL, дБ',
          'ERL heartbeats lost, %': 'Втрачені службові пакети ERL, %', 'margin 3 dB': 'запас 3 дБ',
          'SF11 limit': 'межа SF11', 'usable': 'придатний', 'lost': 'втрачений',
          'reliable': 'надійний', 'edge': 'граничний', 'limit': 'межа', 'required': 'необхідно'}
    tr = (lambda t: UK.get(t, t)) if a.lang == 'uk' else (lambda t: t)

    def plot(name, link, metric, ylabel, scale=1.0, share=False, lines=()):
        fig, ax = plt.subplots(figsize=(8, 3.8), dpi=200)
        drawn = False
        for mode in ('low', 'high'):
            ps = [p for p in points if p['mode'] == mode and g(p, link, metric)]
            if not ps:
                continue
            xs = [p['distance_m'] for p in ps]
            if share:
                ys = [100 * p[link][metric]['p'] for p in ps]
                lo = [max(0.0, y - 100 * p[link][metric]['lo']) for y, p in zip(ys, ps)]
                hi = [max(0.0, 100 * p[link][metric]['hi'] - y) for y, p in zip(ys, ps)]
                ax.errorbar(xs, ys, yerr=[lo, hi], capsize=3, label=tr(mode + ' load'), **style[mode])
            else:
                ys = [p[link][metric]['mean'] * scale for p in ps]
                ax.errorbar(xs, ys, yerr=[p[link][metric]['sd'] * scale for p in ps], capsize=3,
                            label=tr(mode + ' load'), **style[mode])
            drawn = True
        if not drawn:
            plt.close(fig)
            return
        for y, text in lines:
            ax.axhline(y, color='grey', linestyle='--', linewidth=0.8)
            ax.annotate(tr(text), (1.0, y), xycoords=('axes fraction', 'data'), ha='right', va='bottom', fontsize=8,
                        color='grey')
        ax.set_xlabel(tr('Distance, m')); ax.set_ylabel(tr(ylabel))
        ds = [p['distance_m'] for p in points if p['distance_m'] > 0]
        if ds and max(ds) / min(ds) > 20:           # Wi-Fi metres and LoRa kilometres on one axis
            ax.set_xscale('log')
        ax.grid(linestyle=':', linewidth=0.5); ax.legend()
        fig.tight_layout(); fig.savefig(os.path.join(a.out, name + '.png')); plt.close(fig)

    plot('wifi_loss', 'wifi', 'loss', 'Wi-Fi packet loss, %', share=True,
         lines=[(CRIT['wifi_loss_ok'], 'usable'), (CRIT['wifi_loss_lost'], 'lost')])
    plot('wifi_rssi', 'wifi', 'rssi_dbm', 'Wi-Fi RSSI, dBm',
         lines=[(CRIT['wifi_rssi_good'], 'reliable'), (CRIT['wifi_rssi_edge'], 'edge')])
    plot('wifi_snr', 'wifi', 'snr_db', 'Wi-Fi SNR, dB')
    plot('wifi_rtt', 'wifi', 'rtt_ms', 'Wi-Fi RTT, ms', lines=[(CRIT['wifi_rtt_ok_ms'], 'limit')])
    plot('wifi_tq', 'wifi', 'tq_pct', 'Wi-Fi link quality (TQ), %', lines=[(CRIT['wifi_tq_ok'], 'usable')])
    plot('erl_delivery', 'erl', 'delivery', 'ERL messages echoed, %', share=True,
         lines=[(CRIT['erl_delivery_ok'], 'required')])
    plot('erl_rssi', 'erl', 'rssi_dbm', 'ERL RSSI, dBm')
    plot('erl_rtt', 'erl', 'rtt_s', 'ERL round trip, s')
    plot('erl_snr', 'erl', 'snr_db', 'ERL SNR, dB',
         lines=[(-17.5 + 3, 'margin 3 dB'), (-17.5, 'SF11 limit')])
    plot('erl_hb_loss', 'erl', 'hb_loss', 'ERL heartbeats lost, %', share=True)

    print('%d runs, %d points -> %s' % (len(runs), len(points), a.out))
    for mode, v in verdict.items():
        print('%-4s  Wi-Fi usable up to %s m, lost from %s m; ERL usable up to %s m'
              % (mode, v['wifi_usable_up_to_m'], v['wifi_lost_from_m'], v['erl_usable_up_to_m']))


if __name__ == '__main__':
    main()

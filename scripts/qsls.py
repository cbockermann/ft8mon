#!/usr/bin/env python3
"""Group ft8mon JSON decodes into QSOs (QSLs).

Reads a list of JSON records in ft8mon's -json format (one object per
line, i.e. what udp_json_logger.py writes, or a single JSON array) and
reconstructs the QSO exchanges. Each QSL is the ordered list of messages
belonging to one contact between two stations.

  ./scripts/qsls.py ft8_decodes.ndjson          # from a file
  cat ft8_decodes.ndjson | ./scripts/qsls.py    # from stdin

A message is assigned to a QSO by the unordered pair of callsigns it
mentions. Messages of the same pair that are more than --gap seconds
apart are treated as separate contacts. A `CQ` call is attached to the
QSO that answers it, when there is one.

Output is a JSON array of QSLs, each:

  {
    "call1": "AB1HL", "call2": "K1JT",
    "start": "...", "end": "...", "n": 6,
    "messages": [ <original records in time order> ]
  }
"""

import argparse
import json
import re
import sys
from datetime import datetime, timezone

# a Maidenhead grid such as FN42 or FN42ab (an exchange, not a callsign).
GRID_RE = re.compile(r'^[A-R]{2}[0-9]{2}([A-X]{2})?$', re.I)
# a signal report such as -10, +05, R-12.
REPORT_RE = re.compile(r'^R?[-+][0-9]{1,2}$')
# a callsign only contains these characters (after dropping <hash> brackets).
CALL_CHARS_RE = re.compile(r'^[A-Z0-9/]+$', re.I)
# tokens that are sign-offs / acknowledgements, never callsigns.
SIGNOFF = {'RRR', 'RR73', '73', 'R73', 'TU', 'TU73', 'QSL', 'DE', 'CQ', 'DX'}


def normcall(t):
    """Normalise a callsign token (drop <hash> brackets, upper-case)."""
    return t.strip('<>').upper()


def is_call(t):
    """True if the token looks like a callsign rather than an exchange."""
    t = t.strip('<>')
    if not t or not CALL_CHARS_RE.match(t):
        return False
    if GRID_RE.match(t) or REPORT_RE.match(t) or t.upper() in SIGNOFF:
        return False
    # callsigns contain at least one digit and one letter.
    return any(c.isdigit() for c in t) and any(c.isalpha() for c in t)


def parse_msg(msg):
    """Return (kind, [calls]) for an FT8 message text.

    kind is 'cq' (one caller), 'qso' (two callsigns) or None.
    """
    toks = msg.split()
    if not toks:
        return (None, [])
    if toks[0] == 'CQ':
        for t in toks[1:]:
            if is_call(t):
                return ('cq', [normcall(t)])
        return (None, [])
    calls = []
    for t in toks:
        if is_call(t):
            calls.append(normcall(t))
        if len(calls) == 2:
            break
    if len(calls) == 2:
        return ('qso', calls)
    return (None, [])


def rec_time(r, idx):
    """A sortable timestamp for a record; falls back to arrival order."""
    if 'unix' in r:
        try:
            return float(r['unix'])
        except (TypeError, ValueError):
            pass
    if 'time' in r:
        try:
            s = str(r['time']).replace('Z', '+00:00')
            return datetime.fromisoformat(s).timestamp()
        except ValueError:
            pass
    return float(idx)


def iso(ts):
    return datetime.fromtimestamp(ts, timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def load_records(fp):
    """Load NDJSON (one object per line) or a single JSON array."""
    data = fp.read()
    if data.lstrip().startswith('['):
        return list(json.loads(data))
    recs = []
    for line in data.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            recs.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return recs


def build_qsls(records, gap):
    """Group records into QSOs. Returns a list of QSL dicts."""
    # annotate with time + parsed calls, then sort by time.
    items = []
    for idx, r in enumerate(records):
        if not isinstance(r, dict) or 'msg' not in r:
            continue
        kind, calls = parse_msg(str(r['msg']))
        if kind is None:
            continue
        items.append({'rec': r, 't': rec_time(r, idx), 'kind': kind, 'calls': calls})
    items.sort(key=lambda x: x['t'])

    qsos = []              # list of qso dicts
    open_qso = {}          # frozenset(pair) -> current open qso dict

    # first pass: directed two-call messages form the QSOs.
    for it in items:
        if it['kind'] != 'qso':
            continue
        pair = frozenset(it['calls'])
        q = open_qso.get(pair)
        if q is not None and it['t'] - q['end'] <= gap:
            q['items'].append(it)
            q['end'] = it['t']
        else:
            q = {'pair': pair, 'calls': sorted(it['calls']),
                 'items': [it], 'start': it['t'], 'end': it['t']}
            qsos.append(q)
            open_qso[pair] = q

    # second pass: attach each CQ to the earliest following QSO of its caller.
    for it in items:
        if it['kind'] != 'cq' or not it['calls']:
            continue
        caller = it['calls'][0]
        best = None
        for q in qsos:
            if caller in q['pair'] and q['start'] >= it['t'] and q['start'] - it['t'] <= gap:
                if best is None or q['start'] < best['start']:
                    best = q
        if best is not None:
            best['items'].append(it)
            best['start'] = min(best['start'], it['t'])

    # emit, messages sorted by time.
    out = []
    for q in sorted(qsos, key=lambda q: q['start']):
        msgs = [x['rec'] for x in sorted(q['items'], key=lambda x: x['t'])]
        out.append({
            'call1': q['calls'][0],
            'call2': q['calls'][1],
            'start': iso(q['start']),
            'end': iso(q['end']),
            'n': len(msgs),
            'messages': msgs,
        })
    return out


def main():
    ap = argparse.ArgumentParser(
        description="Group ft8mon JSON decodes into QSOs (QSLs).")
    ap.add_argument('files', nargs='*',
                    help="NDJSON files to read (default: stdin)")
    ap.add_argument('--gap', type=float, default=180.0,
                    help="seconds; larger gaps split a pair into separate "
                         "QSOs and bound CQ attachment (default: 180)")
    ap.add_argument('--min-msgs', type=int, default=1,
                    help="drop QSLs with fewer than this many messages "
                         "(default: 1)")
    args = ap.parse_args()

    records = []
    if args.files:
        for fn in args.files:
            with open(fn, encoding='utf-8') as f:
                records.extend(load_records(f))
    else:
        records = load_records(sys.stdin)

    qsls = [q for q in build_qsls(records, args.gap) if q['n'] >= args.min_msgs]
    json.dump(qsls, sys.stdout, indent=2, ensure_ascii=False)
    sys.stdout.write('\n')
    print(f"{len(records)} records -> {len(qsls)} QSLs", file=sys.stderr)


if __name__ == '__main__':
    main()

"""Digest of usage data: %LOCALAPPDATA%\\Ombra\\telemetry + logs.

    py scripts/telemetry.py              # last 3 days
    py scripts/telemetry.py --days 7
    py scripts/telemetry.py --date 20261019

One block per engine run (uptime_s resets) and per day. Tick counters (ticks, late_ticks,
resyncs, encode_*) are per sample row; rebuilds, resolution_changes, saves and audio_* are
cumulative per run (telemetry.cpp). Memory growth is measured after warm-up (one buffer length
+ 60 s); ring commit follows live footage (video_ring_committed_mb, absent before 2026-10-20,
when it grew to the whole arena), so read growth against capacity + overshoot. Standard library only.
"""
import argparse
import collections
import csv
import datetime as dt
import glob
import os
import re
import statistics

ROOT = os.path.join(os.environ.get("OMBRA_HOME") or os.path.join(os.environ["LOCALAPPDATA"], "Ombra"))
TEL = os.path.join(ROOT, "telemetry")
LOGS = os.path.join(ROOT, "logs")


def f(row, key, default=0.0):
    try:
        return float(row.get(key) or default)
    except ValueError:
        return default


def buffer_seconds():
    path = os.path.join(os.environ.get("OMBRA_HOME") or os.environ["APPDATA"], "Ombra", "config.json")
    try:
        text = open(path, encoding="utf-8").read()
    except OSError:
        return 120
    m = re.search(r'"buffer"\s*:\s*\{[^}]*"seconds"\s*:\s*(\d+)', text, re.S)
    return int(m.group(1)) if m else 120


def load_samples(day):
    rows = []
    for path in sorted(glob.glob(os.path.join(TEL, f"samples-{day}*.csv"))):
        with open(path, newline="", encoding="utf-8", errors="replace") as fh:
            rows.extend(csv.DictReader(fh))
    rows.sort(key=lambda r: r.get("time", ""))
    return rows


def split_runs(rows):
    runs, cur, last_up = [], [], -1.0
    for r in rows:
        up = f(r, "uptime_s")
        if cur and up < last_up:
            runs.append(cur)
            cur = []
        cur.append(r)
        last_up = up
    if cur:
        runs.append(cur)
    return runs


def pct(values, p):
    if not values:
        return 0.0
    values = sorted(values)
    return values[min(len(values) - 1, int(p * len(values)))]


def run_summary(run, warm_s):
    t0, t1 = run[0]["time"], run[-1]["time"]
    hours = max((f(run[-1], "uptime_s") - f(run[0], "uptime_s")) / 3600.0, 1e-9)
    out = [f"  run {t0[11:16]}-{t1[11:16]}  {hours:.1f} h  {len(run)} samples"]

    warm = [r for r in run if f(r, "uptime_s") >= warm_s]
    if len(warm) >= 4:
        span_h = (f(warm[-1], "uptime_s") - f(warm[0], "uptime_s")) / 3600.0
        def growth(key):
            a, b = f(warm[0], key), f(warm[-1], key)
            return a, b, (b - a) / span_h if span_h > 0 else 0.0
        pa, pb, prate = growth("private_mb")
        ha, hb, hrate = growth("handles")
        ta, tb, _ = growth("threads")
        # Ring commit is bounded by the arena (and follows live footage since 2026-10-20). Only
        # growth past first sample + arena + audio is unexplained.
        bound = f(run[0], "private_mb") + max(f(r, "video_ring_capacity_mb") for r in run) \
            + max(f(r, "audio_ring_mb") for r in run) + 50
        peak = max(f(r, "private_mb") for r in warm)
        if f(run[0], "uptime_s") > warm_s:
            flag = "  (run continued from the previous day; see that day for the bound)"
        elif peak > bound:
            flag = f"  <-- ABOVE BOUND {bound:.0f}"
        else:
            flag = f"  (bound {bound:.0f}: arena fill, ok)"
        out.append(f"    private_mb {pa:.0f} -> {pb:.0f} after warm-up ({prate:+.1f} MB/h), peak {peak:.0f}{flag}")
        out.append(f"    handles {ha:.0f} -> {hb:.0f} ({hrate:+.0f}/h)  threads {ta:.0f} -> {tb:.0f}")
    else:
        out.append(f"    private_mb max {max(f(r, 'private_mb') for r in run):.0f} (run shorter than warm-up {warm_s}s)")

    armed = [r for r in run if f(r, "armed") == 1 and f(r, "ticks") > 0]
    if armed:
        ticks = sum(f(r, "ticks") for r in armed)
        late = sum(f(r, "late_ticks") for r in armed)
        resync = sum(f(r, "resyncs") for r in armed)
        p50 = statistics.median(f(r, "encode_p50_ms") for r in armed)
        p99 = pct([f(r, "encode_p99_ms") for r in armed], 0.95)
        mx = max(f(r, "encode_max_ms") for r in armed)
        fps = ticks / sum(f(r, "interval_s") for r in armed)
        out.append(f"    encode p50 {p50:.1f} ms  p99(95th row) {p99:.1f}  max {mx:.1f}  ~{fps:.0f} fps"
                   f"  late {late:.0f}/{ticks:.0f} ({100 * late / ticks:.2f}%)  resyncs {resync:.0f}")
    out.append(f"    armed {100 * sum(f(r, 'armed') for r in run) / len(run):.0f}% of samples"
               f"  pipelines {run[-1].get('pipelines', '?')}  ring {f(run[-1], 'video_ring_mb'):.0f}"
               + (f" live, {f(run[-1], 'video_ring_committed_mb'):.0f} committed"
                  if run[-1].get("video_ring_committed_mb") else "")
               + f" of {f(run[-1], 'video_ring_capacity_mb'):.0f} MB")

    last = run[-1]
    counters = {k: f(last, k) for k in ("rebuilds", "resolution_changes", "saves",
                                        "audio_dropped_ms", "audio_late_ms", "audio_encoder_errors")}
    out.append("    " + "  ".join(f"{k} {v:.0f}" for k, v in counters.items()))
    bad = [k for k in ("audio_dropped_ms", "audio_late_ms", "audio_encoder_errors") if counters[k] > 0]
    if bad:
        out.append(f"    <-- audio loss counters non-zero: {', '.join(bad)} (a bug in normal use)")

    fg = collections.Counter(r.get("fg_exe", "") for r in run)
    games = collections.Counter(r.get("game", "") for r in run if r.get("game"))
    out.append("    foreground: " + ", ".join(f"{k} {v}" for k, v in fg.most_common(4)))
    if games:
        out.append("    game: " + ", ".join(f"{k} {v}" for k, v in games.most_common(3)))
    return out


def events(day):
    path = os.path.join(TEL, f"events-{day}.csv")
    if not os.path.exists(path):
        return []
    with open(path, newline="", encoding="utf-8", errors="replace") as fh:
        rows = list(csv.DictReader(fh))
    counts = collections.Counter(r["type"] for r in rows)
    out = ["  events: " + ", ".join(f"{k} {v}" for k, v in counts.most_common())]
    for r in rows:
        if r["type"] in ("save", "save_skipped", "engine_start", "engine_stop", "settings_change",
                         "pipeline_rebuild", "resolution_change", "game", "previous_crash"):
            out.append(f"    {r['time'][11:19]} {r['type']} {r.get('detail', '')[:110]}")
    return out


def log_problems(day):
    path = os.path.join(LOGS, f"ombra-{day}.log")
    if not os.path.exists(path):
        return []
    errors, warns = collections.Counter(), collections.Counter()
    first = {}
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            # "[time] [LEVEL] [P pid thread] message"; the process tag is absent before 2026-11-06.
            m = re.match(r"\[(\S+)\] \[(ERROR|FATAL|WARN )\] (?:\[[EUT] \d+ \S+\] )?(.*)", line)
            if not m:
                continue
            # Normalise numbers so repeats of one message collapse into one line.
            key = re.sub(r"\d+", "N", m.group(3))[:120]
            (warns if m.group(2) == "WARN " else errors)[key] += 1
            first.setdefault(key, m.group(1)[:8])
    out = []
    if errors:
        out.append(f"  log ERROR/FATAL: {sum(errors.values())}")
        out += [f"    {n:>4}x first {first[k]}  {k}" for k, n in errors.most_common(8)]
    if warns:
        out.append(f"  log WARN: {sum(warns.values())} ({len(warns)} distinct; top 3)")
        out += [f"    {n:>4}x  {k}" for k, n in warns.most_common(3)]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--days", type=int, default=3)
    ap.add_argument("--date", help="YYYYMMDD; overrides --days")
    a = ap.parse_args()
    if a.date:
        days = [a.date]
    else:
        today = dt.date.today()
        days = [(today - dt.timedelta(days=i)).strftime("%Y%m%d") for i in range(a.days - 1, -1, -1)]

    warm_s = buffer_seconds() + 60
    print(f"Ombra telemetry digest  ({ROOT}; warm-up {warm_s}s from buffer.seconds)")
    for day in days:
        rows = load_samples(day)
        ev, logs = events(day), log_problems(day)
        if not rows and not ev and not logs:
            continue
        print(f"\n== {day[:4]}-{day[4:6]}-{day[6:]}")
        for run in split_runs(rows):
            print("\n".join(run_summary(run, warm_s)))
        print("\n".join(ev + logs))


if __name__ == "__main__":
    main()

"""Quality-preset calibration: the engine's own encoders (ombra-encbench) over a bitrate ladder,
scored with VMAF against a lossless source, then AV1's bitrate saving over H.264 at equal VMAF
(BD-rate) per clip and GPU.

  py scripts/encbench.py --clips DIR [--out DIR] [--vendors nvidia,amd] [--fps 60] [--height 1080]
  py scripts/encbench.py --clips DIR --height 720     # then: the row matching 1080p's presets
  py scripts/encbench.py --clips DIR --cqp 16,20,24   # what constant quality really costs

DIR holds 1080p .y4m clips, e.g. the first seconds of Xiph's lossless Twitch set
(media.xiph.org/video/derf/twitch/y4m/). Each clip is converted to raw BGRA once (cached next to
the results), as WGC would hand it to the encoder. --fps 30 drops every other frame first;
--height 720 or 900 area-scales to 16:9 first, as capture.resolution's FrameScaler would.
Needs ffmpeg with libvmaf on PATH. Results: results-*.json and a printed table.

Above 60 fps: 120 fps clips (UVG's lossless set, ultravideo.fi/dataset.php; natural video, not games):
  py scripts/encbench.py --clips DIR --source-fps 120 --fps 60   # then:
  py scripts/encbench.py --clips DIR --source-fps 120 --fps 120
VMAF was trained at up to 60 fps and its motion feature halves at 120, so the 120 fps encode is scored on
its even frames only: the same frames, with the same motion between them, as the 60 fps encode.
"""
import argparse
import json
import math
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENCBENCH = os.path.join(ROOT, "build", "Release", "ombra-encbench.exe")
LADDER = [4000, 6000, 8000, 11000, 15000, 20000, 28000]
LADDER_720 = [2000, 3000, 4000, 6000, 8000, 11000, 15000, 20000]
W, H = 1920, 1080
SOURCE_FPS = 60
SCORE_EVEN_FRAMES = False  # above 60 fps; see the module comment
# config.cpp's 1080p row at low/medium/high, 60 fps, per codec (kAv1BitrateFactor).
PRESETS_1080 = {"h264": {"low": 12500, "medium": 18750, "high": 25000},
                "av1": {"low": 10000, "medium": 15000, "high": 20000}}


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def raw_bgra(clip, out_dir, fps):
    path = os.path.join(out_dir, f"{os.path.splitext(os.path.basename(clip))[0]}-{fps}-{H}.bgra")
    if not os.path.exists(path):
        vf = "scale=in_color_matrix=bt709:in_range=tv,format=bgra"
        if H != 1080:
            vf = f"scale={W}:{H}:flags=area:in_color_matrix=bt709:in_range=tv,format=bgra"
        if fps * 2 == SOURCE_FPS:
            vf = "select=not(mod(n\\,2))," + vf
        run(["ffmpeg", "-v", "error", "-y", "-i", clip, "-vf", vf, "-fps_mode", "passthrough",
             "-f", "rawvideo", path + ".tmp"])
        os.replace(path + ".tmp", path)
    return path


def reference(raw, fps):
    """What the encodes are scored against: the BGRA frames the encoder saw, converted to yuv420p
    the way the engine's streams are tagged (BT.709 limited), so the encoder's loss alone is
    measured whatever the source's own matrix."""
    path = raw[:-len(".bgra")] + ".y4m"
    if not os.path.exists(path):
        run(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pixel_format", "bgra", "-video_size",
             f"{W}x{H}", "-framerate", str(fps), "-i", raw, "-vf",
             "scale=out_color_matrix=bt709:out_range=tv,format=yuv420p", path + ".tmp.y4m"])
        os.replace(path + ".tmp.y4m", path)
    return path


def encode(raw, vendor, codec, kbps, fps, out, cq=None):
    extra = ["p4", "cqp", str(cq)] if cq is not None else []  # encode.preset's default
    # OMBRA_HOME: keep benchmark log lines out of the real logs and telemetry digest.
    env = dict(os.environ, OMBRA_HOME=os.path.join(os.path.dirname(os.path.abspath(out)), "home"))
    with open(raw, "rb") as f:
        r = run([ENCBENCH, vendor, codec, str(W), str(H), str(fps), str(kbps), out] + extra,
                stdin=f, capture_output=True, text=True, env=env)
    # The engine's log lines go to stdout too; the result is the last line.
    line = [ln for ln in r.stdout.splitlines() if ln.startswith("frames=")][-1]
    fields = dict(kv.split("=") for kv in line.split())
    return float(fields["kbps"]), float(fields["encode_ms_avg"])


def vmaf(dist, ref, log):
    # AMD's AV1 pads 1080 to 1082 at the bottom: crop it back. n_subsample=2 halves the cost.
    log_arg = log.replace("\\", "/").replace(":", "\\:")  # filtergraph escaping for C:/...
    # Both sides get the same tags: the y4m carries none, and ffmpeg's filter graph otherwise
    # converts one to the other's colour space, which scored a correct BT.709 stream as 23 dB.
    tags = "setparams=range=tv:color_primaries=bt709:color_trc=bt709:colorspace=bt709"
    pick = "select=not(mod(n\\,2))," if SCORE_EVEN_FRAMES else ""
    graph = (f"[0:v]crop={W}:{H}:0:0,{pick}{tags},settb=1/60,setpts=N[d];[1:v]{pick}{tags},settb=1/60,setpts=N[r];"
             f"[d][r]libvmaf=n_threads={os.cpu_count()}:n_subsample=2:log_fmt=json:log_path='{log_arg}'")
    run(["ffmpeg", "-v", "error", "-i", dist, "-i", ref, "-lavfi", graph, "-fps_mode", "passthrough", "-f", "null", "-"])
    with open(log) as f:
        data = json.load(f)
    frames = sorted(fr["metrics"]["vmaf"] for fr in data["frames"])
    return data["pooled_metrics"]["vmaf"]["mean"], frames[max(0, len(frames) // 100 - 1)]


def bd_rate(a, b):
    """Average bitrate difference of b vs a at equal quality, in percent (negative: b is smaller).
    Each curve is (kbps, vmaf) points; piecewise-linear log-rate over the overlapping VMAF range."""
    def curve(points):
        pts = sorted((q, math.log(r)) for r, q in points)
        return pts

    def at(pts, q):
        for (q0, r0), (q1, r1) in zip(pts, pts[1:]):
            if q0 <= q <= q1:
                return r0 + (r1 - r0) * (q - q0) / (q1 - q0) if q1 > q0 else r0
        return None

    ca, cb = curve(a), curve(b)
    lo, hi = max(ca[0][0], cb[0][0]), min(ca[-1][0], cb[-1][0])
    if hi <= lo:
        return None
    n = 64
    diffs = []
    for i in range(n + 1):
        q = lo + (hi - lo) * i / n
        ra, rb = at(ca, q), at(cb, q)
        if ra is not None and rb is not None:
            diffs.append(rb - ra)
    return (math.exp(sum(diffs) / len(diffs)) - 1) * 100 if diffs else None


def rate_for(points, target):
    """kbps where the curve reaches `target` VMAF, interpolated in log-rate; None if never."""
    pts = sorted(points)
    for (r0, q0), (r1, q1) in zip(pts, pts[1:]):
        if q0 <= target <= q1 and q1 > q0:
            return math.exp(math.log(r0) + (math.log(r1) - math.log(r0)) * (target - q0) / (q1 - q0))
    return None


def vmaf_at(points, kbps):
    """VMAF the curve reaches at `kbps`, interpolated in log-rate; None outside the curve."""
    pts = sorted(points)
    for (r0, q0), (r1, q1) in zip(pts, pts[1:]):
        if r0 <= kbps <= r1:
            return q0 + (q1 - q0) * (math.log(kbps) - math.log(r0)) / (math.log(r1) - math.log(r0))
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clips", required=True)
    ap.add_argument("--out", default=None)
    ap.add_argument("--vendors", default="nvidia,amd")
    ap.add_argument("--fps", type=int, default=60, choices=[30, 60, 120])
    ap.add_argument("--source-fps", type=int, default=60, choices=[60, 120], help="the clips' own frame rate")
    ap.add_argument("--height", type=int, default=1080, choices=[720, 900, 1080])
    ap.add_argument("--cqp", default=None, help="comma-separated encode.cq values: rate-control cqp instead of the ladder")
    args = ap.parse_args()
    global W, H, SOURCE_FPS, SCORE_EVEN_FRAMES
    SOURCE_FPS = args.source_fps
    SCORE_EVEN_FRAMES = args.fps > 60
    if args.height != 1080:
        W, H = args.height * 16 // 9, args.height
    out_dir = args.out or os.path.join(args.clips, "results")
    os.makedirs(out_dir, exist_ok=True)
    suffix = f"{args.fps}" if H == 1080 else f"{args.fps}-{H}"
    results_path = os.path.join(out_dir, f"results-{suffix}{'-cqp' if args.cqp else ''}.json")
    results = json.load(open(results_path)) if os.path.exists(results_path) else {}
    # CQP ignores the bitrate; it's only there because the encoder's Open wants one.
    points = [int(q) for q in args.cqp.split(",")] if args.cqp else (LADDER_720 if H == 720 else LADDER)

    clips = sorted(os.path.join(args.clips, c) for c in os.listdir(args.clips) if c.endswith(".y4m"))
    for clip in clips:
        name = os.path.splitext(os.path.basename(clip))[0]
        raw = raw_bgra(clip, out_dir, args.fps)
        ref = reference(raw, args.fps)
        for vendor in args.vendors.split(","):
            for codec in ("h264", "av1"):
                for point in points:
                    key = f"{name}/{vendor}/{codec}/{point}"
                    if key in results:
                        continue
                    dist = os.path.join(out_dir, f"{name}-{vendor}-{point}.{'obu' if codec == 'av1' else 'h264'}")
                    actual, ms = encode(raw, vendor, codec, 25000 if args.cqp else point, args.fps, dist,
                                        point if args.cqp else None)
                    mean, low = vmaf(dist, ref, dist + ".vmaf.json")
                    os.remove(dist)
                    results[key] = {"target": point, "kbps": actual, "vmaf": mean, "vmaf_1pct": low, "ms": ms}
                    print(f"{key}: {actual:.0f} kbps  VMAF {mean:.2f} (1% low {low:.2f})  {ms:.1f} ms/frame",
                          flush=True)
                    json.dump(results, open(results_path, "w"), indent=1)

    if args.cqp:
        print(f"\n{H}p{args.fps} CQP: Mbps and VMAF per clip")
        for clip in clips:
            name = os.path.splitext(os.path.basename(clip))[0]
            for k, v in results.items():
                if k.startswith(name + "/"):
                    print(f"  {k:32} {v['kbps'] / 1000:7.1f} Mbps  VMAF {v['vmaf']:.1f}")
        return 0

    if H != 1080 or args.fps != 60:
        # Another size against 1080p at its frame rate; another 1080p rate against 1080p60, whose
        # bitrates the presets name. Each is scored against its own source.
        ref_fps = args.fps if H != 1080 else 60
        ref_path = os.path.join(out_dir, f"results-{ref_fps}.json")
        if not os.path.exists(ref_path):
            print(f"no {ref_path}: run 1080p at {ref_fps} fps first for the comparison")
            return 0
        full = json.load(open(ref_path))
        ratios = []
        print(f"\n{H}p{args.fps} kbps matching VMAF of 1080p{ref_fps} at the presets' bitrates (ratio):")
        for clip in clips:
            name = os.path.splitext(os.path.basename(clip))[0]
            for vendor in args.vendors.split(","):
                for codec in ("h264", "av1"):
                    prefix = f"{name}/{vendor}/{codec}/"
                    c1080 = sorted((v["kbps"], v["vmaf"]) for k, v in full.items() if k.startswith(prefix))
                    c720 = sorted((v["kbps"], v["vmaf"]) for k, v in results.items() if k.startswith(prefix))
                    cells = []
                    for preset, kbps in PRESETS_1080[codec].items():
                        q = vmaf_at(c1080, kbps)
                        r = rate_for(c720, q) if q is not None else None
                        if r:
                            ratios.append(r / kbps)
                        cells.append(f"{preset} {q if q else float('nan'):.1f}: "
                                     f"{r / kbps if r else float('nan'):.2f}")
                    print(f"  {name:10} {vendor:6} {codec:5} " + "  ".join(cells))
        ratios.sort()
        if ratios:
            print(f"  median ratio {ratios[len(ratios) // 2]:.2f}, range {ratios[0]:.2f}-{ratios[-1]:.2f}, n={len(ratios)}")
        return 0

    print(f"\n{args.fps} fps. AV1 vs H.264 at equal VMAF (BD-rate), and kbps reaching VMAF 90/93/95:")
    for clip in clips:
        name = os.path.splitext(os.path.basename(clip))[0]
        for vendor in args.vendors.split(","):
            curves = {}
            for codec in ("h264", "av1"):
                curves[codec] = [(results[k]["kbps"], results[k]["vmaf"]) for k in results
                                 if k.startswith(f"{name}/{vendor}/{codec}/")]
            if not curves["h264"] or not curves["av1"]:
                continue
            bd = bd_rate(curves["h264"], curves["av1"])
            marks = []
            for t in (90, 93, 95):
                rh, ra = rate_for(curves["h264"], t), rate_for(curves["av1"], t)
                marks.append(f"{t}: {rh / 1000 if rh else float('nan'):.1f}/{ra / 1000 if ra else float('nan'):.1f}")
            print(f"  {name:10} {vendor:6} BD-rate {bd if bd is not None else float('nan'):+.1f}%   "
                  f"Mbps h264/av1  " + "  ".join(marks))


if __name__ == "__main__":
    sys.exit(main())

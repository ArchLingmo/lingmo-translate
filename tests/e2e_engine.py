#!/usr/bin/env python3
"""End-to-end test of the engine against an isolated PulseAudio/PipeWire-pulse server.

The caller exports PULSE_SERVER (a private test server, never the user's), and
LT_DATA (models). Speech files are played with paplay into the test sink or
the fake mic; latency is measured from the moment the speech really ended in
the played file to each event of the engine.

usage: e2e_engine.py MODE CONFIG.json wav1 [wav2 ...]   (prints a JSON report)
"""

import json
import os
import subprocess
import sys
import threading
import time
import wave

import numpy as np

TALK = False

ENGINE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "engine", "engine.py")


def speech_bounds(path):
    with wave.open(path, "rb") as w:
        sr = w.getframerate()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float32) / 32768.0
    win = int(sr * 0.01)
    n = x.size // win
    e = np.abs(x[: n * win]).reshape(n, win).max(axis=1)
    loud = np.where(e > e.max() * 0.03)[0]
    return loud[0] * 0.01, (loud[-1] + 1) * 0.01, x.size / sr


def speech_segments(path, min_gap=0.8):
    """(start, end) of each stretch of speech in a file with pauses."""
    with wave.open(path, "rb") as w:
        sr = w.getframerate()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float32) / 32768.0
    win = int(sr * 0.01)
    n = x.size // win
    e = np.abs(x[: n * win]).reshape(n, win).max(axis=1)
    loud = e > e.max() * 0.03
    segs, start, quiet = [], None, 0
    for i, v in enumerate(loud):
        if v:
            if start is None:
                start = i
            quiet = 0
        elif start is not None:
            quiet += 1
            if quiet * 0.01 >= min_gap:
                segs.append((start * 0.01, (i - quiet + 1) * 0.01))
                start, quiet = None, 0
    if start is not None:
        segs.append((start * 0.01, (n - quiet) * 0.01))
    return segs


class VolumeWatch(threading.Thread):
    """Lowest volume (percent) seen on the other apps' streams, to check the ducking."""

    def __init__(self):
        super().__init__(daemon=True)
        self.samples = []
        self.running = True

    def run(self):
        while self.running:
            try:
                out = json.loads(subprocess.run(["pactl", "-f", "json", "list", "sink-inputs"], capture_output=True, text=True).stdout or "[]")
                for si in out:
                    if si.get("properties", {}).get("application.name") == "Lingmo Tradutor":
                        continue
                    vols = [int(str(v.get("value_percent", "100%")).rstrip("%")) for v in si.get("volume", {}).values()]
                    if vols:
                        self.samples.append((time.time(), si["properties"].get("application.name"), min(vols)))
            except Exception:  # noqa: BLE001
                pass
            time.sleep(0.1)


def main():
    cfg = json.load(open(sys.argv[1]))
    wavs = sys.argv[2:]
    sink = cfg.pop("_playTo", "test_speakers")
    gap = float(cfg.pop("_gap", 1.5))
    global TALK
    TALK = bool(cfg.pop("_talk", False))
    wait_after = float(cfg.pop("_waitAfter", 6))
    py = cfg.pop("_python", sys.executable)
    vmic_out = cfg.pop("_vmicOut", None)
    vmic = []
    rec = None
    if vmic_out:
        # what the app does (src/virtualmic.cpp): null sink + remap source "Lingmo Tradutor"
        for args in (["module-null-sink", "sink_name=lingmo_tradutor_out", "rate=48000", "channels=1"],
                     ["module-remap-source", "master=lingmo_tradutor_out.monitor", "source_name=lingmo_tradutor", "channels=1"]):
            vmic.append(subprocess.run(["pactl", "load-module", *args], capture_output=True, text=True).stdout.strip())
        rec = subprocess.Popen(["parec", "--device=lingmo_tradutor", "--raw", "--format=s16le", "--rate=24000", "--channels=1"],
                               stdout=open(vmic_out, "wb"))
    eng = subprocess.Popen([py, ENGINE], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    events = []
    ready = threading.Event()

    def reader():
        for line in eng.stdout:
            try:
                ev = json.loads(line)
            except ValueError:
                continue
            ev["_rx"] = time.time()
            events.append(ev)
            if ev.get("event") == "state" and ev.get("stage") == "running":
                ready.set()
            if ev.get("event") in ("error", "log"):
                sys.stderr.write("engine: %s\n" % ev.get("message"))

    threading.Thread(target=reader, daemon=True).start()
    cfg["cmd"] = "start"
    eng.stdin.write(json.dumps(cfg) + "\n")
    eng.stdin.flush()
    if not ready.wait(300):
        print(json.dumps({"error": "engine did not start", "events": events[-5:]}))
        eng.kill()
        return 1
    start_ev = [e for e in events if e.get("stage") == "running"][-1]
    time.sleep(1.0)
    played = []
    watch = VolumeWatch()
    watch.start()
    talk = cfg.pop("_talk", False) if False else TALK
    if talk:
        w = wavs[0]
        segs = speech_segments(w)
        t = time.time()
        subprocess.run(["paplay", "--device=%s" % sink, "--client-name=Navegador de teste", w])
        for i, (s0, s1) in enumerate(segs):
            played.append({"file": "%s#%d" % (os.path.basename(w), i + 1), "playStart": t + s0, "speechEnd": t + s1,
                           "returned": t + s1 + 0.05})
        wavs = []
        time.sleep(wait_after)
    for w in wavs:
        s0, s1, dur = speech_bounds(w)
        t = time.time()
        subprocess.run(["paplay", "--device=%s" % sink, w])
        # paplay returns once the sink played it; speech ended at t_play + s1 (+ server latency)
        played.append({"file": os.path.basename(w), "playStart": t, "speechEnd": t + s1, "returned": time.time()})
        time.sleep(gap)
    time.sleep(wait_after)
    eng.stdin.write(json.dumps({"cmd": "stop"}) + "\n")
    eng.stdin.flush()
    time.sleep(1.5)
    eng.stdin.write(json.dumps({"cmd": "quit"}) + "\n")
    eng.stdin.flush()
    try:
        eng.wait(timeout=15)
    except subprocess.TimeoutExpired:
        eng.kill()

    vmic_report = None
    if rec is not None:
        rec.terminate()
        rec.wait()
        for m in reversed(vmic):
            subprocess.run(["pactl", "unload-module", m])
        left = subprocess.run(["pactl", "list", "short", "sources"], capture_output=True, text=True).stdout.count("lingmo_tradutor")
        raw = np.fromfile(vmic_out, dtype="<i2").astype(np.float32) / 32768.0
        voiced = float(np.mean(np.abs(raw) > 0.02)) if raw.size else 0.0
        vmic_report = {"seconds": round(raw.size / 24000, 1), "voicedFraction": round(voiced, 3),
                       "peak": round(float(np.abs(raw).max()) if raw.size else 0, 3), "sourcesLeftAfterRemove": left}
    # match each final to the file that was playing
    finals = [e for e in events if e.get("event") == "final"]
    spoken = [e for e in events if e.get("event") == "spoken"]
    partials = [e for e in events if e.get("event") == "partial" and e.get("original")]
    rows = []
    for p in played:
        mine = [f for f in finals if p["playStart"] - 0.1 <= f.get("speechEnd", f["t"]) <= p["returned"] + 1.2]
        if not mine:
            rows.append({"file": p["file"], "missing": True})
            continue
        f = mine[0]
        if len(mine) > 1:
            f = dict(f)
            f["original"] = " | ".join(m["original"] for m in mine)
            f["text"] = " | ".join(m["text"] for m in mine)
            f["t"] = mine[-1]["t"]
        row = {"file": p["file"], "pieces": len(mine), "original": f["original"], "text": f["text"], "engine": f["engine"], "_f": f,
               "finalMs": int((f["t"] - p["speechEnd"]) * 1000), "engineStages": f.get("latency", {})}
        first_partial = [x for x in partials if x.get("id") == f["id"]] or [x for x in partials if p["playStart"] < x["t"] < f["t"]]
        if first_partial:
            row["firstPartialAfterStartMs"] = int((first_partial[0]["t"] - p["playStart"]) * 1000)
        drafts = [d for d in events if d.get("event") == "draft" and d.get("id") == f["id"]]
        if drafts:
            row["draftMs"] = int((drafts[0]["t"] - p["speechEnd"]) * 1000)
        sp = [s for s in spoken if s.get("id") == f["id"]]
        if sp:
            row["voiceStartsMs"] = int((sp[0]["t"] + sp[0].get("startsIn", 0) - p["speechEnd"]) * 1000)
            row["ttsSynthMs"] = sp[0].get("synthMs")
            row["takes"] = [s.get("takes") for s in sp]
            row["spokenSeconds"] = round(sum(s.get("seconds", 0) for s in sp), 2)
        rows.append(row)
    for r in rows:
        r.pop("_f", None)
    stats = [e for e in events if e.get("event") == "stats"]
    watch.running = False
    ducked = [s for s in watch.samples if s[2] < 95]
    duck = {"minPercent": min([s[2] for s in watch.samples], default=None), "duckedSamples": len(ducked),
            "restored": bool(ducked) and any(s[0] > ducked[-1][0] and s[1] == ducked[-1][1] and s[2] >= 95 for s in watch.samples), "samples": len(watch.samples)}
    voice_state = [e for e in events if e.get("event") == "state" and e.get("voice")]
    print(json.dumps({"voice": voice_state[-1] if voice_state else None, "vmic": vmic_report, "duck": duck, "start": {k: start_ev.get(k) for k in ("readyS", "sttGpu", "sttLoadS", "mtEngine", "vad")},
                      "rows": rows, "stats": [s["latency"] for s in stats],
                      "errors": [e.get("message") for e in events if e.get("event") == "error"]}, ensure_ascii=False, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())

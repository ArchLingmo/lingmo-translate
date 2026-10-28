#!/usr/bin/env python3
# Copyright (C) 2026 LingmoOS Team.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# any later version.
"""Live translation engine of Lingmo ("Tradução ao vivo").

One long-lived process, started by lingmo-translate in its own virtualenv
(~/.local/share/lingmo/translate/venv: numpy, onnxruntime, ctranslate2,
sentencepiece). It talks JSON lines: commands on stdin, events on stdout.

    capture (parec, 16 kHz mono)          ─┐ one thread each, joined by queues,
      → VAD (Silero, 32 ms frames)          │ so listening, recognising,
      → segmenter (partials, finals)        │ translating and speaking overlap
      → STT (whisper-server, model warm)    │
      → MT (Opus-MT/CTranslate2 draft,      │
            qwen via Ollama when "quality") │
      → TTS (piper / espeak-ng / cloned     │
            Chatterbox or OmniVoice worker) │
        → pacat                             ─┘

Audio goes through the PulseAudio protocol (pactl/parec/pacat), which is
served by pipewire-pulse on ArchLingmo and by PulseAudio elsewhere.

Commands
  {"cmd": "start", ...session...}      see Session.__init__ for the keys
  {"cmd": "pause"} / {"cmd": "resume"} / {"cmd": "stop"}
  {"cmd": "lang", "stream": "in", "from": "ja"|"auto"}   switch the source language live
  {"cmd": "record.start", "source": "...", "out": "/path/voice.wav", "seconds": 30}
  {"cmd": "record.stop"}
  {"cmd": "profile.prepare", "wav": "...", "out": "/path/conds-v3.pt", "python": ..., "worker": ..., "model": "v3"}
  {"cmd": "quit"}

Events (all carry "t", wall-clock seconds)
  ready, state, partial, draft, final, spoken, stats, level, record.done,
  profile.ready, log, error
"""

import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
import uuid
import wave

SR = 16000
FRAME = 512  # 32 ms at 16 kHz: Silero's frame

_out_lock = threading.Lock()


def emit(event, **fields):
    fields["event"] = event
    fields.setdefault("t", round(time.time(), 4))
    line = json.dumps(fields, ensure_ascii=False)
    with _out_lock:
        sys.stdout.write(line + "\n")
        sys.stdout.flush()


def log(message):
    emit("log", message=str(message))


def now():
    return time.time()


# ------------------------------------------------------------------ VAD


class SileroVad:
    """Silero VAD v5 (MIT), ONNX, one 512-sample frame at a time (~0.1 ms on the CPU)."""

    def __init__(self, path):
        import numpy as np
        import onnxruntime as ort

        opts = ort.SessionOptions()
        opts.inter_op_num_threads = 1
        opts.intra_op_num_threads = 1
        self.np = np
        self.session = ort.InferenceSession(path, sess_options=opts, providers=["CPUExecutionProvider"])
        self.reset()

    def reset(self):
        np = self.np
        self.state = np.zeros((2, 1, 128), dtype=np.float32)
        self.context = np.zeros((1, 64), dtype=np.float32)
        self.sr = np.array(SR, dtype=np.int64)

    def __call__(self, frame):
        np = self.np
        x = np.concatenate([self.context, frame.reshape(1, -1)], axis=1)
        out, self.state = self.session.run(None, {"input": x, "state": self.state, "sr": self.sr})
        self.context = x[:, -64:]
        return float(out[0][0])


class EnergyVad:
    """Fallback when onnxruntime or the Silero model is missing: adaptive energy gate."""

    def __init__(self):
        import numpy as np

        self.np = np
        self.floor = 1e-4

    def reset(self):
        pass

    def __call__(self, frame):
        e = float(self.np.sqrt(self.np.mean(frame * frame)) + 1e-9)
        if e < self.floor * 2:
            self.floor = 0.95 * self.floor + 0.05 * e
        ratio = e / max(self.floor, 1e-5)
        return max(0.0, min(1.0, (ratio - 2.0) / 6.0))


# ------------------------------------------------------------------ segmenter


class Segmenter:
    """Turns VAD frames into utterances.

    While someone speaks, a partial is offered every `partial_ms` (the STT
    worker keeps only the newest), and the utterance is final after
    `end_silence_ms` of silence, or at `max_ms` (cut at the quietest recent
    frame so a word is not split)."""

    def __init__(self, on_partial, on_final, end_silence_ms=400, partial_ms=700, max_ms=7000,
                 start_prob=0.5, end_prob=0.35, min_speech_ms=220, preroll_ms=200, split_after_ms=0, split_silence_ms=200,
                 hold=None, hold_end_ms=0):
        import numpy as np

        self.np = np
        self.on_partial, self.on_final = on_partial, on_final
        self.end_frames = max(1, int(end_silence_ms / 32))
        # dialogue: once an utterance is this long, a shorter pause ends it (the next line of a
        # fast exchange starts its own translation instead of waiting for the whole exchange)
        self.split_frames = int(split_after_ms / 32) if split_after_ms else 0
        self.split_silence = max(1, int(split_silence_ms / 32))
        self.partial_frames = max(1, int(partial_ms / 32))
        # speaking: a pause after an unfinished sentence ("... e", "... porque", no final
        # punctuation in the latest partial) waits longer before the sentence is cut
        self.hold = hold
        self.hold_end_frames = int(hold_end_ms / 32) if hold_end_ms else 0
        self.max_frames = int(max_ms / 32)
        self.start_prob, self.end_prob = start_prob, end_prob
        self.min_frames = max(1, int(min_speech_ms / 32))
        self.preroll = int(preroll_ms / 32)
        self.history = []  # pre-roll frames
        self.reset()

    def reset(self):
        self.frames = []
        self.energies = []
        self.speaking = False
        self.silence = 0
        self.voiced = 0
        self.since_partial = 0
        self.start_t = 0.0
        self.last_voice_t = 0.0
        self.uid = None

    def push(self, frame, prob, t):
        np = self.np
        if not self.speaking:
            self.history.append(frame)
            if len(self.history) > self.preroll:
                self.history.pop(0)
            if prob >= self.start_prob:
                self.speaking = True
                self.uid = uuid.uuid4().hex[:8]
                self.frames = list(self.history)
                self.energies = [float(np.mean(f * f)) for f in self.frames]
                self.start_t = t - 0.032 * len(self.history)
                self.last_voice_t = t
                self.voiced = 1
                self.silence = 0
                self.since_partial = 0
                self.history = []
            return
        self.frames.append(frame)
        self.energies.append(float(np.mean(frame * frame)))
        self.since_partial += 1
        if prob >= self.end_prob:
            self.silence = 0
            self.voiced += 1
            self.last_voice_t = t
        else:
            self.silence += 1
        end = self.end_frames
        if self.hold_end_frames and self.silence >= end and self.hold is not None and self.hold(self.uid):
            end = self.hold_end_frames
        if self.silence >= end:
            self._finish(cut=None)
            return
        if self.split_frames and self.silence >= self.split_silence and len(self.frames) - self.silence >= self.split_frames:
            # a short pause in long speech: this piece goes now while the speaker goes on
            self._finish(cut=None, split=True)
            return
        if len(self.frames) >= self.max_frames:
            # cut at the quietest frame of the last 1.5 s, keep the rest for the next utterance
            look = min(len(self.frames) - 1, 47)
            tail = self.energies[-look:]
            cut = len(self.frames) - look + int(np.argmin(tail))
            self._finish(cut=cut)
            return
        if self.since_partial >= self.partial_frames and self.voiced >= self.min_frames:
            self.since_partial = 0
            self.on_partial(self.uid, np.concatenate(self.frames), self.start_t)

    def _finish(self, cut, split=False):
        np = self.np
        frames = self.frames if cut is None else self.frames[:cut]
        rest = [] if cut is None else self.frames[cut:]
        voiced = self.voiced
        uid, start_t, end_t = self.uid, self.start_t, self.last_voice_t
        if cut is not None:
            end_t = start_t + 0.032 * cut
        if cut is None and self.silence:
            frames = frames[: max(1, len(frames) - self.silence + 4)]  # keep ~130 ms of the pause
        self.reset()
        if voiced >= self.min_frames and frames:
            self.on_final(uid, np.concatenate(frames), start_t, end_t, cut is not None or split)
        if rest:  # speech continues: start the next utterance with what was cut off
            self.speaking = True
            self.uid = uuid.uuid4().hex[:8]
            self.frames = rest
            self.energies = [float(np.mean(f * f)) for f in rest]
            self.start_t = end_t
            self.last_voice_t = end_t + 0.032 * len(rest)
            self.voiced = len(rest)


# ------------------------------------------------------------------ audio I/O


def pa_env():
    env = dict(os.environ)
    env.setdefault("LC_ALL", "C")
    return env


# ------------------------------------------------------------------ resource guard
#
# Live translation must never take the computer down (no swap on many desktops):
# every model is checked against the free RAM/VRAM before it loads, only one
# large model loads at a time, and the model processes run in a memory-capped
# systemd scope when systemd is there.

NEEDS = {  # MB, measured on the target machine (README.md)
    # whisper small-q5_1; the app sends each model's figure (stt.minFreeMb: large-v3-turbo 1912 MB,
    # its q5_0 902 MB, hwprofile.h in the app)
    "whisper-gpu": {"ram": 900, "vram": 700},
    "whisper-cpu": {"ram": 1200, "vram": 0},
    "opus": {"ram": 400, "vram": 0},
    "clone": {"ram": 5000, "vram": 3600},
    "omnivoice": {"ram": 5000, "vram": 2600},  # Voice Studio's figure; peak 2.3 GB measured live
}
CAPS = {"whisper": "3G", "clone": "9G", "omnivoice": "9G", "piper": "1G"}
_big_model = threading.Lock()  # one large model loading at a time in this engine


class ResourceError(RuntimeError):
    pass


def mem_available_mb():
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) // 1024
    except OSError:
        pass
    return 1 << 20


def require(what, gpu=False, vram=None):
    need = dict(NEEDS[what])
    if vram:
        need["vram"] = int(vram)
    avail = mem_available_mb()
    if avail < need["ram"] + 1500:  # keep 1.5 GB for the desktop
        raise ResourceError("Memória insuficiente para carregar %s: %d MB livres, precisa de %d MB. Feche alguns programas."
                            % (what, avail, need["ram"] + 1500))
    if gpu and need["vram"]:
        free = gpu_free_mb()
        if free < need["vram"]:
            raise ResourceError("Memória de vídeo insuficiente para %s: %d MB livres, precisa de %d MB." % (what, free, need["vram"]))


def _die_with_parent():
    """Child processes (whisper-server, piper, the voice worker, parec/pacat) end with the engine."""
    try:
        import ctypes
        import signal

        ctypes.CDLL("libc.so.6", use_errno=True).prctl(1, signal.SIGTERM)  # PR_SET_PDEATHSIG
    except Exception:  # noqa: BLE001
        pass


def capped(cmd, cap):
    """Runs a model process inside a memory-capped user scope (killed alone if it overflows)."""
    if os.environ.get("LINGMO_TRANSLATE_NO_CAP") or not shutil.which("systemd-run"):
        return cmd
    if subprocess.run(["systemctl", "--user", "is-system-running"], capture_output=True, text=True).returncode not in (0, 1):
        return cmd
    return ["systemd-run", "--user", "--scope", "-q", "-p", "MemoryMax=%s" % cap, "-p", "MemorySwapMax=0", "--", *cmd]


def pactl_json(*args):
    try:
        out = subprocess.run(["pactl", "-f", "json", *args], capture_output=True, text=True, timeout=5, env=pa_env()).stdout
        return json.loads(out or "[]")
    except Exception:  # noqa: BLE001
        return []


def find_sink_inputs(match):
    """Sink inputs whose app name/binary/media name contains `match` (case-insensitive)."""
    found = []
    m = (match or "").lower()
    for si in pactl_json("list", "sink-inputs"):
        props = si.get("properties", {})
        if props.get("application.name") == "Lingmo Tradutor":
            continue
        hay = " ".join(str(props.get(k, "")) for k in ("application.name", "application.process.binary", "media.name", "application.id")).lower()
        if not m or m in hay:
            found.append(si)
    return found


def playing_stream():
    """The sink input that is playing right now (not ours, not paused), or None.
    Several at once: the most recent one (highest index)."""
    live = [si for si in find_sink_inputs("") if not si.get("corked")]
    return max(live, key=lambda si: si.get("index", 0)) if live else None


class Capture(threading.Thread):
    """parec → 512-sample float frames, with the wall time of each frame.

    `follow` (whole system while we speak into the same speakers): instead of the
    speakers' monitor, which also carries our voice, it listens to the stream that
    is playing (the video) and moves to another one when that one starts playing."""

    def __init__(self, source, on_frame, on_level=None):
        super().__init__(daemon=True)
        self.source = source
        self.on_frame = on_frame
        self.on_level = on_level
        self.proc = None
        self.running = True
        self.paused = False
        self.follow = False
        self.following = None  # index of the followed stream; None: the speakers' monitor

    def command(self):
        kind = self.source.get("kind", "system")
        cmd = ["parec", "--raw", "--format=s16le", "--rate=%d" % SR, "--channels=1", "--latency-msec=20",
               "--client-name=Lingmo Tradutor", "--stream-name=Escuta"]
        if kind == "app":
            idx = self.source.get("index")
            if idx is None:
                cand = find_sink_inputs(self.source.get("app", ""))
                if not cand:
                    return None
                idx = cand[0]["index"]
            cmd.append("--monitor-stream=%s" % idx)
        elif kind == "mic":
            cmd.append("--device=%s" % self.source.get("device", "@DEFAULT_SOURCE@"))
        elif self.follow and playing_stream() is not None:
            self.following = playing_stream()["index"]
            cmd.append("--monitor-stream=%s" % self.following)
        else:
            self.following = None
            cmd.append("--device=%s" % self.source.get("device", "@DEFAULT_MONITOR@"))
        return cmd

    def run(self):
        import numpy as np

        nbytes = FRAME * 2
        level_acc, level_n = 0.0, 0
        while self.running:
            cmd = self.command()
            if cmd is None:
                emit("state", capture="waiting", message="O app escolhido não está tocando som")
                time.sleep(1.0)
                continue
            self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=pa_env(), preexec_fn=_die_with_parent)
            emit("state", capture="on")
            buf = b""
            checked = now()
            while self.running:
                if self.follow and now() - checked > 2.0:
                    # another tab or app started playing: follow it
                    checked = now()
                    cur = playing_stream()
                    if cur is not None and cur["index"] != self.following:
                        break
                chunk = self.proc.stdout.read(nbytes - len(buf))
                if not chunk:
                    break
                buf += chunk
                if len(buf) < nbytes:
                    continue
                frame = np.frombuffer(buf, dtype="<i2").astype(np.float32) / 32768.0
                buf = b""
                t = now()
                if self.on_level is not None:
                    level_acc = max(level_acc, float(np.max(np.abs(frame))))
                    level_n += 1
                    if level_n >= 3:
                        self.on_level(level_acc)
                        level_acc, level_n = 0.0, 0
                if not self.paused:
                    self.on_frame(frame, t)
            if self.proc.poll() is None:
                self.proc.kill()
            self.proc.wait()
            if self.running:
                # the stream went away (a new video, the app closed): look for it again
                emit("state", capture="lost")
                time.sleep(0.5)
                if self.source.get("kind") == "app":
                    self.source.pop("index", None)

    def stop(self):
        self.running = False
        if self.proc and self.proc.poll() is None:
            self.proc.kill()


# Listening dubbed: a sentence that would start this many seconds after it was said is
# dropped (it stays in the subtitles) instead of pushing every later line further back
LISTEN_DROP_S = 7.0
# below that it speaks faster to catch up (time budget per line) rather than losing lines
LISTEN_TARGET_S = 1.5
MIN_PACE = 0.7  # piper length_scale × 0.7 (~1.4× faster) is still clear
# dubbing: the voice waits this long after the instant draft for the LLM's translation when
# it has nothing else to say (when it is busy, the LLM gets until the voice is free)
VOICE_WAIT_MS = 0
VOICE_WAIT_LONG_MS = 1600  # ...and for a long line (the LLM takes ~1.1 s after the draft)
SHORT_CJK_CHARS = 10  # "short line": up to 10 characters of Japanese/Chinese/Korean...
SHORT_WORDS = 5  # ...or 5 words
SYNTH_LEAD_S = 0.25  # synthesis starts this long before the voice is free (no gap)
PENDING = object()  # tts queue: "the draft or the refinement, whichever is there in time"
# rapid dialogue: a quality refinement this late (with newer lines waiting) keeps the draft
LLM_STALE_S = 3.0


class Player:
    """One pacat stream kept open for the whole session (no per-sentence start-up)."""

    def __init__(self, rate, device=None, role="phone", mirror=None):
        self.rate = rate
        cmd = ["pacat", "--playback", "--raw", "--format=s16le", "--rate=%d" % rate, "--channels=1",
               "--latency-msec=60", "--client-name=Lingmo Tradutor", "--stream-name=Voz traduzida",
               "--property=application.name=Lingmo Tradutor", "--property=media.role=%s" % role,
               "--property=application.icon_name=lingmo-translate"]
        if device:
            cmd.append("--device=%s" % device)
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stderr=subprocess.DEVNULL, env=pa_env(), preexec_fn=_die_with_parent)
        # `mirror`: the same voice also goes into the translator microphone, so OBS or a
        # call app can take the dubbed voice from "Lingmo Tradutor" as well
        self.mirror = None
        if mirror:
            mcmd = [c for c in cmd if not c.startswith(("--device=", "--property=media.role="))]
            mcmd += ["--device=%s" % mirror, "--property=media.role=phone"]
            self.mirror = subprocess.Popen(mcmd, stdin=subprocess.PIPE, stderr=subprocess.DEVNULL, env=pa_env(), preexec_fn=_die_with_parent)
        self.busy_until = 0.0
        self.lock = threading.Lock()

    def play(self, samples):
        """Queues the samples; returns the wall time they start playing."""
        import numpy as np

        pcm = (np.clip(samples, -1.0, 1.0) * 32767.0).astype("<i2").tobytes()
        with self.lock:
            start = max(now(), self.busy_until)
            self.busy_until = start + len(samples) / float(self.rate)
            for proc in (self.proc, self.mirror):
                if proc is None:
                    continue
                try:
                    proc.stdin.write(pcm)
                    proc.stdin.flush()
                except (BrokenPipeError, ValueError):
                    pass
        return start

    def idle(self):
        return now() >= self.busy_until

    def close(self):
        for proc in (self.proc, self.mirror):
            if proc is None:
                continue
            try:
                proc.stdin.close()
            except Exception:  # noqa: BLE001
                pass
            try:
                proc.wait(timeout=2)
            except Exception:  # noqa: BLE001
                proc.kill()


# The volumes the Ducker lowered, so a killed or crashed engine never leaves the user's
# audio at 30%: the next engine and the app (when the engine exits) put them back
DUCKED_FILE = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or "/tmp", "lingmo-translate-ducked.json")


def _app_key(si):
    props = si.get("properties", {})
    return props.get("application.process.binary") or props.get("application.name") or ""


def _volumes(si):
    return [int(str(v.get("value", 65536))) for v in si.get("volume", {}).values()] or [65536]


def _set_volume(idx, vols):
    subprocess.run(["pactl", "set-sink-input-volume", str(idx), *["%d" % v for v in vols]], capture_output=True, env=pa_env())


def _undo_ducking(saved):
    """Puts the saved volumes back. The audio server remembers each app's last volume
    (module-stream-restore): a stream the app made while it was lowered (a new video, a
    reloaded tab) starts lowered too, so those are found by app and level and raised."""
    by_app = {}
    for entry in saved.values():
        if entry.get("app"):
            by_app[entry["app"]] = entry
    current = {si["index"]: si for si in pactl_json("list", "sink-inputs")}
    for idx, entry in saved.items():
        if int(idx) in current:
            _set_volume(idx, entry["vols"])
    for idx, si in current.items():
        entry = by_app.get(_app_key(si))
        if entry is None or str(idx) in saved:
            continue
        low = [v * entry["level"] for v in entry["vols"]]
        if all(abs(a - b) <= 0.03 * 65536 for a, b in zip(_volumes(si), low)):
            _set_volume(idx, entry["vols"])


def restore_leftover_ducking():
    try:
        with open(DUCKED_FILE, encoding="utf-8") as f:
            saved = json.load(f)
    except (OSError, ValueError):
        return
    if saved and isinstance(next(iter(saved.values())), dict):
        _undo_ducking(saved)
    else:  # the older format: index → volumes
        for idx, vols in saved.items():
            _set_volume(idx, vols)
    try:
        os.remove(DUCKED_FILE)
    except OSError:
        pass


class Ducker:
    """Lowers (never mutes) the original audio while the translated voice speaks."""

    def __init__(self, source, level):
        self.source = source
        self.level = max(0.05, min(1.0, float(level)))
        self.saved = {}  # sink-input index (str) → {"vols", "app", "level"}
        self.lock = threading.Lock()
        self.active = False

    def targets(self):
        kind = self.source.get("kind")
        if kind == "app":
            return find_sink_inputs(self.source.get("app", "")) or []
        if kind == "system":
            return find_sink_inputs("")
        return []

    def duck(self):
        with self.lock:
            by_app = {e["app"]: e for e in self.saved.values() if e.get("app")}
            for si in self.targets():
                idx = str(si["index"])
                if idx in self.saved:
                    continue
                vols, app = _volumes(si), _app_key(si)
                known = by_app.get(app)
                if known is not None and all(abs(a - b * self.level) <= 0.03 * 65536 for a, b in zip(vols, known["vols"])):
                    # a new stream of an app already lowered: the audio server started it
                    # lowered, its real volume is the app's
                    self.saved[idx] = {"vols": known["vols"], "app": app, "level": self.level}
                    continue
                self.saved[idx] = {"vols": vols, "app": app, "level": self.level}
                _set_volume(idx, [int(v * self.level) for v in vols])
            self.active = bool(self.saved)
            self._persist()

    def _persist(self):
        try:
            if self.saved:
                with open(DUCKED_FILE, "w", encoding="utf-8") as f:
                    json.dump(self.saved, f)
            elif os.path.exists(DUCKED_FILE):
                os.remove(DUCKED_FILE)
        except OSError:
            pass

    def restore(self):
        with self.lock:
            _undo_ducking(self.saved)
            self.saved = {}
            self.active = False
            self._persist()


# ------------------------------------------------------------------ STT


def open_ended(text):
    """A transcript that trails off: "...", a comma, or a word no sentence ends with."""
    text = (text or "").strip()
    if not text:
        return False
    if text.endswith(("...", "…")) or text[-1] in ",;:-–":
        return True
    words = text.rstrip(".!?…").split()
    return bool(words) and words[-1].lower().strip(",") in CONTINUES


# Words a sentence does not end with (pt, en, es): the speaker paused to think
CONTINUES = {
    "e", "que", "mas", "porque", "pois", "de", "do", "da", "dos", "das", "para", "pra", "com", "o", "a", "os", "as",
    "um", "uma", "se", "quando", "então", "como", "no", "na", "em", "por", "pelo", "pela", "ou", "nem", "também",
    "and", "but", "so", "because", "the", "an", "to", "of", "with", "that", "which", "or", "in", "on", "for", "if",
    "y", "pero", "el", "la", "los", "las", "en", "con", "por", "para", "porque",
}

HALLUCINATIONS = {
    "thank you.", "thanks for watching!", "thank you for watching.", "obrigado.", "obrigada.",
    "legendas pela comunidade amara.org", "you", "bye.", "tchau.", "...", "[música]", "[music]",
    "(música)", "(music)", "♪", "subscribe", "inscreva-se no canal",
}


# Gasps and fillers of anime/drama dialogue: subtitled, but not dubbed (the original voice
# carries them, and saying "Ah." in Portuguese only pushes the next line back)
FILLERS = {"ん", "んん", "あ", "あっ", "ああ", "え", "えっ", "えー", "はぁ", "はあ", "は", "はっ", "おお", "おっ", "ふん", "ふっ",
           "へえ", "へぇ", "ほう", "うわ", "うわぁ", "わぁ", "わあ", "ぞっ", "くっ", "ちっ", "ひっ", "ううん", "うう", "と"}


def is_filler(text):
    return text.strip(" 。、.,!?！？…~ー") in FILLERS if text else False


class WhisperServer:
    """whisper-server with the model loaded once; GPU unless told otherwise."""

    def __init__(self, model, gpu=True, threads=6, binary="whisper-server", vram=None):
        import socket

        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        self.port = s.getsockname()[1]
        s.close()
        cmd = [binary, "-m", model, "--host", "127.0.0.1", "--port", str(self.port), "-t", str(threads),
               "-l", "auto", "-nt", "-nf", "-sns"]
        if not gpu:
            cmd.append("-ng")
        self.gpu = gpu
        self.model = model
        started = now()
        require("whisper-gpu" if gpu else "whisper-cpu", gpu=gpu, vram=vram)
        self.proc = subprocess.Popen(capped(cmd, CAPS["whisper"]), stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, preexec_fn=_die_with_parent)
        self.stderr_tail = []
        threading.Thread(target=self._drain, daemon=True).start()
        for _ in range(240):
            if self.proc.poll() is not None:
                raise RuntimeError("whisper-server saiu: %s" % " ".join(self.stderr_tail[-3:]))
            try:
                urllib.request.urlopen("http://127.0.0.1:%d/" % self.port, timeout=1).read()
                break
            except Exception:  # noqa: BLE001
                time.sleep(0.25)
        else:
            raise RuntimeError("whisper-server não respondeu")
        self.lock = threading.Lock()
        self.warm()
        self.load_s = now() - started

    def warm(self):
        """The first CUDA inferences build their kernels (seconds): pay that now, not on the first sentence."""
        import numpy as np

        rng = np.random.default_rng(0)
        for seconds in (1.0, 3.0, 6.0):
            n = int(SR * seconds)
            t = np.arange(n) / SR
            x = (0.05 * np.sin(2 * np.pi * 180 * t) * (1 + np.sin(2 * np.pi * 3 * t)) + 0.005 * rng.standard_normal(n)).astype(np.float32)
            try:
                self.transcribe(x, "en")
            except Exception:  # noqa: BLE001
                pass

    def _drain(self):
        for line in self.proc.stderr:
            self.stderr_tail.append(line.strip())
            del self.stderr_tail[:-20]

    def transcribe(self, samples, language="auto", prompt=""):
        import io

        import numpy as np

        pcm = (np.clip(samples, -1.0, 1.0) * 32767.0).astype("<i2").tobytes()
        wav = io.BytesIO()
        with wave.open(wav, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SR)
            w.writeframes(pcm)
        boundary = "lingmo" + uuid.uuid4().hex
        fields = {"response_format": "verbose_json", "language": language or "auto", "temperature": "0"}
        if prompt:
            fields["prompt"] = prompt
        body = b""
        for k, v in fields.items():
            body += ("--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n" % (boundary, k, v)).encode()
        body += ("--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.wav\"\r\nContent-Type: audio/wav\r\n\r\n" % boundary).encode()
        body += wav.getvalue() + ("\r\n--%s--\r\n" % boundary).encode()
        req = urllib.request.Request("http://127.0.0.1:%d/inference" % self.port, data=body,
                                     headers={"Content-Type": "multipart/form-data; boundary=%s" % boundary})
        with self.lock:
            r = json.load(urllib.request.urlopen(req, timeout=20))
        text = " ".join(str(r.get("text", "")).split())
        probs = r.get("language_probabilities") or {}
        lang, p = "", 0.0
        if probs:
            lang, p = max(probs.items(), key=lambda kv: kv[1])
        return text, lang, float(p)

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except Exception:  # noqa: BLE001
                self.proc.kill()


# ------------------------------------------------------------------ MT


class OpusMT:
    """Opus-MT (Marian) converted to CTranslate2 int8: ~60 ms a sentence on the CPU."""

    def __init__(self, path, prefix="", threads=4):
        import ctranslate2
        import sentencepiece as spm

        self.tr = ctranslate2.Translator(path, device="cpu", inter_threads=1, intra_threads=threads, compute_type="int8")
        self.src = spm.SentencePieceProcessor(model_file=os.path.join(path, "source.spm"))
        self.tgt = spm.SentencePieceProcessor(model_file=os.path.join(path, "target.spm"))
        self.prefix = prefix
        self.lock = threading.Lock()
        self.translate("ok")

    def translate(self, text):
        toks = self.src.encode(text, out_type=str)
        if self.prefix:
            toks = [self.prefix] + toks
        with self.lock:
            r = self.tr.translate_batch([toks], beam_size=2, max_decoding_length=200, repetition_penalty=1.1)
        return self.tgt.decode(r[0].hypotheses[0])


class PivotMT:
    """Two Opus-MT models through English (ja→en→pt) for the pairs without a direct model.

    Measured on Tatoeba/FLEURS ja→pt (chrF): pivot 54.3/44.4 against 46.8/40.5 for the
    direct Tatoeba jpn-por model, ~0.2 s for a short line on the CPU."""

    slow = True  # ~3× a direct model: not used for the partials while dubbing

    def __init__(self, first, second):
        self.first, self.second = first, second

    def translate(self, text):
        return self.second.translate(self.first.translate(text))


KANA = re.compile("[\u3040-\u30ff]")
HANGUL = re.compile("[\u1100-\u11ff\u3130-\u318f\uac00-\ud7af]")


def script_lang(text, lang):
    """Whisper mixes up Japanese, Chinese and Korean on noisy audio (anime music); the
    script it wrote settles it: kana only exist in Japanese, hangul only in Korean."""
    if lang in ("ja", "zh", "ko"):
        if KANA.search(text):
            return "ja"
        if HANGUL.search(text):
            return "ko"
    return lang


class LangVote:
    """Sticky source language while listening to one video: after a few confident,
    agreeing detections the language is used as a hint for every sentence (no more
    ja/zh flip-flops on a single word), and it is re-checked now and then."""

    def __init__(self, need=3, recheck=8):
        self.need, self.recheck = need, recheck
        self.votes = []
        self.lang = None
        self.since = 0
        self.dissent = 0

    def hint(self):
        """The language to ask whisper for, or None to let it detect."""
        if self.lang is None:
            return None
        self.since += 1
        if self.since >= self.recheck:
            self.since = 0
            return None  # this sentence detects again
        return self.lang

    def add(self, lang, p, asked):
        """Feeds one final's detection; returns the newly locked language (or None)."""
        if asked or not lang or p < 0.6:
            return None
        if self.lang is not None:
            self.dissent = self.dissent + 1 if lang != self.lang and p >= 0.8 else 0
            if self.dissent >= 2:  # really another language now: start over
                self.lang, self.votes, self.dissent = None, [], 0
            return None
        self.votes = (self.votes + [lang])[-4:]
        best = max(set(self.votes), key=self.votes.count)
        if self.votes.count(best) >= self.need:
            self.lang, self.since = best, 0
            return best
        return None


LANG_NAMES = {"en": "English", "pt": "Brazilian Portuguese", "es": "Spanish", "fr": "French", "it": "Italian", "de": "German",
              "ja": "Japanese", "zh": "Chinese", "ko": "Korean"}


class OllamaMT:
    """The quality translator: qwen3.5 (4B) without thinking, a strict, faithful prompt.

    Measured on 30 meeting sentences: chrF 75.0 en→pt / 80.3 pt→en against
    65.4 / 75.8 for Opus-MT, at ~1.1 s a sentence on an RTX 2060."""

    def __init__(self, model, host=None, keep_alive="30m"):
        host = host or os.environ.get("OLLAMA_HOST", "127.0.0.1:11434")
        if "://" not in host:
            host = "http://" + host
        self.base = host.rstrip("/")
        self.model = model
        self.keep_alive = keep_alive
        self.lock = threading.Lock()

    def _post(self, path, body, timeout):
        req = urllib.request.Request(self.base + path, data=json.dumps(body).encode(), headers={"Content-Type": "application/json"})
        return json.load(urllib.request.urlopen(req, timeout=timeout))

    def available(self):
        try:
            tags = json.load(urllib.request.urlopen(self.base + "/api/tags", timeout=2))
            return any(m.get("name") == self.model or m.get("model") == self.model for m in tags.get("models", []))
        except Exception:  # noqa: BLE001
            return False

    def loaded(self):
        try:
            ps = json.load(urllib.request.urlopen(self.base + "/api/ps", timeout=2))
            return any(m.get("name") == self.model or m.get("model") == self.model for m in ps.get("models", []))
        except Exception:  # noqa: BLE001
            return False

    def warm(self):
        # remember whether someone else had it loaded: then stopping must not unload it
        self.was_loaded = self.loaded()
        self._post("/api/generate", {"model": self.model, "prompt": "", "keep_alive": self.keep_alive}, 120)

    def release(self):
        if getattr(self, "was_loaded", True):
            return
        try:
            self._post("/api/generate", {"model": self.model, "keep_alive": 0}, 10)
        except Exception:  # noqa: BLE001
            pass

    def translate(self, text, src, tgt, previous="", video=False):
        s, t = LANG_NAMES.get(src, src), LANG_NAMES.get(tgt, tgt)
        if video:
            # a line of dialogue to be dubbed: natural, and about as short as the original
            system = ("Translate this line of %s dialogue from a video (anime, film or series) into natural spoken %s. "
                      "Keep the exact meaning and the tone, keep names, add no notes, and keep it about as short as the "
                      "original. Output only the translation." % (s, t))
        else:
            system = ("Translate this %s sentence from a video call into %s. Be faithful: keep the exact meaning, add nothing, "
                      "drop nothing, keep the same tone. Use everyday spoken %s and common tech/meeting terms (mute, screen, bug, "
                      "deadline). Output only the translation." % (s, t, t))
        prompt = text
        if previous:
            system += " The previous sentence (context only, do not translate it) was: \"%s\"" % previous
        body = {"model": self.model, "system": system, "prompt": prompt, "stream": False, "think": False,
                "keep_alive": self.keep_alive, "options": {"temperature": 0, "num_predict": 160}}
        # no num_ctx: a different context size makes Ollama reload the model (seconds, and it
        # disturbs whoever else uses it); the loaded instance is reused as it is
        with self.lock:
            r = self._post("/api/generate", body, 30)
        out = str(r.get("response", "")).strip().strip('"').strip()
        out = out.splitlines()[0].strip() if out else ""
        if out.endswith(")") and "(" not in text and "（" not in text:
            out = re.sub(r"\s*\([^()]*\)$", "", out).strip()  # "(continuação necessária…)": a note, not speech
        return out


# ------------------------------------------------------------------ TTS


def split_for_speech(text, limit=180):
    """Sentences (and long clauses) so the first one is spoken while the rest are made."""
    parts = re.split(r"(?<=[.!?…;:])\s+", " ".join(text.split()))
    out = []
    for p in parts:
        while len(p) > limit:
            cut = p.rfind(",", 0, limit)
            cut = cut if cut > 30 else p.rfind(" ", 0, limit)
            cut = cut if cut > 0 else limit
            out.append(p[: cut + 1].strip())
            p = p[cut + 1:].strip()
        if p:
            out.append(p)
    # The voice starts with the first clause: a slow voice (cloned) makes it in half the
    # time and the rest is made while it plays, so the wait after you stop is shorter
    if out and len(out[0]) > 50:
        cut = out[0].find(",", 15, 60)
        if cut > 0:
            out[0:1] = [out[0][: cut + 1].strip(), out[0][cut + 1:].strip()]
    return [p for p in out if any(c.isalnum() for c in p)]


def read_wav(path):
    import numpy as np

    with wave.open(path, "rb") as w:
        sr, ch, width = w.getframerate(), w.getnchannels(), w.getsampwidth()
        raw = w.readframes(w.getnframes())
    if width == 2:
        x = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    elif width == 4:
        x = np.frombuffer(raw, dtype="<f4").astype(np.float32)
    else:
        raise RuntimeError("unsupported wav width %d" % width)
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    return x, sr


def resample(x, sr, to):
    import numpy as np

    if sr == to or x.size == 0:
        return x
    n = int(round(x.size * to / float(sr)))
    return np.interp(np.linspace(0, x.size - 1, n), np.arange(x.size), x).astype(np.float32)


class PiperVoice:
    """piper (GPL-3.0 engine from the venv, voices CC0): in-process when the piper-tts
    package is there (no start-up per sentence), else a long-lived piper binary."""

    rate = 22050

    def __init__(self, model, workdir, binary="piper-tts", length_scale=1.0):
        with open(model + ".json", encoding="utf-8") as f:
            self.rate = int(json.load(f).get("audio", {}).get("sample_rate", 22050))
        self.lock = threading.Lock()
        self.proc = None
        self.voice = None
        self.length_scale = length_scale
        try:
            from piper import PiperVoice as _Piper

            self.voice = _Piper.load(model)
        except ImportError:
            self.workdir = workdir
            os.makedirs(workdir, exist_ok=True)
            self.proc = subprocess.Popen(capped([binary, "-m", model, "--json-input", "-q", "--sentence_silence", "0.05",
                                                 "--length_scale", str(length_scale), "-d", workdir], CAPS["piper"]),
                                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1,
                                         preexec_fn=_die_with_parent)
        self.synth("Ok.")  # loads the model now, not on the first sentence

    def synth(self, text, pace=1.0):
        import numpy as np

        if self.voice is not None:
            with self.lock:
                try:
                    from piper import SynthesisConfig

                    chunks = list(self.voice.synthesize(text, syn_config=SynthesisConfig(length_scale=self.length_scale * pace)))
                except ImportError:
                    chunks = list(self.voice.synthesize(text))
            if not chunks:
                return np.zeros(0, dtype=np.float32)
            return np.concatenate([c.audio_float_array for c in chunks]).astype(np.float32)
        path = os.path.join(self.workdir, "p%s.wav" % uuid.uuid4().hex[:10])
        with self.lock:
            self.proc.stdin.write(json.dumps({"text": text, "output_file": path}, ensure_ascii=False) + "\n")
            self.proc.stdin.flush()
            line = self.proc.stdout.readline()  # piper prints the file name when it is written
        if not line:
            raise RuntimeError("piper parou")
        x, sr = read_wav(path)
        os.unlink(path)
        return resample(x, sr, self.rate)

    def close(self):
        if self.proc is None:
            return
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=2)
        except Exception:  # noqa: BLE001
            self.proc.kill()


class EspeakVoice:
    rate = 22050

    def __init__(self, voice):
        self.voice = voice

    def synth(self, text):
        import io

        wav = subprocess.run(["espeak-ng", "-v", self.voice, "-s", "165", "--stdout", text], capture_output=True).stdout
        with wave.open(io.BytesIO(wav), "rb") as w:
            import numpy as np

            x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float32) / 32768.0
            return resample(x, w.getframerate(), self.rate)

    def close(self):
        pass


class CloneVoice:
    """The user's own voice: Chatterbox Multilingual in the voice venv (clone_worker.py),
    loaded once, conditionals read from the cached profile (no reference processing per sentence)."""

    rate = 24000

    def __init__(self, python, worker, conds, model="v3", device="auto", quality="quality"):
        env = dict(os.environ)
        env["PYTHONUNBUFFERED"] = "1"
        env["PYTHONDONTWRITEBYTECODE"] = "1"  # the environment is Lingmo Studio's: leave it as it is
        env.setdefault("HF_HUB_OFFLINE", "1")  # the model is already there (installed with the engine)
        # live use needs the GPU: on the CPU each sentence takes many seconds (and ~6 GB of RAM)
        require("clone", gpu=True)
        self.proc = subprocess.Popen(capped([python, worker], CAPS["clone"]), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     text=True, bufsize=1, env=env, preexec_fn=_die_with_parent)
        threading.Thread(target=self._drain, daemon=True).start()
        self.lock = threading.Lock()
        r = self.call({"cmd": "load", "model": model, "device": "cuda" if device == "auto" else device, "conds": conds, "quality": quality}, timeout=600)
        self.info = r
        self.device = r.get("device", "")

    def _drain(self):
        for line in self.proc.stderr:
            pass

    def call(self, msg, timeout=120):
        with self.lock:
            self.proc.stdin.write(json.dumps(msg, ensure_ascii=False) + "\n")
            self.proc.stdin.flush()
            while True:
                line = self.proc.stdout.readline()
                if not line:
                    raise RuntimeError("o motor de voz parou")
                try:
                    r = json.loads(line)
                except ValueError:
                    continue
                if r.get("event") == "log":
                    log(r.get("message"))
                    continue
                if r.get("event") == "error":
                    raise RuntimeError(r.get("message"))
                return r

    def synth(self, text, language="pt"):
        r = self.call({"cmd": "speak", "text": text, "language": language})
        x, sr = read_wav(r["file"])
        os.unlink(r["file"])
        self.last = r
        return resample(x, sr, self.rate)

    def close(self):
        try:
            self.proc.stdin.write(json.dumps({"cmd": "quit"}) + "\n")
            self.proc.stdin.flush()
            self.proc.wait(timeout=5)
        except Exception:  # noqa: BLE001
            self.proc.kill()


REF_TARGET_S = 8.0   # OmniVoice's reference: 8-12 s of the recording (k2-fsa suggests 3-10 s)
REF_LONGEST_S = 12.0


class OmniCloneVoice(CloneVoice):
    """The user's own voice through OmniVoice (k2-fsa): opt-in, non-commercial weights.

    Everything is Voice Studio's: the venv (~/.local/share/lingmo/omnivoice-venv), the
    worker (workers/omnivoice_worker.py, run from where Voice Studio installed it), the
    weights in the Hugging Face cache and the licence acceptance; this engine only
    starts the worker when the app found all of them. The voice clone prompt is made
    once from voice.wav (its transcript from our own whisper) and cached beside it as
    omnivoice-prompt.pt, the file Voice Studio uses too. Nothing is written into the
    audio (no watermark, no tag)."""

    rate = 24000

    def __init__(self, python, worker, wav, prompt, quality="quality", device="auto", transcribe=None):
        self.env = dict(os.environ)
        self.env["PYTHONUNBUFFERED"] = "1"
        self.env["PYTHONDONTWRITEBYTECODE"] = "1"  # Voice Studio's environment: left as it is
        self.env["HF_HUB_OFFLINE"] = "1"  # the weights are there already (Voice Studio downloaded them)
        self.python, self.worker = python, worker
        self.lock = threading.Lock()
        require("omnivoice", gpu=True)
        t = now()
        if not os.path.exists(prompt) or os.path.getmtime(prompt) < os.path.getmtime(wav):
            self._make_prompt(wav, prompt, transcribe)
        self._spawn()
        r = self.call({"cmd": "load", "device": "cuda" if device == "auto" else device}, timeout=600)
        self.device = r.get("device", "")
        # 32 flow steps is Voice Studio's quality; 16 halves the time for the "fast" choice
        self.steps = 16 if quality == "fast" else 32
        self.prompt = prompt
        # warm the GPU kernels so the first sentence is not slower than the rest
        tmp = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or "/tmp", "lingmo-translate-omni-%d.wav" % os.getpid())
        w = self.call({"cmd": "synth", "text": "Ok.", "out": tmp, "lang": "en", "prompt": prompt, "steps": self.steps})
        try:
            os.unlink(tmp)
        except OSError:
            pass
        self.info = {"vramMb": w.get("peakVramMb"), "warmS": round(now() - t, 1), "attempts": 1}

    def _spawn(self):
        self.proc = subprocess.Popen(capped([self.python, self.worker], CAPS["omnivoice"]), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True, bufsize=1, env=self.env, preexec_fn=_die_with_parent)
        threading.Thread(target=self._drain, daemon=True).start()

    def _make_prompt(self, wav, prompt, transcribe):
        """Once per recording, in a worker of its own on the CPU: encoding the reference
        audio takes ~2.5 GB of video memory on top of the model, more than a 6 GB card
        has beside whisper (out of memory on an RTX 2060), and ~20 s on the processor."""
        # OmniVoice wants a short reference (it is prepended to every sentence)
        ref = self._short_reference(wav)
        try:
            text = ""
            if transcribe is not None:
                try:
                    text = transcribe(ref)
                except Exception as e:  # noqa: BLE001
                    log("omnivoice: transcript: %s" % e)
            if not text:
                raise RuntimeError("não consegui transcrever a gravação da voz")
            emit("state", stage="loading", what="voice-prompt")
            self._spawn()
            try:
                self.call({"cmd": "load", "device": "cpu"}, timeout=900)
                self.call({"cmd": "prompt", "wav": ref, "text": text, "out": prompt}, timeout=900)
                os.chmod(prompt, 0o600)
            finally:
                self.close()
        finally:
            if ref != wav:
                os.unlink(ref)

    @staticmethod
    def _short_reference(wav, target_s=REF_TARGET_S, longest_s=REF_LONGEST_S):
        """voice.wav cut at the quietest moment between target_s and longest_s (a pause
        between sentences), as a private temporary file; voice.wav itself when short."""
        import numpy as np

        x, sr = read_wav(wav)
        if x.size <= longest_s * sr:
            return wav
        win = int(sr * 0.05)
        lo, hi = int(target_s * sr) // win, int(longest_s * sr) // win
        energy = np.abs(x[: hi * win]).reshape(hi, win).mean(axis=1)
        cut = (lo + int(np.argmin(energy[lo:hi]))) * win
        out = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or "/tmp", "lingmo-translate-ref-%d.wav" % os.getpid())
        fd = os.open(out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "wb") as f, wave.open(f, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(sr)
            w.writeframes((np.clip(x[:cut], -1.0, 1.0) * 32767.0).astype("<i2").tobytes())
        return out

    def synth(self, text, language="pt"):
        out = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or "/tmp", "lingmo-translate-omni-%d-%s.wav" % (os.getpid(), uuid.uuid4().hex[:8]))
        r = self.call({"cmd": "synth", "text": text, "out": out, "lang": base_lang(language), "prompt": self.prompt, "steps": self.steps})
        x, sr = read_wav(r["file"])
        os.unlink(r["file"])
        self.last = {"takes": 1, "ms": r.get("ms"), "rtf": r.get("rtf"), "peakVramMb": r.get("peakVramMb")}
        return resample(x, sr, self.rate)


# ------------------------------------------------------------------ session


def gpu_free_mb():
    try:
        out = subprocess.run(["nvidia-smi", "--query-gpu=memory.free", "--format=csv,noheader,nounits"],
                             capture_output=True, text=True, timeout=5).stdout.split()
        return int(out[0]) if out else 0
    except Exception:  # noqa: BLE001
        return 0


def base_lang(code):
    return (code or "").split("_")[0].split("-")[0].lower()


class Pipeline:
    """One direction: a source of speech → subtitles (and maybe a voice).

    Call mode runs two of these sharing the STT server and the translators."""

    def __init__(self, session, name, cfg):
        self.s = session
        self.name = name
        self.cfg = cfg
        self.source = dict(cfg.get("source", {"kind": "system"}))
        self.src_lang = base_lang(cfg.get("from", "auto")) or "auto"
        self.tgt_lang = base_lang(cfg.get("to", "pt"))
        self.locked_lang = None if self.src_lang == "auto" else self.src_lang
        self.last_lang = None
        self.speak = bool(cfg.get("speak", False))
        self.mt_mode = cfg.get("mt", "quality")
        self.votes = LangVote()
        self.previous = ""
        self.stt_q = queue.Queue()
        self.partial_slot = None
        self.partial_lock = threading.Lock()
        self.mt_q = queue.Queue()
        self.llm_q = queue.Queue()
        self.tts_q = queue.Queue()
        self.timings = {}
        self.running = True
        self.paused = False
        try:
            self.vad = SileroVad(session.paths["vad"])
            self.vad_kind = "silero"
        except Exception as e:  # noqa: BLE001
            log("VAD Silero indisponível (%s): usando energia" % e)
            self.vad = EnergyVad()
            self.vad_kind = "energy"
        self.seg = Segmenter(self.on_partial, self.on_final,
                             end_silence_ms=int(cfg.get("endSilenceMs", 400)),
                             partial_ms=int(cfg.get("partialMs", 700)),
                             max_ms=int(cfg.get("maxUtteranceMs", 7000)),
                             split_after_ms=int(cfg.get("splitAfterMs", 0)),
                             split_silence_ms=int(cfg.get("splitSilenceMs", 200)),
                             hold=self.unfinished, hold_end_ms=int(cfg.get("holdEndMs", 0)))
        self.last_partial = (None, "")
        self.player = None
        self.voice = None
        self.ducker = None
        self.half_duplex = False
        self.listening = False
        self.speak_draft = False
        self.voice_wait = 0.0
        self.voice_wait_long = 0.0
        self.sticky = False
        self.base_pace = 1.0
        self.sec_per_char = 0.065  # piper faber at pace 1, refined while it speaks
        self.capture = Capture(self.source, self.on_frame, self.on_level if cfg.get("levels") else None)

    # -- capture → VAD → segmenter (capture thread)
    def on_frame(self, frame, t):
        if self.paused:
            return
        if self.half_duplex and self.capture.following is None and self.player is not None and not self.player.idle():
            # the system output also carries our voice: do not translate ourselves
            self.seg.reset()
            return
        if not self.cfg.get("output") and self.s.speaking_to_mic(self):
            # call: my translated voice goes out through "Lingmo Tradutor" and comes back
            # (OBS monitoring, the call app's echo): never translate it as the other side
            self.seg.reset()
            return
        if self.source.get("kind") == "mic" and not self.cfg.get("headphones") and self.s.speaking_aloud(self):
            # call: the other side's dubbed voice comes out of the speakers into the microphone
            self.seg.reset()
            return
        if self.ducker is not None and self.ducker.active and (self.source.get("kind") == "app" or self.capture.following is not None):
            # a monitored stream is captured after its volume: undo the ducking for the recogniser
            frame = frame * (1.0 / self.ducker.level)
        self.seg.push(frame, self.vad(frame), t)

    def on_level(self, peak):
        emit("level", stream=self.name, peak=round(peak, 3))

    def on_partial(self, uid, audio, start_t):
        with self.partial_lock:
            self.partial_slot = (uid, audio, start_t)
        self.stt_q.put(("partial",))

    def unfinished(self, uid):
        """The latest partial of this utterance reads like a sentence that goes on."""
        puid, text = self.last_partial
        text = (text or "").strip()
        if puid != uid or not text:
            return False
        return text[-1] not in ".!?…" or open_ended(text)

    def on_final(self, uid, audio, start_t, end_t, forced):
        self.timings[uid] = {"speechStart": start_t, "speechEnd": end_t, "vadFinal": now(), "audioS": round(audio.size / SR, 2)}
        with self.partial_lock:
            if self.partial_slot and self.partial_slot[0] == uid:
                self.partial_slot = None
        self.stt_q.put(("final", uid, audio, forced))

    # -- STT worker: finals first, only the newest partial
    def stt_loop(self):
        hold_s = float(self.cfg.get("holdEndMs", 0)) / 1000.0
        pending = None  # an open-ended sentence waiting for its continuation: (uid, text, src, deadline)
        while self.running:
            try:
                job = self.stt_q.get(timeout=0.1)
            except queue.Empty:
                if pending and now() >= pending[3]:
                    self.mt_q.put((pending[0], pending[1], pending[2], False))  # no continuation came
                    pending = None
                continue
            if job[0] == "partial":
                if not self.stt_q.empty():
                    continue  # a final (or a newer partial) is waiting
                with self.partial_lock:
                    slot, self.partial_slot = self.partial_slot, None
                if not slot:
                    continue
                uid, audio, _ = slot
                try:
                    text, lang, p = self.s.stt.transcribe(audio, self.locked_lang or self.last_lang or "auto")
                except Exception as e:  # noqa: BLE001
                    log("stt parcial: %s" % e)
                    continue
                if not text or text.lower() in HALLUCINATIONS:
                    continue
                src = self.locked_lang or self.last_lang or script_lang(text, base_lang(lang)) or "en"
                draft = ""
                opus = self.s.opus_for(src, self.tgt_lang) if src != self.tgt_lang else None
                # dubbing: a slow (pivot) partial would hold the translator when the final comes
                if opus is not None and not (self.speak and getattr(opus, "slow", False)):
                    try:
                        draft = opus.translate(text)
                    except Exception:  # noqa: BLE001
                        pass
                self.last_partial = (uid, text)
                emit("partial", stream=self.name, id=uid, original=text, text=draft, lang=src)
                continue
            _, uid, audio, forced = job
            tm = self.timings.get(uid, {})
            # a video keeps its language: once it is clear, it is a hint (no ja/zh flip-flops);
            # a call stays free to switch (people change language mid-call)
            hint = None if self.locked_lang or not self.sticky else self.votes.hint()
            try:
                text, lang, p = self.s.stt.transcribe(audio, self.locked_lang or hint or "auto")
            except Exception as e:  # noqa: BLE001
                emit("error", message="Reconhecimento de fala falhou: %s" % e)
                continue
            tm["sttDone"] = now()
            if not text or text.lower().strip(" .!") in {h.strip(" .!") for h in HALLUCINATIONS}:
                emit("partial", stream=self.name, id=uid, original="", text="", lang="", dropped=True)
                continue
            if hint:
                lang, p = hint, 1.0
            lang = script_lang(text, base_lang(lang))
            if self.sticky and not self.locked_lang:
                locked = self.votes.add(lang, p, bool(hint))
                if locked:
                    emit("state", stream=self.name, detected=locked, sticky=True)
            src = self.locked_lang or lang or self.last_lang or "en"
            if not self.locked_lang and p >= 0.6:
                # finals always detect (people switch languages in a call); partials reuse this
                if src != self.last_lang:
                    emit("state", stream=self.name, detected=src)
                self.last_lang = src
            if pending:
                # the continuation of the sentence that trailed off: translated as one
                text = pending[1].rstrip(".… ") + " " + text
                pending = None
            if hold_s and not forced and open_ended(text):
                pending = (uid, text, src, now() + hold_s)
                continue
            self.mt_q.put((uid, text, src, forced))

    # -- MT: an instant Opus-MT draft, then (quality) the LLM refinement on its own thread,
    #    so a slow refinement never delays the next draft
    def mt_loop(self):
        while self.running:
            try:
                uid, text, src, forced = self.mt_q.get(timeout=0.2)
            except queue.Empty:
                continue
            tm = self.timings.get(uid, {})
            tgt = self.tgt_lang
            if src == tgt:
                tm["mtDraft"] = tm["mtFinal"] = now()
                self._final(uid, text, text, src, tgt, "none", forced)
                continue
            opus = self.s.opus_for(src, tgt)
            draft = ""
            if opus is not None:
                try:
                    draft = opus.translate(text)
                except Exception as e:  # noqa: BLE001
                    log("opus: %s" % e)
            tm["mtDraft"] = now()
            tm["lang"] = src
            use_llm = self.mt_mode == "quality" and self.s.llm is not None and self.s.llm_ok
            if use_llm or not draft:
                if draft:
                    emit("draft", stream=self.name, id=uid, original=text, text=draft, lang=src, target=tgt,
                         latency=self._latency(tm))
                    if self.speak_draft and not is_filler(text):
                        # dubbing keeps up with the picture: the voice does not wait for the LLM
                        # (tts_loop picks the refined text only if it is there in time)
                        tm["spoken"] = "pending"
                        tm["draftText"] = draft
                        tm["srcText"] = text
                        self.tts_q.put((uid, PENDING, tgt))
                        self.tts_q.put((uid, None, tgt))
                self.llm_q.put((uid, text, src, tgt, draft, forced))
            else:
                tm["mtFinal"] = tm["mtDraft"]
                self._final(uid, text, draft, src, tgt, "opus", forced)

    def llm_loop(self):
        while self.running:
            try:
                uid, text, src, tgt, draft, forced = self.llm_q.get(timeout=0.2)
            except queue.Empty:
                continue
            tm = self.timings.get(uid, {})
            out, engine = draft, "opus"
            stale = draft and not self.llm_q.empty() and now() - tm.get("speechEnd", now()) > LLM_STALE_S
            if stale:
                # fast dialogue: the refinement fell behind; keep the draft and catch up
                pass
            elif self.s.llm is not None and self.s.llm_ok:
                try:
                    # in a video the lines are different people talking: the previous line as
                    # context made the LLM translate it again instead of the current one
                    out = self.s.llm.translate(text, src, tgt, "" if self.sticky else self.previous, video=self.sticky) or draft
                    engine = "llm"
                except Exception as e:  # noqa: BLE001
                    log("tradução de qualidade indisponível (%s): usando Opus-MT" % e)
                    self.s.llm_ok = False
                    emit("state", notice="Modelo de tradução de qualidade indisponível: usando Opus-MT")
            tm["mtFinal"] = now()
            self._final(uid, text, out, src, tgt, engine, forced)

    def _final(self, uid, text, out, src, tgt, engine, forced):
        tm = self.timings.get(uid, {})
        self.previous = text
        tm["original"], tm["text"] = text, out
        tm["engine"] = engine  # after the text: the voice thread reads engine, then text
        emit("final", stream=self.name, id=uid, original=text, text=out, lang=src, target=tgt, engine=engine,
             continues=bool(forced), latency=self._latency(tm), speechStart=round(tm.get("speechStart", 0), 3),
             speechEnd=round(tm.get("speechEnd", 0), 3))
        if self.speak and self.listening and is_filler(text) and not tm.get("spoken"):
            tm["spoken"] = "filler"  # subtitled only
            self.s.report(self.name, uid, tm)
        elif self.speak and out and not tm.get("spoken"):
            tm["spoken"] = engine
            self._speak(uid, out, tgt)
        elif not self.speak:
            self.s.report(self.name, uid, tm)
        elif tm.get("ttsDone"):
            self.s.report(self.name, uid, tm)  # the voice finished before the refined subtitle came

    def _speak(self, uid, out, tgt):
        tm = self.timings.get(uid, {})
        tm["ttsChars"] = len(out)
        for sentence in split_for_speech(out):
            self.tts_q.put((uid, sentence, tgt))
        self.tts_q.put((uid, None, tgt))  # end of this utterance

    # -- TTS worker: sentence by sentence; synthesis of the next overlaps playback of the current
    def tts_loop(self):
        self.restore_at = 0.0
        while self.running:
            try:
                uid, sentence, lang = self.tts_q.get(timeout=0.1)
            except queue.Empty:
                if self.ducker and self.restore_at and now() >= self.restore_at and self.player.idle():
                    self.ducker.restore()
                    self.restore_at = 0.0
                continue
            tm = self.timings.get(uid, {})
            if sentence is None:
                self.restore_at = self.player.busy_until + 0.6
                tm["ttsDone"] = True
                if "mtFinal" in tm or not self.speak_draft:
                    self.s.report(self.name, uid, tm)
                continue
            if sentence is PENDING:
                # dubbing with the quality translator: say the refined translation when it
                # costs no time (it came while the voice was still busy, or within a short
                # grace), else the instant one, so the voice never waits for the LLM
                for part in split_for_speech(self._choose_text(tm)):
                    self._say(uid, part, lang, tm)
                continue
            self._say(uid, sentence, lang, tm)

    def _choose_text(self, tm):
        # a short line ("Só seis?!") is said at once: the instant translation is as good there and
        # rapid exchanges are made of them; a long one is worth waiting for the LLM (the instant
        # translator loses the meaning of long, noisy lines: chrF 17.7 vs 28.1 on the anime test)
        src = tm.get("srcText", "")
        short = len(src) <= SHORT_CJK_CHARS if base_lang(tm.get("lang", "")) in ("ja", "zh", "ko") else len(src.split()) <= SHORT_WORDS
        wait = self.voice_wait if short else self.voice_wait_long
        deadline = max(tm.get("mtDraft", now()) + wait, self.player.busy_until - SYNTH_LEAD_S)
        while self.running and now() < deadline and tm.get("engine") != "llm" and "mtFinal" not in tm:
            time.sleep(0.02)
        if tm.get("engine") == "llm" and tm.get("text"):
            tm["spoken"] = "llm"
            text = tm["text"]
        else:
            tm["spoken"] = "opus"
            text = tm.get("draftText", "")
        tm["ttsChars"] = len(text)
        return text

    def _say(self, uid, sentence, lang, tm):
        # Listening to a video or an anime the voice must keep up with the picture: when
        # it falls behind it speaks faster, and a sentence that would come far too late
        # is left to the subtitles (a call or the microphone never skips anything)
        pace = self.base_pace
        if self.listening and tm.get("speechEnd"):
            late = max(now(), self.player.busy_until) - tm["speechEnd"]
            if late > LISTEN_DROP_S:
                tm["skipped"] = tm.get("skipped", 0) + 1
                tm["late"] = round(late, 2)
                emit("skipped", stream=self.name, id=uid, text=sentence, late=round(late, 1))
                return
            if "budgetPace" not in tm:
                tm["budgetPace"] = self._budget_pace(tm, late, self._newer_waiting(uid))
            pace = self.base_pace * tm["budgetPace"]
            tm["late"] = max(tm.get("late", 0), round(late, 2))
        tm["pace"] = round(min(tm.get("pace", 9), pace), 2)
        t0 = now()
        try:
            if isinstance(self.voice, CloneVoice):
                x = self.voice.synth(sentence, lang)
            elif isinstance(self.voice, PiperVoice):
                x = self.voice.synth(sentence, pace)
            else:
                x = self.voice.synth(sentence)
        except Exception as e:  # noqa: BLE001
            emit("error", message="A voz falhou: %s" % e)
            return
        synth_s = now() - t0
        if isinstance(self.voice, PiperVoice) and len(sentence) > 8 and x.size:
            # how long this voice takes per character at pace 1 (for the time budget)
            spc = x.size / float(self.voice.rate) / pace / len(sentence)
            self.sec_per_char = 0.8 * self.sec_per_char + 0.2 * spc
        start = self.player.play(x)
        if self.ducker:
            self.ducker.duck()  # pactl takes ~20 ms: after the samples are queued, not before
        tm.setdefault("ttsFirst", t0 + synth_s)
        tm.setdefault("playStart", start)
        tm.setdefault("ttsSynthS", 0.0)
        tm["ttsSynthS"] += synth_s
        emit("spoken", stream=self.name, id=uid, text=sentence, seconds=round(x.size / self.voice.rate, 2),
             synthMs=int(synth_s * 1000), startsIn=round(start - now(), 3),
             takes=getattr(self.voice, "last", {}).get("takes") if isinstance(self.voice, CloneVoice) else None)
        self.restore_at = 0.0

    def _budget_pace(self, tm, late, newer):
        """Dubbing a fast dialogue: the translation should not take (much) longer to say than
        the original took, and the voice catches up when it is behind, by speaking faster
        (down to MIN_PACE) instead of skipping lines."""
        text_s = tm.get("ttsChars", 0) * self.sec_per_char
        if text_s <= 0:
            return 1.0
        behind = max(0.0, late - LISTEN_TARGET_S)
        allowed = tm.get("audioS", text_s) + 0.4 - 0.6 * behind
        if newer:
            allowed -= 0.3
        return round(max(MIN_PACE, min(1.0, allowed / text_s)), 2)

    def _newer_waiting(self, uid):
        """Is a later utterance already waiting to be spoken?"""
        with self.tts_q.mutex:
            return any(item[0] != uid for item in list(self.tts_q.queue))

    def _latency(self, tm):
        end = tm.get("speechEnd")
        out = {}
        if not end:
            return out
        for k in ("vadFinal", "sttDone", "mtDraft", "mtFinal", "ttsFirst", "playStart"):
            if k in tm:
                out[k] = int((tm[k] - end) * 1000)
        return out

    def start(self):
        if self.speak:
            vcfg = self.cfg.get("voice", {})
            kind = vcfg.get("kind", "piper")
            if kind == "clone":
                self.voice = self.s.clone_voice(vcfg)
            elif kind == "piper" and vcfg.get("model") and os.path.exists(vcfg["model"]):
                self.voice = PiperVoice(vcfg["model"], os.path.join(self.s.workdir, "piper-" + self.name),
                                        binary=self.s.paths.get("piper", "piper-tts"), length_scale=float(vcfg.get("lengthScale", 1.0)))
            else:
                self.voice = EspeakVoice(vcfg.get("espeak", "pt-br" if self.tgt_lang == "pt" else self.tgt_lang))
            self.player = Player(self.voice.rate, self.cfg.get("output") or None,
                                 role="phone" if self.cfg.get("output") else "accessibility", mirror=self.cfg.get("mirror"))
            duck = self.cfg.get("duck")
            if duck is not None and self.source.get("kind") in ("app", "system"):
                self.ducker = Ducker(self.source, duck)
            self.listening = self.source.get("kind") in ("app", "system") and not self.cfg.get("output")
            self.speak_draft = bool(self.cfg.get("speakDraft", self.listening))
            # how long the voice may wait for the refined translation when it is idle (ms)
            self.voice_wait = float(self.cfg.get("voiceWaitMs", VOICE_WAIT_MS)) / 1000.0
            self.voice_wait_long = float(self.cfg.get("voiceWaitLongMs", VOICE_WAIT_LONG_MS)) / 1000.0
            self.base_pace = float(self.cfg.get("pace", 1.0))
            # system output carries our own voice when we speak to the same speakers
            self.half_duplex = self.source.get("kind") == "system" and not self.cfg.get("output")
            # listen to the playing video itself, so nothing it says is lost while we speak
            self.capture.follow = self.half_duplex
        # a video (subtitles or dubbed, not a call) keeps one language: sticky detection
        self.sticky = self.source.get("kind") in ("app", "system") and not self.cfg.get("output") \
            and self.s.cfg.get("mode", "subtitles") in ("subtitles", "dubbed")
        for fn in (self.stt_loop, self.mt_loop, self.llm_loop) + ((self.tts_loop,) if self.speak else ()):
            threading.Thread(target=fn, daemon=True).start()
        self.capture.start()

    def stop(self):
        self.running = False
        self.capture.stop()
        if self.ducker:
            self.ducker.restore()
        if self.player:
            self.player.close()
        if self.voice and not isinstance(self.voice, CloneVoice):
            self.voice.close()


class Session:
    def __init__(self, cfg):
        """cfg keys:
            mode            subtitles | dubbed | speak | call
            paths           {vad, whisperModel, mt: {"en-pt": {"path", "prefix"}, ...}, piper, workdir}
            stt             {"gpu": true|false|"auto", "threads": 6}
            mt              quality | fast
            llmModel        qwen3.5:4b
            streams         [ {source, from, to, speak, voice, output, duck, endSilenceMs}, ... ]
        """
        self.cfg = cfg
        self.paths = cfg.get("paths", {})
        self.workdir = self.paths.get("workdir") or os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "lingmo-translate")
        os.makedirs(self.workdir, exist_ok=True)
        self.stt = None
        self.opus = {}
        self._opus_cache = {}
        self._pivots = {}
        self.llm = None
        self.llm_ok = False
        self.clone = None
        self.pipelines = []
        self.stats = []

    def speaking_to_mic(self, asking, tail=1.0):
        """My translated voice is playing into the translator microphone (or just did:
        `tail` covers the monitoring and network delay of whatever plays it back)."""
        return any(p is not asking and p.player is not None and p.cfg.get("output") and now() < p.player.busy_until + tail
                   for p in self.pipelines)

    def speaking_aloud(self, asking):
        """Another pipeline is playing a voice on the speakers right now."""
        return any(p is not asking and p.player is not None and not p.cfg.get("output") and not p.player.idle()
                   for p in self.pipelines)

    def opus_for(self, src, tgt):
        """The direct Opus-MT model, else src→en→tgt through two of them (ja/zh/ko → pt)."""
        direct = self.opus.get("%s-%s" % (src, tgt))
        if direct is not None or "en" in (src, tgt):
            return direct
        key = "%s-en-%s" % (src, tgt)
        if key not in self._pivots:
            a, b = self.opus.get("%s-en" % src), self.opus.get("en-%s" % tgt)
            self._pivots[key] = PivotMT(a, b) if a is not None and b is not None else None
        return self._pivots[key]

    def clone_voice(self, vcfg):
        if self.clone is not None:
            return self.clone
        emit("state", stage="loading", what="voice")
        if not _big_model.acquire(blocking=False):
            raise ResourceError("Outro modelo grande está carregando (perfil de voz): tente de novo em instantes.")
        try:
            return self._clone_voice(vcfg)  # the lock stays held while the voice is loaded (released in stop)
        except Exception:
            _big_model.release()
            raise

    def _transcribe_file(self, path):
        x, sr = read_wav(path)
        text, _lang, _p = self.stt.transcribe(resample(x, sr, SR))
        return text

    def _clone_voice(self, vcfg):
        if self.clone is None:
            t = now()
            if vcfg.get("engine") == "omnivoice":
                self.clone = OmniCloneVoice(vcfg["python"], vcfg["worker"], vcfg["wav"], vcfg["prompt"], vcfg.get("quality", "quality"),
                                            vcfg.get("device", "auto"), transcribe=self._transcribe_file)
            else:
                self.clone = CloneVoice(vcfg["python"], vcfg["worker"], vcfg["conds"], vcfg.get("model", "v3"),
                                        vcfg.get("device", "auto"), vcfg.get("quality", "quality"))
            emit("state", voice="clone", engine=vcfg.get("engine", "chatterbox"), device=self.clone.device, loadS=round(now() - t, 1),
                 vramMb=self.clone.info.get("vramMb"), warmS=self.clone.info.get("warmS"), takes=self.clone.info.get("attempts"))
        return self.clone

    def start(self):
        t0 = now()
        mode = self.cfg.get("mode", "subtitles")
        stt = self.cfg.get("stt", {})
        want_gpu = stt.get("gpu", "auto")
        model = self.paths["whisperModel"]
        free = gpu_free_mb()
        need_vram = int(stt.get("minFreeMb", 700))
        gpu = bool(want_gpu) if want_gpu != "auto" else free >= need_vram
        emit("state", vramFreeMb=free)
        if not gpu and self.paths.get("whisperCpuModel"):
            model = self.paths["whisperCpuModel"]
        emit("state", stage="loading", what="stt", gpu=gpu, model=os.path.basename(model))
        binary = self.paths.get("whisperServer", "whisper-server")
        try:
            self.stt = WhisperServer(model, gpu=gpu, threads=int(stt.get("threads", 6)), binary=binary,
                                     vram=need_vram if gpu and model == self.paths["whisperModel"] else None)
        except ResourceError:
            if not gpu:
                raise
            # no room on the GPU: the CPU model (slower, ~2 s more per sentence)
            emit("state", notice="GPU sem memória livre: reconhecimento de fala no processador (mais lento)")
            gpu = False
            self.stt = WhisperServer(self.paths.get("whisperCpuModel") or model, gpu=False, threads=int(stt.get("threads", 6)), binary=binary)
        for pair, m in (self.paths.get("mt") or {}).items():
            if os.path.isdir(m.get("path", "")):
                key = os.path.realpath(m["path"]) + m.get("prefix", "")
                if key in self._opus_cache:  # one model serves several pairs: load it once
                    self.opus[pair] = self._opus_cache[key]
                    continue
                require("opus")
                self.opus[pair] = self._opus_cache[key] = OpusMT(m["path"], m.get("prefix", ""), threads=int(self.cfg.get("mtThreads", 4)))
        if self.cfg.get("mt", "quality") == "quality" and self.cfg.get("llmModel"):
            self.llm = OllamaMT(self.cfg["llmModel"], keep_alive=self.cfg.get("llmKeepAlive", "30m"))
            self.llm_ok = self.llm.available()
            if self.llm_ok:
                emit("state", stage="loading", what="llm")
                try:
                    self.llm.warm()
                except Exception as e:  # noqa: BLE001
                    log("ollama: %s" % e)
                    self.llm_ok = False
            if not self.llm_ok:
                emit("state", notice="Modelo de tradução de qualidade indisponível: usando Opus-MT")
        for i, scfg in enumerate(self.cfg.get("streams", [])):
            p = Pipeline(self, scfg.get("name", "s%d" % i), scfg)
            p.start()
            self.pipelines.append(p)
        emit("state", stage="running", mode=mode, readyS=round(now() - t0, 2), sttGpu=gpu, sttLoadS=round(self.stt.load_s, 2),
             mtEngine="llm" if self.llm_ok else "opus", vad=self.pipelines[0].vad_kind if self.pipelines else "")

    def report(self, stream, uid, tm):
        end = tm.get("speechEnd")
        if not end or tm.get("reported"):
            return
        tm["reported"] = True
        row = {k: round((tm[k] - end) * 1000) for k in ("vadFinal", "sttDone", "mtDraft", "mtFinal", "ttsFirst", "playStart") if k in tm}
        extra = {k: tm[k] for k in ("lang", "skipped", "late", "pace", "spoken", "engine", "audioS") if k in tm}
        emit("stats", stream=stream, id=uid, latency=row, **extra)
        if STATS_PATH:
            write_stats(dict(stream=stream, id=uid, mode=self.cfg.get("mode", ""), latency=row, **extra), tm)

    def set_lang(self, stream, lang):
        """Changes the source language of a running stream (whisper, the translators and
        the voice stay loaded): a code fixes it, "auto" goes back to detecting."""
        lang = base_lang(lang) or "auto"
        for p in self.pipelines:
            if p.name != stream:
                continue
            p.src_lang = lang
            p.locked_lang = None if lang == "auto" else lang
            p.last_lang = None
            p.votes = LangVote()
            emit("state", stream=p.name, source=lang, detected=p.locked_lang or "")

    def pause(self, on):
        for p in self.pipelines:
            p.paused = on
            p.capture.paused = on
            if on:
                p.seg.reset()

    def stop(self):
        for p in self.pipelines:
            p.stop()
        if self.clone:
            self.clone.close()
            self.clone = None
            try:
                _big_model.release()
            except RuntimeError:
                pass
        if self.stt:
            self.stt.close()
        if self.llm and self.llm_ok and self.cfg.get("llmReleaseOnStop", True):
            self.llm.release()
        shutil.rmtree(self.workdir, ignore_errors=True)


# Opt-in latency log, local only, to check the timings on one's own videos:
#   LINGMO_TRANSLATE_STATS=/path/file.jsonl  one JSON line per utterance (stages in ms after
#                                            the end of speech, skipped/late/pace, MT engine)
#   LINGMO_TRANSLATE_STATS_TEXT=1            also the recognised and translated text
# Audio is never written. Read it with build-agent-translate/bench/stats_report.py.
STATS_PATH = os.environ.get("LINGMO_TRANSLATE_STATS", "")
STATS_TEXT = os.environ.get("LINGMO_TRANSLATE_STATS_TEXT") == "1"
_stats_lock = threading.Lock()


def write_stats(row, tm):
    row = dict(row, t=round(now(), 3))
    row.setdefault("skipped", 0)
    if STATS_TEXT:
        row["original"], row["text"] = tm.get("original", ""), tm.get("text", "")
    try:
        with _stats_lock:
            fd = os.open(STATS_PATH, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
            with os.fdopen(fd, "a", encoding="utf-8") as f:
                f.write(json.dumps(row, ensure_ascii=False) + "\n")
    except OSError as e:
        log("stats: %s" % e)


# ------------------------------------------------------------------ voice profile recording


class Recorder(threading.Thread):
    """Guided recording of the voice profile, with a level meter and a quality check."""

    def __init__(self, source, out, seconds):
        super().__init__(daemon=True)
        self.out = out
        self.seconds = float(seconds)
        self.source = source
        self.frames = []
        self.stopped = False

    def run(self):
        import numpy as np

        cap = Capture({"kind": "mic", "device": self.source or "@DEFAULT_SOURCE@"}, self.on_frame, None)
        self.cap = cap
        cap.start()
        start = now()
        while not self.stopped and now() - start < self.seconds:
            time.sleep(0.05)
        cap.stop()
        audio = np.concatenate(self.frames) if self.frames else np.zeros(0, dtype=np.float32)
        report = check_voice(audio)
        os.makedirs(os.path.dirname(self.out), mode=0o700, exist_ok=True)
        pcm = (np.clip(audio, -1, 1) * 32767).astype("<i2").tobytes()
        with wave.open(self.out, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SR)
            w.writeframes(pcm)
        os.chmod(self.out, 0o600)
        emit("record.done", file=self.out, **report)

    def on_frame(self, frame, t):
        import numpy as np

        self.frames.append(frame)
        if len(self.frames) % 3 == 0:
            recent = np.concatenate(self.frames[-3:])
            emit("level", stream="record", peak=round(float(np.max(np.abs(recent))), 3),
                 rms=round(float(np.sqrt(np.mean(recent * recent))), 4), seconds=round(len(self.frames) * FRAME / SR, 1))


def check_voice(audio):
    """Length, clipping, background noise and how much of it is speech."""
    import numpy as np

    res = {"seconds": round(audio.size / SR, 1), "problems": []}
    if audio.size < SR:
        res["problems"].append("short")
        res["ok"] = False
        return res
    win = 480
    n = audio.size // win
    frames = audio[: n * win].reshape(n, win)
    rms = np.sqrt(np.mean(frames * frames, axis=1)) + 1e-9
    db = 20 * np.log10(rms)
    noise = float(np.percentile(db, 10))
    speech = float(np.percentile(db, 90))
    clip = float(np.mean(np.abs(audio) > 0.985))
    voiced = float(np.mean(db > noise + 12))
    res.update(noiseDb=round(noise, 1), speechDb=round(speech, 1), snrDb=round(speech - noise, 1),
               clipping=round(clip * 100, 3), speechRatio=round(voiced, 2))
    if res["seconds"] < 18:
        res["problems"].append("short")
    if clip > 0.001:
        res["problems"].append("clipping")
    if noise > -45:
        res["problems"].append("noisy")
    if speech < -32:
        res["problems"].append("quiet")
    if voiced < 0.45:
        res["problems"].append("pauses")
    res["ok"] = not res["problems"]
    return res


def prepare_profile(cmd):
    """Runs the voice worker once to compute and cache the Chatterbox conditionals."""
    if not _big_model.acquire(blocking=False):
        emit("error", message="A voz clonada está em uso: pare a tradução para preparar o perfil.")
        return
    try:
        require("clone")
        _prepare_profile(cmd)
    except ResourceError as e:
        emit("error", message=str(e))
    finally:
        _big_model.release()


def _prepare_omni_prompt(cmd):
    """OmniVoice: the voice clone prompt made right after the recording (or when the user
    picks OmniVoice in Settings), so the first translation starts without the wait."""
    t = now()
    model = cmd.get("whisperModel", "")

    def transcribe(path):
        r = subprocess.run(capped(["whisper-cli", "-m", model, "-f", path, "-nt", "-np", "-l", "auto", "-t", "4"], CAPS["whisper"]),
                           capture_output=True, text=True, timeout=300, preexec_fn=_die_with_parent)
        return " ".join(r.stdout.split()).strip()

    voice = OmniCloneVoice.__new__(OmniCloneVoice)
    voice.env = dict(os.environ, PYTHONUNBUFFERED="1", PYTHONDONTWRITEBYTECODE="1", HF_HUB_OFFLINE="1")
    voice.python, voice.worker = cmd["python"], cmd["worker"]
    voice.lock = threading.Lock()
    try:
        voice._make_prompt(cmd["wav"], cmd["out"], transcribe)
    except Exception as e:  # noqa: BLE001
        emit("error", message="Não foi possível preparar a voz para o OmniVoice: %s" % e)
        return
    emit("profile.ready", file=cmd["out"], engine="omnivoice", seconds=round(now() - t, 1))


def _prepare_profile(cmd):
    if cmd.get("engine") == "omnivoice":
        _prepare_omni_prompt(cmd)
        return
    t = now()
    env = dict(os.environ)
    env["PYTHONUNBUFFERED"] = "1"
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env.setdefault("HF_HUB_OFFLINE", "1")
    r = subprocess.run(capped([cmd["python"], cmd["worker"], "prepare", cmd["wav"], cmd["out"], cmd.get("model", "v3")], CAPS["clone"]),
                       capture_output=True, text=True, env=env, preexec_fn=_die_with_parent)
    last = {}
    for line in r.stdout.splitlines():
        try:
            last = json.loads(line)
        except ValueError:
            pass
    if r.returncode != 0 or last.get("event") != "done":
        emit("error", message="Não foi possível preparar o perfil de voz: %s" % (last.get("message") or r.stderr.strip().splitlines()[-1:]))
        return
    os.chmod(cmd["out"], 0o600)
    emit("profile.ready", file=cmd["out"], seconds=round(now() - t, 1), device=last.get("device", ""))


# ------------------------------------------------------------------ main loop


def main():
    session = None
    recorder = None
    restore_leftover_ducking()  # a previous engine that was killed while the voice spoke
    emit("ready", version=1, pid=os.getpid())
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            cmd = json.loads(line)
        except ValueError:
            emit("error", message="comando inválido")
            continue
        c = cmd.get("cmd")
        try:
            if c == "start":
                if session:
                    session.stop()
                session = Session(cmd)
                session.start()
            elif c == "lang" and session:
                session.set_lang(cmd.get("stream", "in"), cmd.get("from", "auto"))
            elif c == "pause" and session:
                session.pause(True)
                emit("state", stage="paused")
            elif c == "resume" and session:
                session.pause(False)
                emit("state", stage="running")
            elif c == "stop":
                if session:
                    session.stop()
                    session = None
                emit("state", stage="stopped")
            elif c == "record.start":
                recorder = Recorder(cmd.get("source"), cmd["out"], cmd.get("seconds", 30))
                recorder.start()
            elif c == "record.stop" and recorder:
                recorder.stopped = True
            elif c == "profile.prepare":
                threading.Thread(target=prepare_profile, args=(cmd,), daemon=True).start()
            elif c == "quit":
                break
        except Exception as e:  # noqa: BLE001
            import traceback

            traceback.print_exc(file=sys.stderr)
            emit("error", message="%s: %s" % (type(e).__name__, e))
            if c == "start" and session:
                session.stop()
                session = None
                emit("state", stage="stopped")
    if session:
        session.stop()
    emit("state", stage="quit")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# Copyright (C) 2026 LingmoOS Team.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# any later version.
"""The user's own cloned voice for live translation.

Runs in the voice virtualenv shared with Lingmo Studio's dubbing
(~/.local/share/lingmo/dub-venv: Chatterbox Multilingual TTS, MIT). It never
changes that environment; it only imports from it.

The voice is the Studio's, on purpose: the same model (v3 when installed),
the same settings (exaggeration 0.5, cfg_weight 0.0, temperature 0.8,
seed 1234, up to 3 takes on the GPU / 1 on the CPU) and the same take check
(natural 8-22 characters a second, trimmed silence). The quality logic below
is a copy of lingmo-studio/data/dub.py's `speak()`, kept separate so the
Studio's dubbing never changes because of live translation.

What makes it fast enough for live use:
  - the model is loaded once and stays warm (this process lives for the session);
  - the speaker conditionals (speaker embedding + prompt speech tokens + the
    S3Gen reference) are computed once when the profile is recorded and saved
    with torch.save (Conditionals.save); each sentence just reuses them;
  - the engine sends sentence by sentence, so the next one is generated while
    the current one plays.

  clone_worker.py prepare VOICE.wav OUT.pt [v3|v2]     (one shot, JSON on stdout)
  clone_worker.py                                      (serve JSON lines on stdin)
      {"cmd": "load", "model": "v3", "device": "auto", "conds": "/…/conds-v3.pt", "quality": "quality"|"fast"}
      {"cmd": "speak", "text": "…", "language": "pt"}  → {"event": "done", "file": …, "ms": …, "takes": …}
      {"cmd": "quit"}
"""

import json
import os
import sys
import tempfile
import time

SR = 24000

# The Studio's settings (lingmo-studio/src/app/dubber.cpp, startSynth)
STUDIO = {"exaggeration": 0.5, "cfg_weight": 0.0, "temperature": 0.8, "seed": 1234}


class NoWatermark:
    """Chatterbox stamps its output with Perth's inaudible watermark: Lingmo writes nothing
    into the audio, so the user can publish it wherever they like."""

    def apply_watermark(self, wav, sample_rate=None):
        return wav


def emit(event, **fields):
    fields["event"] = event
    sys.stdout.write(json.dumps(fields, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def log(message):
    emit("log", message=str(message))


def trim_silence(x, sr, floor_db=-42.0, margin_ms=45, fade_ms=8):
    """Copy of lingmo-studio/data/dub.py: cuts the silence around a sentence."""
    import numpy as np

    if x.size == 0:
        return x
    win = max(1, int(sr * 0.01))
    n = x.size // win
    if n == 0:
        return x
    frames = np.abs(x[: n * win]).reshape(n, win).max(axis=1)
    peak = float(frames.max())
    if peak <= 1e-6:
        return x
    loud = np.where(frames > peak * (10.0 ** (floor_db / 20.0)))[0]
    if loud.size == 0:
        return x
    margin = int(sr * margin_ms / 1000)
    start = max(0, int(loud[0]) * win - margin)
    end = min(x.size, (int(loud[-1]) + 1) * win + margin)
    y = x[start:end].copy()
    fade = min(int(sr * fade_ms / 1000), y.size // 2)
    if fade > 1:
        ramp = np.linspace(0.0, 1.0, fade, dtype=np.float32)
        y[:fade] *= ramp
        y[-fade:] *= ramp[::-1]
    return y


def clean_text(text):
    text = " ".join(text.split())
    if text and text[-1] not in ".!?…。！？":
        text += "."
    return text


def write_wav(path, samples, sr):
    import wave

    import numpy as np

    pcm = (np.clip(samples, -1.0, 1.0) * 32767.0).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(pcm.tobytes())


class Voice:
    def __init__(self):
        self.model = None
        self.device = "cpu"
        self.version = "v2"

    def pick_device(self, want):
        import torch

        if want in ("cuda", "cpu"):
            return want
        if torch.cuda.is_available():
            try:
                free, _total = torch.cuda.mem_get_info()
                if free >= 3.6e9:
                    return "cuda"
                log("only %.1f GB of video memory free: using the CPU (much slower)" % (free / 1e9))
            except Exception:  # noqa: BLE001
                return "cuda"
        return "cpu"

    def load(self, model="v3", device="auto"):
        import inspect

        from chatterbox.mtl_tts import ChatterboxMultilingualTTS

        self.device = self.pick_device(device)
        kwargs = {}
        if "t3_model" in inspect.signature(ChatterboxMultilingualTTS.from_pretrained).parameters:
            kwargs["t3_model"] = model
            self.version = model
        t = time.time()
        # a plain string device, as in the Studio (chatterbox compares it to pick map_location)
        self.model = ChatterboxMultilingualTTS.from_pretrained(device=self.device, **kwargs)
        self.model.watermarker = NoWatermark()
        return time.time() - t

    def use_conds(self, path):
        from chatterbox.mtl_tts import Conditionals

        self.model.conds = Conditionals.load(path, map_location=self.device).to(self.device)

    def generate(self, text, language, seed):
        import torch

        torch.manual_seed(seed)
        if self.device == "cuda":
            torch.cuda.manual_seed_all(seed)
        wav = self.model.generate(text, language_id=language, audio_prompt_path=None,
                                  exaggeration=STUDIO["exaggeration"], cfg_weight=STUDIO["cfg_weight"],
                                  temperature=STUDIO["temperature"])
        return wav.squeeze().detach().cpu().numpy().astype("float32")


def speak(voice, text, language, attempts, seed_base):
    """Copy of the Studio's take selection (lingmo-studio/data/dub.py `speak`), without the
    timeline slot: several seeds, the most natural one wins, early stop on a good take."""
    import numpy as np

    best, best_score, takes = None, 1e9, 0
    for attempt in range(attempts):
        seed = seed_base + attempt * 7919
        x = voice.generate(text, language, seed)
        takes += 1
        x = trim_silence(x, SR)
        seconds = x.size / SR
        if seconds <= 0.05:
            continue
        cps = max(len(text), 1) / seconds
        # natural speech is 8-22 characters a second; far outside means a cut-off or a ramble
        score = abs(np.log(cps / 14.0))
        if 6.0 <= cps <= 30.0 and score < best_score:
            best, best_score = x, score
            if score < 0.35:
                break
        elif best is None and score < best_score:
            best, best_score = x, score + 5
    return (best if best is not None else np.zeros(int(SR * 0.3), dtype="float32")), takes


def cmd_prepare(args):
    """voice.wav → conds-<model>.pt (the speaker conditionals, computed once)."""
    wav, out = args[0], args[1]
    model = args[2] if len(args) > 2 else "v3"
    voice = Voice()
    load_s = voice.load(model, "auto")
    t = time.time()
    voice.model.prepare_conditionals(wav, exaggeration=STUDIO["exaggeration"])
    tmp = out + ".part"
    voice.model.conds.save(tmp)
    os.replace(tmp, out)
    os.chmod(out, 0o600)
    emit("done", file=out, device=voice.device, loadS=round(load_s, 1), prepareS=round(time.time() - t, 2), model=voice.version)
    return 0


def serve():
    import torch

    voice = None
    attempts = 1
    tmpdir = tempfile.mkdtemp(prefix="lingmo-voice-", dir=os.environ.get("XDG_RUNTIME_DIR") or None)
    os.chmod(tmpdir, 0o700)
    counter = 0
    for line in sys.stdin:
        try:
            cmd = json.loads(line)
        except ValueError:
            continue
        c = cmd.get("cmd")
        try:
            if c == "load":
                voice = Voice()
                load_s = voice.load(cmd.get("model", "v3"), cmd.get("device", "auto"))
                voice.use_conds(cmd["conds"])
                # the Studio makes 3 takes on the GPU and 1 on the CPU; "fast" (visible option) makes one
                attempts = 1 if (voice.device != "cuda" or cmd.get("quality") == "fast") else 3
                # warm the CUDA kernels so the first real sentence is not the slow one
                t = time.time()
                voice.generate("Olá.", "pt", STUDIO["seed"])
                vram = torch.cuda.memory_allocated() / 1e6 if voice.device == "cuda" else 0
                emit("ready", device=voice.device, model=voice.version, loadS=round(load_s, 1),
                     warmS=round(time.time() - t, 2), attempts=attempts, vramMb=int(vram))
            elif c == "speak":
                t = time.time()
                text = clean_text(cmd["text"])
                counter += 1
                x, takes = speak(voice, text, cmd.get("language", "pt"), attempts, STUDIO["seed"] + counter * 101)
                path = os.path.join(tmpdir, "s%d.wav" % counter)
                write_wav(path, x, SR)
                emit("done", file=path, ms=int((time.time() - t) * 1000), takes=takes, seconds=round(x.size / SR, 2))
            elif c == "quit":
                break
        except Exception as e:  # noqa: BLE001
            if voice is not None and voice.device == "cuda" and "out of memory" in str(e).lower():
                emit("error", message="A memória de vídeo acabou (outro programa está usando a GPU)")
            else:
                emit("error", message="%s: %s" % (type(e).__name__, e))
    try:
        os.rmdir(tmpdir)
    except OSError:
        pass
    return 0


def cmd_warmup(args):
    """Downloads the model once (installer step): loads it on the CPU and exits."""
    voice = Voice()
    voice.load(args[0] if args else "v3", "cpu")
    emit("done", model=voice.version)
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "warmup":
        sys.exit(cmd_warmup(sys.argv[2:]))
    if len(sys.argv) > 1 and sys.argv[1] == "prepare":
        try:
            sys.exit(cmd_prepare(sys.argv[2:]))
        except Exception as e:  # noqa: BLE001
            emit("error", message="%s: %s" % (type(e).__name__, e))
            sys.exit(1)
    sys.exit(serve())

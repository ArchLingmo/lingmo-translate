# Tradução ao vivo (lingmo-translate)

Live speech translation for the Lingmo desktop, entirely on the user's computer:

| Mode | What it does | Target | Measured (RTX 2060 6 GB, Ryzen 5 1600) |
|---|---|---|---|
| **Legendas** (subtitles) | system audio, one app or the mic → translated subtitles in a glass strip on top of everything | 1–2 s | draft 0.6–0.9 s, final 1.6–2.0 s (quality) · 0.56–0.72 s (fast) |
| **Ouvir dublado** (listen dubbed) | the same + a translated voice in the headphones, the original lowered | 1–3 s | English: voice starts 2.1–3.0 s · Japanese anime (7 min, 122 lines): median 2.4 s, p90 4.9 s after each subtitle cue (was 3.9 / 7.2 s); short lines 0.8 s median |
| **Falar traduzido** (speak translated) | mic → translation → your cloned voice (or piper) → the **Lingmo Tradutor** virtual mic for Meet/Teams/Zoom | 3–5 s | piper: not measured yet · cloned: not measured yet |
| **Chamada** (call) | both directions at once | — | implemented, not measured yet |

Times are from the moment the speaker stops to the event, as measured by the
engine (`tests/e2e_engine.py` plays generated speech into a private audio
server and also checks them from outside).

## Architecture

```
 lingmo-translate (Qt 6 / QML)                         engine.py (own venv, one long-lived process)
 ─────────────────────────────                         ──────────────────────────────────────────────
 window · overlay (GlassSurface, click-through)        capture: parec (PulseAudio protocol: pipewire-pulse
 Island activity · virtual-mic indicator     JSON         or PulseAudio), 16 kHz mono, 32 ms frames
 settings (translate.conf, shared with  ◀── lines ──▶   → Silero VAD (ONNX) → segmenter (partials every
   Settings › IA) · voice profiles · setup               700 ms, final after 400 ms of silence or 7 s)
 VRAM guard (nvidia-smi + Ollama /api/ps)              → STT: whisper-server (whisper.cpp, model kept warm)
 virtual mic (pactl: null sink + remap source)         → MT: Opus-MT/CTranslate2 draft (~60 ms, CPU)
 intents (com.lingmo.Translate /Intents)                     + qwen3.5 via Ollama refinement (quality, ~1.1 s)
 D-Bus com.lingmo.Translate /Translate                 → TTS: piper (in-process) / espeak-ng /
                                                              clone_worker.py (Chatterbox, Studio's venv)
                                                       → pacat (one stream per session) · ducking (pactl)
```

Each stage is its own thread joined by queues, so listening, recognising,
translating and speaking overlap: sentence N+1 is recognised while sentence N
is translated and sentence N−1 plays. Partials only run when no final is
waiting; the LLM refinement runs apart from the instant draft so a slow
refinement never delays the next subtitle.

### Resource plan: the hardware recommendation

Each mode's defaults come from the hardware profile (`src/hwprofile.{h,cpp}`,
shared with Voice Studio and Settings; `~/.config/lingmoos/hardware.conf`,
documented in `docs/voices.md` section 5). What the user sets on the Translate
page is kept per mode (`[Translate] <mode>/sttModel`, `<mode>/quality`) and
"(recommended)" always marks the recommendation.

| GPU | subtitles / dubbed | speak / call |
|---|---|---|
| NVIDIA 6 GB | whisper large-v3-turbo-q5_0 (0.9 GB) + Opus-MT + qwen3.5:4b (3.7 GB), piper (~4.6 GB) | whisper large-v3-turbo (1.9 GB) + Opus-MT only + cloned voice (OmniVoice 2.6 GB, ~4.5 GB) |
| NVIDIA 8 GB and more | large-v3-turbo + Opus-MT + qwen, piper | large-v3-turbo + Opus-MT + qwen beside the cloned voice when the free VRAM allows |
| NVIDIA under 5 GB, or no CUDA | small-q5_1 (base on a weak CPU), Opus-MT only, piper | the same; a cloned voice is allowed with a warning |

Dubbed listening speaks with piper only, so the LLM fits beside it. At run
time the free VRAM still decides: with a cloned voice and too little room the
compressed turbo replaces the f16 one for that session (Chatterbox beside the
f16 model on 6 GB), and qwen loads beside the voice only when both fit. A
missing model is downloaded on start while the closest one on disk is used.

Whisper on the CPU of this machine takes 2–3 s per sentence (base) to 7–12 s
(small), so recognition stays on the GPU; the CPU model is only a fallback when
the GPU has no room (the user is told).

**Guards** (`engine.py`, "resource guard"): before each model loads the engine
checks the free RAM (keeps 1.5 GB for the desktop) and VRAM and refuses with a
clear message; only one large model loads at a time (a profile is never
prepared while the cloned voice is loaded); whisper-server, piper and the voice
worker run in memory-capped systemd user scopes (`MemoryMax` 3 G / 1 G / 9 G,
no swap) and die with the engine. The app asks before unloading Ollama models
("Liberar a GPU": `keep_alive: 0` for each model in `/api/ps`); a quality model
the engine loaded itself is unloaded when translation stops, one somebody else
loaded is left alone. A different `num_ctx` would make Ollama reload a model,
so none is sent.

### Audio routing

Everything goes through the PulseAudio protocol (`pactl`, `parec`, `pacat`),
served by pipewire-pulse on ArchLingmo and by PulseAudio elsewhere.

- **System**: `@DEFAULT_MONITOR@`. When the dubbed voice plays on the same
  speakers it would be heard again, so capture is muted while it speaks
  (half duplex); pick "one app" for full duplex.
- **One app**: `parec --monitor-stream=<sink-input>` of the app's stream,
  found again by name when the app opens a new stream. The monitored stream is
  captured after its volume, so while it is ducked the frames are scaled back
  up for the recogniser.
- **Ducking**: the source app's streams (or all others) go to `duck` (30 %)
  while the translated voice speaks and come back 0.6 s after it ends.
- **Virtual mic**: `module-null-sink lingmo_tradutor_out` + `module-remap-source
  lingmo_tradutor` ("Lingmo Tradutor"). Created when speaking starts, removed
  when it stops; leftovers of a crash are removed on the next start and by
  `lingmo-translate --cleanup`. While it exists the overlay shows a red
  "Microfone tradutor ligado" pill and the Island shows a call-style activity.

### Voice profiles

`~/.local/share/lingmo/voices/<name>/` (0700): `voice.wav` (16 kHz mono, a
guided 20–30 s reading with a level meter and a check for length, clipping,
noise floor, loudness and pauses), `profile.json`, and `conds-v3.pt`: the
Chatterbox conditionals (`prepare_conditionals` → `Conditionals.save`, speaker
embedding + prompt speech tokens + S3Gen reference) computed once, so each
sentence skips that step (`Conditionals.load` in the warm worker). Only a
recording made in the app, or an imported one with the explicit "this is my
own voice" confirmation, becomes a profile. Nothing is written into the audio (Chatterbox's watermark is turned off).

The cloned voice uses **the same model and settings as Lingmo Studio's
dubbing** (Chatterbox Multilingual v3, exaggeration 0.5, cfg_weight 0,
temperature 0.8, up to 3 takes with the natural-speed check on the GPU); its
environment (`~/.local/share/lingmo/dub-venv`) is shared with the Studio and
never modified by this app. "Rápida" (one take) is an explicit option.

**OmniVoice** (opt-in, chosen system-wide in Settings › IA › Voz clonada,
`~/.config/lingmoos/voice.conf` `[Voice] cloneEngine`, see `docs/voices.md`):
used only when Voice Studio reports the licence accepted and the engine
installed; otherwise the app falls back to Chatterbox with a one-line notice,
and re-reads the file when it changes. Everything is Voice Studio's: the venv
(`~/.local/share/lingmo/omnivoice-venv`), the worker it installs
(`/usr/share/lingmo-voicestudio/engine/workers/omnivoice_worker.py`, run in
that venv), the weights in the Hugging Face cache; nothing is copied. The
reference is cut to 8–12 s of `voice.wav` at a pause, transcribed by our own
whisper, and encoded once **on the CPU** (on the GPU that step needs ~2.5 GB
more and runs out of memory on a 6 GB card beside whisper) into
`omnivoice-prompt.pt` next to the recording (the same cache Voice Studio
uses; re-recording deletes it). "Melhor qualidade" is 32 flow steps, "Rápida"
16. Nothing is written into the audio (no watermark, no tag).

Measured (RTX 2060 6 GB, speak pt→en, the 4-sentence `talk_cadu` test with a
piper "cadu" voice as the cloned speaker, Opus-MT, one run each):

| | voice starts after the speech ends (each sentence) | median | whisper + voice VRAM | similarity (Resemblyzer, vs held-out clips) |
|---|---|---|---|---|
| Chatterbox v3, best quality | 6.46 / 4.25 / 6.17 / 6.22 s | 6.2 s | 4.1 GB | 0.918 |
| OmniVoice, 32 steps | 2.88 / 3.20 / 3.22 / 3.06 s | 3.1 s | 3.0 GB | 0.932 |

The first start with OmniVoice also makes the prompt (~40 s, once per recording).

## Models and licences

Nothing is in the package or the ISO; everything downloads on first use
(Models page, ~650 MB without the cloned voice).

| Part | Model | Size | Licence |
|---|---|---|---|
| Voice activity | Silero VAD v5.1.2 (ONNX) | 2 MB | MIT |
| Speech recognition | whisper.cpp `ggml-small-q5_1` (shared with the Assistant), plus what the hardware recommendation uses: `large-v3-turbo-q5_0` (547 MB) and `large-v3-turbo` (1.6 GB) on a 6 GB GPU | 181 MB + | MIT |
| Instant translation | Opus-MT en→ROMANCE (`>>pt_br<<`) and ROMANCE→en, Helsinki-NLP, converted here to CTranslate2 int8 | 82 MB each | CC-BY 4.0 (attribution) |
| Instant translation from Japanese, Chinese, Korean (optional, Models page) | Opus-MT ja→en (OPUS-MT 2019-12-18), zho→eng (Tatoeba-MT 2020-07-17), kor→eng (Tatoeba-MT 2021-02-18), Helsinki-NLP, CTranslate2 int8; the engine chains them with en→ROMANCE | 80 MB each | CC-BY 4.0 (attribution) |
| Quality translation | qwen3.5:4b in the user's Ollama (optional) | 3.4 GB | Apache-2.0 |
| Voices | piper-tts 1.8 (in the engine venv) + voices pt_BR faber, en_US joe | 63 MB each | GPL-3.0 engine · CC0 voices |
| Fallback voice | espeak-ng (system) | — | GPL-3.0 |
| Cloned voice | Chatterbox Multilingual (Resemble AI), Studio's environment | ~3 GB | MIT |
| Engine libraries | numpy, onnxruntime, ctranslate2, sentencepiece | ~170 MB | BSD / MIT / MIT / Apache-2.0 |

Rejected: NLLB and SeamlessStreaming (CC-BY-NC); Opus-MT tc-big en→pt
(CC-BY 4.0, but it answers in European Portuguese and is 2.5× slower).
OmniVoice (Apache-2.0 code, CC-BY-NC weights) is never shipped or downloaded
by this app: it is used only after the user turned it on in Voice Studio
(licence accepted there, personal non-commercial use) and chose it in Settings.

### Translation quality

30 meeting sentences in both directions against reference translations
(chrF, higher is better):

| | en→pt | pt→en | time per sentence |
|---|---|---|---|
| Opus-MT (draft, "Rápida") | 65.4 | 75.8 | 40–110 ms (CPU) |
| Opus-MT tc-big en→pt | 69.3 (European Portuguese) | — | 100–240 ms |
| qwen3.5:4b, plain prompt | 70.2 | 77.9 | ~1.1 s |
| **qwen3.5:4b, faithful prompt (default)** | **75.0** | **80.3** | ~1.1 s (36 s cold) |

Opus-MT makes lexical blunders a commercial product would not ("O inseto" for
*the bug*, "mude seu microfone" for *mute your microphone*), so the default is
the instant Opus-MT draft replaced ~1 s later by qwen.

### Japanese → Portuguese (anime)

Corpus chrF against reference translations; time per line on the CPU (Opus) or
the RTX 2060 (qwen):

| | Tatoeba ja→pt (100 short lines) | FLEURS ja→pt (100 long lines) | time |
|---|---|---|---|
| Opus-MT jpn→por, direct (Tatoeba-MT) | 46.8 | 40.5 | 60–150 ms |
| **Opus-MT ja→en → en→pt (pivot, shipped)** | **54.3** | **44.4** | 190–480 ms |
| qwen3.5:4b, call prompt | 51.7 | 46.8 | 0.9–1.5 s |
| qwen3.5:4b, video-dialogue prompt (used for videos) | 53.2 | 47.4 | 1.1–1.4 s |

Tatoeba lines may overlap Opus-MT's training data (it favours Opus); on short
lines (≤ 10 characters) the pivot is as good as qwen, on long lines qwen is
clearly better, and on real anime audio (noisy recognition) the pivot loses the
meaning far more often. So, dubbing a video, a short line is spoken at once
with the pivot and a long one waits (up to 1.6 s) for qwen; the subtitles always
get qwen's version. zh→pt and ko→pt through the pivot: chrF 59.4 and 64.2 on
100 Tatoeba lines each.

## Use

- `lingmo-translate` · `--toggle` (Meta+Shift+T) · `--start [subtitles|dubbed]`
  · `--stop` · `--page translate|voices|models|record` · `--cleanup`
- Intents: `translate.live.start` (subtitles/dubbed only; speaking for the user
  always starts from the window) and `translate.live.stop`.
- Settings › IA › Tradução ao vivo edits the same `~/.config/lingmo/translate.conf`.
- Each mode keeps its own languages (`[Translate] subtitles/from`, `subtitles/to`,
  `dubbed/…`, `speak/…`, `call/…`): listening defaults to auto → pt, speaking
  to pt → en; in a call `from` is the language they speak and `to` the one I
  speak. The old global `from`/`to` become every mode's pair once (speak keeps
  pt → en when the old pair was a listening one). The Island's language
  picker and Meta+Shift+T (the last mode) use the current mode's pair.

## Dubbed latency (anime, ja→pt)

Japanese track of a 7-minute section of an anime episode (122 PT-BR subtitle
cues as the line boundaries, half of the gaps under 0.3 s), played as an app
into a private audio server; "voice start" is after each cue ends:

| | voice start median / p90 | lines lost | voice still on an older line at a new cue | voice chrF vs official subtitles |
|---|---|---|---|---|
| before (LLM before every line, 400 ms pause) | 3.85 / 7.19 s | 0 (+14 unrecognised) | 64 / 121 | 30.7 |
| instant pivot always spoken | 1.98 / 5.74 s | 2 | 42 / 121 | 24.7 (meaning lost) |
| **shipped** (split at pauses, short → pivot, long → LLM ≤1.6 s, pace budget) | **2.44 / 4.90 s** | 2 (+10 unrecognised, 1 filler) | 58 / 121 | **32.7** |

Engine-side (after the end of speech): end of line 0.20 s, recognised 0.37 s,
instant translation 0.59 s, LLM 1.74 s, voice 2.26 s median (p90 3.88 s). On a
rapid exchange of short lines (27 lines, 0.2–0.6 s gaps) the voice starts
0.80 s median (p90 1.04 s) after each line, none lost. Subtitle chrF varies ±4
between runs (recognition is not deterministic), so compare options, not
absolutes.

## Latency log (opt-in, local only)

Start the app with `LINGMO_TRANSLATE_STATS=/path/stats.jsonl` and the engine
appends one JSON line per utterance: stream, language, the stage timings in ms
after the end of speech (`vadFinal`, `sttDone`, `mtDraft`, `mtFinal`,
`ttsFirst`, `playStart` = the dubbed voice starts), whether a sentence was
skipped and how late, the pace and which translator the voice used. Audio is
never written; the text only with `LINGMO_TRANSLATE_STATS_TEXT=1`. The file is
created 0600.

## Tests

`tests/e2e_engine.py CONFIG.json speech.wav…` runs the engine against an
isolated audio server (never the user's), plays generated speech (espeak-ng,
piper) and reports per-stage latency, the translations and the ducking.

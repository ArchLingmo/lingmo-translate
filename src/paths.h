/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

#pragma once

#include <QString>

// Where everything lives. Nothing big ships in the package: the engine's
// virtualenv and the models are downloaded on first use.
//
//   ~/.local/share/lingmo/translate/venv              engine virtualenv (numpy, onnxruntime, ctranslate2, sentencepiece)
//   ~/.local/share/lingmo/translate/models/mt-*        Opus-MT converted to CTranslate2 (int8)
//   ~/.local/share/lingmo/translate/models/silero_vad.onnx
//   ~/.local/share/lingmo/translate/piper/             extra piper voices (English)
//   ~/.local/share/lingmo/whisper-models/ggml-*.bin    whisper.cpp models, shared with the Assistant
//   ~/.local/share/lingmo/dub-venv                     voice engine (Chatterbox), shared with Lingmo Studio
//   ~/.local/share/lingmo/voices/<name>/               voice profiles (0700): voice.wav, profile.json, conds-v3.pt
//   ~/.config/lingmo/translate.conf                    [Translate] settings, shared with Settings › IA
//   ~/.config/lingmoos/voice.conf                      [Voice] cloneEngine, system-wide (Settings › IA, docs/voices.md)
//   ~/.local/share/lingmo/omnivoice-venv               OmniVoice (opt-in, CC-BY-NC weights), Voice Studio's
//
// LINGMO_TRANSLATE_DATA overrides ~/.local/share/lingmo (tests).
namespace Paths
{
QString dataRoot();          // ~/.local/share/lingmo
QString translateDir();      // …/translate
QString venvDir();
QString venvPython();
QString modelsDir();
QString whisperDir();
QString voicesDir();
QString voiceVenvPython();   // …/dub-venv/bin/python
QString configFile();
QString engineScript();      // engine.py (installed, or next to the build)
QString cloneWorker();       // clone_worker.py
QString whisperModel(const QString &name); // "small-q5_1" → …/ggml-small-q5_1.bin
QString voiceConf();         // ~/.config/lingmoos/voice.conf
// OmniVoice belongs to Voice Studio: its venv, its worker and its consent are read, never copied
QString omniVoicePython();   // …/omnivoice-venv/bin/python (LINGMO_OMNIVOICE_VENV)
QString omniVoiceWorker();   // Voice Studio's workers/omnivoice_worker.py (LINGMO_OMNIVOICE_WORKER)
QString voiceStudioConfig(); // ~/.config/lingmo/voicestudio.json (the OmniVoice licence acceptance)
QString omniVoiceWeights();  // the Hugging Face cache folder of k2-fsa/OmniVoice
}

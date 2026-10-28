/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

// The recommended configuration of Lingmo's voice and translation apps for
// this computer, and the shared hardware profile that carries it:
//
//   ~/.config/lingmoos/hardware.conf     (QSettings INI, docs/voices.md "Perfil de hardware")
//
// The policy lives in ONE function, recommend(). The repositories build on
// their own, so this file and hwprofile.cpp exist in each, byte for byte the
// same (as whisperhw.{h,cpp}, which they build on):
//   lingmo-voicestudio/src/common/hwprofile.{h,cpp}   (tests: tests/tst_hwprofile.cpp)
//   lingmo-translate/src/hwprofile.{h,cpp}
//   lingmo-settings/src/hwprofile.{h,cpp}
// Change them together.
//
// Every app reads its defaults from current(): the file when it matches this
// computer, else a fresh recommendation (written back). A value the user set
// in an app's own config wins over the recommendation; a value never set
// follows it.
//
// Only Qt Core: the tests link nothing else.

#pragma once

#include "whisperhw.h"

#include <QDateTime>
#include <QMap>
#include <QString>
#include <QStringList>

namespace HwProfile {

// Bump when recommend() changes: every app recomputes hardware.conf on start
constexpr int PolicyVersion = 1;

// Video memory each part takes on the GPU (MiB), measured on an RTX 2060 6 GB
// (whisper.cpp 1.9.5 with flash attention, Ollama, the voice workers)
constexpr int LlmVramMb = 3700;        // qwen3.5:4b Q4_K_M with its default context
constexpr int OmniVoiceVramMb = 2600;  // OmniVoice fp16 while speaking (2.3 GB peak measured)
constexpr int ChatterboxVramMb = 3400; // Chatterbox Multilingual v3 while speaking
constexpr int DesktopVramMb = 400;     // the desktop, a browser and OBS (0.3–0.5 GB)
// whisper.cpp model on the GPU: small-q5_1 526, large-v3-turbo-q5_0 902,
// large-v3-turbo 1912 (measured); the others from WhisperHw's catalogue
int sttVramMb(const QString &model);

// Live translation, per mode ("subtitles", "dubbed", "speak", "call")
struct ModePlan {
    QString sttModel;             // whisper.cpp model, e.g. "large-v3-turbo-q5_0"
    QString quality;              // "quality" (Opus-MT draft + the LLM) | "fast" (Opus-MT only)
    QString voice;                // "none" (subtitles) | "piper" | "clone"
    bool cloneAllowed = false;    // a cloned voice is recommended in this mode
    bool llmBesideClone = false;  // the LLM may load beside the cloned voice
    int vramMb = 0;               // estimate on the GPU while running (0 = all on the processor)
};

struct Profile {
    // ---- detected
    QString gpuVendor;   // "nvidia", "amd", "intel" or ""
    QString gpuName;
    int vramMb = 0;      // total video memory, 0 = none or unknown
    bool cuda = false;   // an NVIDIA GPU and whisper.cpp's CUDA backend (ggml-cuda)
    QStringList backends;
    int ramMb = 0;
    QString cpuName;
    int cores = 0;
    int threads = 0;
    bool avx2 = false;
    QDateTime detected;
    int version = 0;

    // ---- recommended
    QString tier;                     // "cpu" | "small" | "gpu6" | "gpu8"
    QMap<QString, ModePlan> translate;
    QString dictationModel;           // WhisperHw::recommend()
    bool dictationGpu = false;
    QString cloneEngine;              // "omnivoice" | "chatterbox" (voice.conf's choice)
    bool cloneOnGpu = false;          // that engine fits on the GPU beside whisper
    QString designEngine;             // Voice Studio's voice design: "omnivoice" | "parler"
    QString readVoice;                // Voice Studio's long reading: "clone" | "piper"

    bool isValid() const { return !tier.isEmpty() && translate.size() == 4; }
    ModePlan mode(const QString &mode) const { return translate.value(mode); }
    // The whisper.cpp models the recommendation uses, each once, smallest first
    QStringList whisperModels() const;
};

// "subtitles", "dubbed", "speak", "call"
const QStringList &modes();

// The policy: what suits this hardware (pure, tested with fake hardware)
Profile recommend(const WhisperHw::Hardware &hw);

// $XDG_CONFIG_HOME/lingmoos/hardware.conf; LINGMO_HARDWARE_CONF replaces it (tests)
QString path();
// An invalid profile when the file is missing or unreadable
Profile read(const QString &file = path());
bool write(const Profile &profile, const QString &file = path());
// Same GPU (vendor, name, video memory within 64 MiB), CUDA backend and processor
bool sameHardware(const Profile &a, const Profile &b);

// Probes this computer (WhisperHw::probe, LINGMO_WHISPER_HW for fake hardware)
// and recommends, reading and writing nothing
Profile detect();
// The profile every app uses: hardware.conf when it matches this computer and
// this policy version, else detect() written to hardware.conf
Profile current(bool *recomputed = nullptr);

} // namespace HwProfile

/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

// Which whisper.cpp model suits this computer, and how fast it will run.
//
// Shared by Lingmo Settings (AI › Transcrição e legendas) and Lingmo Studio
// (captions, Gerar cortes). The two repositories build on their own, so this
// file and whisperhw.cpp exist in both, byte for byte the same:
//   lingmo-studio/src/core/whisperhw.{h,cpp}
//   lingmo-settings/src/whisperhw.{h,cpp}
//   lingmo-voicestudio/src/common/whisperhw.{h,cpp}   (dictation's model choice)
//   lingmo-translate/src/whisperhw.{h,cpp}            (under hwprofile.{h,cpp})
// Change them all together; each repository has its own tests (tst_whisperhw).
// The recommendation for the voice and translation apps as a whole is built on
// this in hwprofile.{h,cpp} (~/.config/lingmoos/hardware.conf).
//
// whisper.cpp (Arch package whisper-cpp) loads ggml's compute backends at run
// time from /usr/lib/ggml: the ggml package ships only CPU variants
// (libggml-cpu-*.so); a GPU needs one of the packages ggml-cuda (NVIDIA),
// ggml-hip (AMD, ROCm), ggml-vulkan (any GPU) or ggml-sycl (Intel).
// A GPU without its backend runs nothing: the recommendation is then for the
// CPU, with a hint to install the backend.
//
// Only Qt Core: the tests link nothing else.

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace WhisperHw {

// ------------------------------------------------------------------ models

// A multilingual whisper.cpp ggml model on https://huggingface.co/ggerganov/whisper.cpp
// (file ggml-<name>.bin). The *.en models can't transcribe Portuguese.
struct Model {
    QString name;
    qint64 bytes;   // exact file size
    QString sha256; // of the whole file
    int memoryMb;   // approximate RAM (or VRAM) while transcribing
    int speed;      // 1 (very slow) .. 5 (very fast), relative to each other
    int quality;    // Portuguese: 1 basic, 2 good, 3 very good, 4 excellent
    bool quantized;
};

// Small to large
const QList<Model> &catalogue();
const Model *findModel(const QString &name);

// "small-q5_1" -> "small", "large-v3-turbo-q8_0" -> "large-v3-turbo"
QString family(const QString &name);
// Compute cost of the family: tiny 0, base 1, small 2, medium 3,
// large-v3-turbo 4, large-v3 (and older large) 5; -1 when unknown
int familyRank(const QString &name);

// ------------------------------------------------------------------ hardware

struct Hardware {
    double ramGb = 0;
    int cores = 0;   // physical cores
    int threads = 0; // logical processors
    bool avx2 = false;
    bool avx512 = false;
    QString cpuName;

    QString gpuVendor; // "nvidia", "amd", "intel" or ""
    QString gpuName;   // "NVIDIA GeForce RTX 2060"
    double vramGb = 0; // 0 = unknown (or shared memory)

    QStringList backends; // ggml GPU backends installed: "cuda", "hip", "vulkan", "sycl"

    // The installed backend whisper.cpp would use for this GPU ("" = none)
    QString gpuBackend() const;
    // The GPU runs whisper: backend installed and enough video memory
    bool gpuUsable() const;
};

// Reads /proc, /sys, nvidia-smi and /usr/lib/ggml. `root` replaces "/" (tests).
// LINGMO_WHISPER_HW (see fromSpec) replaces the whole probe, for tests and
// screenshots on another machine.
Hardware probe(const QString &root = QString());

// "ram=32,cores=6,threads=12,avx2=1,avx512=0,gpu=nvidia,gpuname=NVIDIA GeForce RTX 2060,vram=6,backends=cuda+vulkan"
Hardware fromSpec(const QString &spec);
QString toSpec(const Hardware &hw);

// Pure parsers, used by probe() and tested on their own
void parseCpuinfo(const QString &text, Hardware &hw);    // cores, threads, AVX2/AVX-512, name
double parseMeminfoGb(const QString &text);              // MemTotal
// nvidia-smi --query-gpu=name,memory.total --format=csv,noheader,nounits: the biggest GPU
bool parseNvidiaSmi(const QString &text, QString *name, double *vramGb);
// File names in ggml's backend folders -> backend ids
QStringList backendsFromFiles(const QStringList &fileNames);

// The ggml package that gives this GPU vendor a backend: ggml-cuda for NVIDIA,
// ggml-vulkan for the rest (works on every AMD and Intel GPU with Mesa, without
// the several GB of ROCm/oneAPI that ggml-hip and ggml-sycl pull in)
QString backendPackage(const QString &vendor);
// "NVIDIA" / "AMD" / "Intel"
QString vendorDisplayName(const QString &vendor);

// ------------------------------------------------------------------ recommendation

struct Recommendation {
    QString model;    // to use now
    bool gpu = false; // runs on the GPU
    QString device;   // "NVIDIA GeForce RTX 2060" or "CPU"
    QString reason;   // why, translated

    // A GPU is present but ggml has no backend for it
    QString upgradePackage; // "ggml-cuda"
    QString upgradeModel;   // what to use once it is installed
    QString upgradeHint;    // "Install ggml-cuda to use your NVIDIA (up to ~10× faster)"
    double upgradeSpeedup = 0;
};

Recommendation recommend(const Hardware &hw);

// The model is too heavy for how this computer runs it: a bigger family than
// the recommendation for the current backend
bool tooHeavy(const QString &model, const Hardware &hw);

// Estimated speed in seconds of audio per second of work (x real time) with
// the settings of runArguments(); 0 when unknown
double estimatedSpeed(const QString &model, const Hardware &hw, bool gpu);
double estimatedSpeed(const QString &model, const Hardware &hw); // on the device it would use
// "~4× real time" (translated)
QString speedText(double speed);
// "large-v3-turbo · CPU · ~0.6× real time" (translated)
QString runSummary(const QString &model, const Hardware &hw);

// ------------------------------------------------------------------ running whisper-cli

// whisper-cli 1.9.5 options for this computer (after -m/-f/-l/output options),
// from measurements on a Ryzen 5 1600 + RTX 2060 (see whisperhw.cpp):
//  - greedy decoding (-bs 1 -bo 1): 1.6-1.9× faster than the default beam
//    search of 5 on the CPU and 1.75× on the GPU (where beam search keeps one
//    CPU core at 100 % while the GPU waits); the words barely change;
//  - flash attention (-fa) on both: without it the GPU is 1.5× slower, the CPU 1.2×;
//  - threads: every logical processor on the CPU (12 beat 6 by ~10 %), 4 with a GPU;
//  - -ng on the CPU, so a GPU too small for the model isn't tried;
//  - -mc 0 (no previous text as context) and -sns (suppress non-speech
//    tokens): over music or game sound whisper otherwise falls into a loop,
//    one sentence dozens of times; costs ~2 % of speed, 0.7 pt of WER.
//    Captions::collapseRepeats removes what still slips through.
// Not used: --vad (whisper-cli 1.9.5 maps the segment times of -ojf back to
// the file but leaves the token times in the speech-only audio's time, so
// word-synced captions would drift), -p N (slower
// on the CPU; on the GPU +17 % but words near the chunk edges get lost).
//  - with a known model, word times by DTW (-dtw <preset>, which needs -nfa):
//    on Portuguese speech with known word onsets (large-v3-turbo), median
//    error 53 ms and 89 % within 100 ms after Captions' 140 ms lead, against
//    91 ms and 53 % for the token offsets; costs ~30 % of speed on the GPU.
QStringList runArguments(const Hardware &hw, bool gpu, const QString &model = QString());
// whisper.cpp's alignment-heads preset for a model: "large.v3.turbo", "small"; "" = none
QString dtwPreset(const QString &model);
int threadCount(const Hardware &hw, bool gpu);

// ------------------------------------------------------------------ installing a GPU backend

struct Command {
    QString program;
    QStringList arguments;
};
// pkexec /usr/lib/lingmo-store/lingmo-store-helper install <pkg> when Lingmo
// Store is installed (polkit action com.lingmo.store.manage-packages), else
// pkexec pacman -S --needed --noconfirm <pkg>. `root` replaces "/" (tests).
Command installCommand(const QString &package, const QString &root = QString());

} // namespace WhisperHw

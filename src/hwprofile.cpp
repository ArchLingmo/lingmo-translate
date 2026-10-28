/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

// Keep identical in every repository that has it (see hwprofile.h).

#include "hwprofile.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace HwProfile {

namespace {

// Tiers by total video memory (MiB). An "8 GB" card reports 7.6–8 GiB, a "6 GB" one 5.8–6.
constexpr int SmallGpuBelowMb = 5000;  // 2–4 GB cards
constexpr int BigGpuFromMb = 7500;     // 8 GB and more

const QStringList Qualities = {QStringLiteral("quality"), QStringLiteral("fast")};
const QStringList Voices = {QStringLiteral("none"), QStringLiteral("piper"), QStringLiteral("clone")};
const QStringList Tiers = {QStringLiteral("cpu"), QStringLiteral("small"), QStringLiteral("gpu6"), QStringLiteral("gpu8")};

int cloneVramMb(const QString &engine)
{
    return engine == QLatin1String("omnivoice") ? OmniVoiceVramMb : ChatterboxVramMb;
}

ModePlan plan(const QString &mode, const QString &stt, const QString &quality, bool clone, bool llmBesideClone)
{
    ModePlan p;
    p.sttModel = stt;
    p.quality = quality;
    const bool mic = mode == QLatin1String("speak") || mode == QLatin1String("call");
    // dubbed listening speaks with piper only, so the LLM fits beside it
    p.voice = mode == QLatin1String("subtitles") ? QStringLiteral("none") : mic && clone ? QStringLiteral("clone") : QStringLiteral("piper");
    p.cloneAllowed = mic && clone;
    p.llmBesideClone = p.cloneAllowed && llmBesideClone;
    return p;
}

} // namespace

int sttVramMb(const QString &model)
{
    if (model == QLatin1String("small-q5_1"))
        return 526;
    if (model == QLatin1String("large-v3-turbo-q5_0"))
        return 902;
    if (model == QLatin1String("large-v3-turbo"))
        return 1912;
    const WhisperHw::Model *m = WhisperHw::findModel(model);
    return m ? m->memoryMb : 600;
}

const QStringList &modes()
{
    static const QStringList list = {QStringLiteral("subtitles"), QStringLiteral("dubbed"), QStringLiteral("speak"), QStringLiteral("call")};
    return list;
}

QStringList Profile::whisperModels() const
{
    QStringList out;
    for (const QString &m : modes()) {
        const QString model = translate.value(m).sttModel;
        if (!model.isEmpty() && !out.contains(model))
            out << model;
    }
    std::sort(out.begin(), out.end(), [](const QString &a, const QString &b) { return sttVramMb(a) < sttVramMb(b); });
    return out;
}

// ------------------------------------------------------------------ the policy

// Measured on the reference machine (RTX 2060 6 GB, Ryzen 5 1600), 2026-10:
//   whisper.cpp on the GPU, 6 pt-BR sentences: small-q5_1 0.11 s/sentence WER 7.8 % 526 MB;
//   large-v3-turbo-q5_0 0.24 s 3.9 % 902 MB; large-v3-turbo (f16) 0.23 s 1.3 % 1912 MB.
//   OmniVoice ~2.6 GB, Chatterbox ~3.4 GB, qwen3.5:4b ~3.7 GB, the desktop 0.3–0.5 GB.
//   Translation (chrF): Opus-MT en→pt 65 / qwen 75, pt→en 76 / 80; Japanese through
//   the Opus pivot far below qwen.
//
//   no CUDA      small-q5_1 (base on a weak processor), Opus-MT only, piper; a cloned
//                voice works but is slow on the processor: not recommended
//   GPU < 5 GB   the same, whisper on the GPU
//   GPU 6 GB     subtitles, dubbed: large-v3-turbo-q5_0 + Opus-MT draft + qwen, piper (~4.6 GB)
//                speak, call: large-v3-turbo + Opus-MT only + the cloned voice (~4.5 GB)
//   GPU ≥ 8 GB   large-v3-turbo everywhere, qwen in every mode (beside the cloned voice too)
Profile recommend(const WhisperHw::Hardware &hw)
{
    Profile p;
    p.gpuVendor = hw.gpuVendor;
    p.gpuName = hw.gpuName;
    p.vramMb = int(std::lround(hw.vramGb * 1024));
    p.backends = hw.backends;
    p.cuda = hw.gpuVendor == QLatin1String("nvidia") && hw.backends.contains(QLatin1String("cuda"));
    p.ramMb = int(std::lround(hw.ramGb * 1024));
    p.cpuName = hw.cpuName;
    p.cores = hw.cores;
    p.threads = hw.threads;
    p.avx2 = hw.avx2;
    p.detected = QDateTime::currentDateTime();
    p.version = PolicyVersion;

    const bool gpu = p.cuda && p.vramMb > 0;
    if (!gpu)
        p.tier = QStringLiteral("cpu");
    else if (p.vramMb < SmallGpuBelowMb)
        p.tier = QStringLiteral("small");
    else if (p.vramMb < BigGpuFromMb)
        p.tier = QStringLiteral("gpu6");
    else
        p.tier = QStringLiteral("gpu8");

    // the cloned voice: OmniVoice is smaller and faster (when Voice Studio has it on)
    const int smallStt = sttVramMb(QStringLiteral("small-q5_1"));
    if (gpu && p.vramMb >= smallStt + OmniVoiceVramMb + DesktopVramMb) {
        p.cloneEngine = QStringLiteral("omnivoice");
        p.cloneOnGpu = true;
    } else {
        p.cloneEngine = QStringLiteral("chatterbox");
        p.cloneOnGpu = gpu && p.vramMb >= smallStt + ChatterboxVramMb + DesktopVramMb;
    }
    const int voiceMb = cloneVramMb(p.cloneEngine);

    const QString turbo = QStringLiteral("large-v3-turbo");
    const QString turboQ5 = QStringLiteral("large-v3-turbo-q5_0");
    const QString quality = QStringLiteral("quality");
    const QString fast = QStringLiteral("fast");
    for (const QString &mode : modes()) {
        const bool mic = mode == QLatin1String("speak") || mode == QLatin1String("call");
        ModePlan m;
        if (p.tier == QLatin1String("gpu8")) {
            m = plan(mode, turbo, quality, true, true);
        } else if (p.tier == QLatin1String("gpu6")) {
            m = mic ? plan(mode, turbo, fast, true, false) : plan(mode, turboQ5, quality, false, false);
        } else {
            const bool weakCpu = !gpu && (hw.cores < 4 || !hw.avx2);
            m = plan(mode, weakCpu ? QStringLiteral("base") : QStringLiteral("small-q5_1"), fast, false, false);
        }
        if (gpu) {
            m.vramMb = sttVramMb(m.sttModel) + (m.voice == QLatin1String("clone") ? voiceMb : 0)
                + (m.quality == quality && (!m.cloneAllowed || m.llmBesideClone) ? LlmVramMb : 0);
        }
        p.translate.insert(mode, m);
    }

    const WhisperHw::Recommendation dictation = WhisperHw::recommend(hw);
    p.dictationModel = dictation.model;
    p.dictationGpu = dictation.gpu;
    p.designEngine = p.cloneOnGpu && p.cloneEngine == QLatin1String("omnivoice") ? QStringLiteral("omnivoice") : QStringLiteral("parler");
    p.readVoice = p.tier == QLatin1String("gpu6") || p.tier == QLatin1String("gpu8") ? QStringLiteral("clone") : QStringLiteral("piper");
    return p;
}

// ------------------------------------------------------------------ hardware.conf

QString path()
{
    const QString env = qEnvironmentVariable("LINGMO_HARDWARE_CONF");
    if (!env.isEmpty())
        return env;
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/lingmoos/hardware.conf");
}

Profile read(const QString &file)
{
    Profile p;
    if (!QFileInfo::exists(file))
        return p;
    QSettings s(file, QSettings::IniFormat);
    if (s.status() != QSettings::NoError)
        return p;
    s.beginGroup(QStringLiteral("Hardware"));
    p.version = s.value(QStringLiteral("version")).toInt();
    p.detected = QDateTime::fromString(s.value(QStringLiteral("detected")).toString(), Qt::ISODate);
    p.gpuVendor = s.value(QStringLiteral("gpuVendor")).toString();
    p.gpuName = s.value(QStringLiteral("gpuName")).toString();
    p.vramMb = s.value(QStringLiteral("vramMb")).toInt();
    p.cuda = s.value(QStringLiteral("cuda")).toBool();
    p.backends = s.value(QStringLiteral("backends")).toString().split(QLatin1Char('+'), Qt::SkipEmptyParts);
    p.ramMb = s.value(QStringLiteral("ramMb")).toInt();
    p.cpuName = s.value(QStringLiteral("cpuName")).toString();
    p.cores = s.value(QStringLiteral("cores")).toInt();
    p.threads = s.value(QStringLiteral("threads")).toInt();
    p.avx2 = s.value(QStringLiteral("avx2")).toBool();
    p.tier = s.value(QStringLiteral("tier")).toString();
    s.endGroup();
    if (!Tiers.contains(p.tier)) {
        p.tier.clear();
        return p;
    }

    // a hand-edited value that makes no sense invalidates the file (it is recomputed)
    bool ok = true;
    s.beginGroup(QStringLiteral("Translate"));
    for (const QString &mode : modes()) {
        ModePlan m;
        m.sttModel = s.value(mode + QStringLiteral("/sttModel")).toString();
        m.quality = s.value(mode + QStringLiteral("/quality")).toString();
        m.voice = s.value(mode + QStringLiteral("/voice")).toString();
        m.cloneAllowed = s.value(mode + QStringLiteral("/cloneAllowed")).toBool();
        m.llmBesideClone = s.value(mode + QStringLiteral("/llmBesideClone")).toBool();
        m.vramMb = s.value(mode + QStringLiteral("/vramMb")).toInt();
        ok = ok && WhisperHw::findModel(m.sttModel) && Qualities.contains(m.quality) && Voices.contains(m.voice);
        p.translate.insert(mode, m);
    }
    s.endGroup();
    p.dictationModel = s.value(QStringLiteral("Dictation/model")).toString();
    p.dictationGpu = s.value(QStringLiteral("Dictation/gpu")).toBool();
    p.cloneEngine = s.value(QStringLiteral("Voice/cloneEngine")).toString();
    p.cloneOnGpu = s.value(QStringLiteral("Voice/cloneOnGpu")).toBool();
    p.designEngine = s.value(QStringLiteral("VoiceStudio/designEngine")).toString();
    p.readVoice = s.value(QStringLiteral("VoiceStudio/readVoice")).toString();
    ok = ok && WhisperHw::findModel(p.dictationModel)
        && (p.cloneEngine == QLatin1String("omnivoice") || p.cloneEngine == QLatin1String("chatterbox"))
        && (p.designEngine == QLatin1String("omnivoice") || p.designEngine == QLatin1String("parler"))
        && (p.readVoice == QLatin1String("clone") || p.readVoice == QLatin1String("piper"));
    if (!ok)
        p.tier.clear();
    return p;
}

bool write(const Profile &p, const QString &file)
{
    QDir().mkpath(QFileInfo(file).absolutePath());
    QSettings s(file, QSettings::IniFormat);
    s.clear();
    s.beginGroup(QStringLiteral("Hardware"));
    s.setValue(QStringLiteral("version"), p.version);
    s.setValue(QStringLiteral("detected"), p.detected.toString(Qt::ISODate));
    s.setValue(QStringLiteral("gpuVendor"), p.gpuVendor);
    s.setValue(QStringLiteral("gpuName"), p.gpuName);
    s.setValue(QStringLiteral("vramMb"), p.vramMb);
    s.setValue(QStringLiteral("cuda"), p.cuda);
    s.setValue(QStringLiteral("backends"), p.backends.join(QLatin1Char('+')));
    s.setValue(QStringLiteral("ramMb"), p.ramMb);
    s.setValue(QStringLiteral("cpuName"), p.cpuName);
    s.setValue(QStringLiteral("cores"), p.cores);
    s.setValue(QStringLiteral("threads"), p.threads);
    s.setValue(QStringLiteral("avx2"), p.avx2);
    s.setValue(QStringLiteral("tier"), p.tier);
    s.endGroup();
    s.beginGroup(QStringLiteral("Translate"));
    for (const QString &mode : modes()) {
        const ModePlan m = p.translate.value(mode);
        s.setValue(mode + QStringLiteral("/sttModel"), m.sttModel);
        s.setValue(mode + QStringLiteral("/quality"), m.quality);
        s.setValue(mode + QStringLiteral("/voice"), m.voice);
        s.setValue(mode + QStringLiteral("/cloneAllowed"), m.cloneAllowed);
        s.setValue(mode + QStringLiteral("/llmBesideClone"), m.llmBesideClone);
        s.setValue(mode + QStringLiteral("/vramMb"), m.vramMb);
    }
    s.endGroup();
    s.setValue(QStringLiteral("Dictation/model"), p.dictationModel);
    s.setValue(QStringLiteral("Dictation/gpu"), p.dictationGpu);
    s.setValue(QStringLiteral("Voice/cloneEngine"), p.cloneEngine);
    s.setValue(QStringLiteral("Voice/cloneOnGpu"), p.cloneOnGpu);
    s.setValue(QStringLiteral("VoiceStudio/designEngine"), p.designEngine);
    s.setValue(QStringLiteral("VoiceStudio/readVoice"), p.readVoice);
    s.sync();
    return s.status() == QSettings::NoError;
}

bool sameHardware(const Profile &a, const Profile &b)
{
    return a.gpuVendor == b.gpuVendor && a.gpuName == b.gpuName && std::abs(a.vramMb - b.vramMb) <= 64 && a.cuda == b.cuda
        && a.cpuName == b.cpuName;
}

Profile detect()
{
    return HwProfile::recommend(WhisperHw::probe());
}

Profile current(bool *recomputed)
{
    const Profile fresh = detect();
    const Profile stored = read();
    if (stored.isValid() && stored.version == PolicyVersion && sameHardware(stored, fresh)) {
        if (recomputed)
            *recomputed = false;
        return stored;
    }
    write(fresh);
    if (recomputed)
        *recomputed = true;
    return fresh;
}

} // namespace HwProfile

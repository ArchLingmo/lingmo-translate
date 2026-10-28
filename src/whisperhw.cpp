/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

// Keep identical to the copy in the other repository (see whisperhw.h).

#include "whisperhw.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>
#include <cmath>

namespace WhisperHw {

namespace {

QString readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

QString rooted(const QString &root, const QString &path)
{
    return root.isEmpty() ? path : QDir::cleanPath(root + path);
}

// --- Speed of whisper-cli 1.9.5 with runArguments(), in seconds of audio per
// second of work (× real time), model loading excluded. Measured on 2026-10-17
// with Portuguese speech (LibriVox, Machado de Assis), 60 s and 300 s clips:
//   CPU: AMD Ryzen 5 1600 (6 cores / 12 threads, AVX2, no AVX-512), -ng -t 12
//   GPU: NVIDIA GeForce RTX 2060 6 GB, ggml-cuda 0.26, -t 4 -fa
// Measured (greedy, -t 6, 60 s; -t 12 adds ~9 %, long files ~25 %):
//   tiny 14.2  base 7.6  base-q5_1 12.3  base-q8_0 13.5  small 1.8
//   small-q5_1 3.1  small-q8_0 4.2  medium-q5_0 1.0  large-v3-turbo-q5_0 0.72
//   large-v3-turbo 0.46; beam search (the default) is 1.6-1.9× slower.
//   GPU, 300 s: large-v3-turbo 45 (beam: 26), small 48, medium-q5_0 33,
//   large-v3-turbo-q5_0 10 (quantised models are slow on CUDA: use full ones).
// The CPU column is the 60 s figure × 1.36 (12 threads, long files); models
// not measured are derived from their neighbours (q8_0 ≈ 1.33× q5, f16 ≈ 0.57× q5).
struct Speed {
    const char *model;
    double cpu;
    double gpu;
};
const Speed ReferenceSpeeds[] = {
    {"tiny", 19, 60},
    {"base-q5_1", 16.7, 40},
    {"base-q8_0", 18.3, 45},
    {"base", 10.3, 55},
    {"small-q5_1", 4.3, 30},
    {"small-q8_0", 5.7, 35},
    {"small", 2.4, 48},
    {"medium-q5_0", 1.36, 33},
    {"medium-q8_0", 1.8, 30},
    {"medium", 0.78, 38},
    {"large-v3-turbo-q5_0", 0.98, 10},
    {"large-v3-turbo-q8_0", 1.3, 15},
    {"large-v3-turbo", 0.63, 45},
    {"large-v3-q5_0", 0.35, 5},
    {"large-v3", 0.22, 14},
};
constexpr int ReferenceCores = 6;
// Word times by DTW (runArguments) turn flash attention off: 18:29 of audio on
// the RTX 2060 took 36.7 s instead of 25.7 s; on the CPU -dtw adds ~15 %
constexpr double DtwCostGpu = 0.7;
constexpr double DtwCostCpu = 0.85;

const Speed *referenceSpeed(const QString &model)
{
    for (const Speed &s : ReferenceSpeeds) {
        if (model == QLatin1String(s.model))
            return &s;
    }
    return nullptr;
}

// Backend id from a library file name: libggml-cuda.so -> "cuda"
const QStringList GpuBackends = {QStringLiteral("cuda"), QStringLiteral("hip"), QStringLiteral("vulkan"),
                                 QStringLiteral("sycl")};

QString backendLabel(const QString &backend)
{
    if (backend == QLatin1String("cuda"))
        return QStringLiteral("CUDA");
    if (backend == QLatin1String("hip"))
        return QStringLiteral("ROCm");
    if (backend == QLatin1String("vulkan"))
        return QStringLiteral("Vulkan");
    if (backend == QLatin1String("sycl"))
        return QStringLiteral("SYCL");
    return backend;
}

// Backends whisper.cpp can use for a vendor, preferred first
QStringList backendsFor(const QString &vendor)
{
    if (vendor == QLatin1String("nvidia"))
        return {QStringLiteral("cuda"), QStringLiteral("vulkan")};
    if (vendor == QLatin1String("amd"))
        return {QStringLiteral("hip"), QStringLiteral("vulkan")};
    if (vendor == QLatin1String("intel"))
        return {QStringLiteral("sycl"), QStringLiteral("vulkan")};
    return {};
}

// A dedicated GPU worth using: enough video memory, or a discrete NVIDIA/AMD
// card whose memory we couldn't read
bool gpuStrongEnough(const Hardware &hw)
{
    if (hw.gpuVendor.isEmpty())
        return false;
    if (hw.vramGb > 0)
        return hw.vramGb >= 1.5;
    return hw.gpuVendor == QLatin1String("nvidia");
}

// The model for a GPU that runs whisper
QString gpuModel(const Hardware &hw)
{
    const double vram = hw.vramGb > 0 ? hw.vramGb : 4;
    if (vram >= 3.8 && (hw.ramGb <= 0 || hw.ramGb >= 7))
        return QStringLiteral("large-v3-turbo");
    if (vram >= 2.8)
        return QStringLiteral("medium");
    return QStringLiteral("medium-q5_0");
}

// The model for the processor: the best one that keeps up with the speech
// (ReferenceSpeeds, scaled by cores). q8_0 is the fastest form on a CPU.
//   small-q8_0  ~5.7× real time on 6 cores (3.8× on 4): laptops and desktops
//   medium-q8_0 ~1.8× on 6 cores, ~4.8× on 16: only on very strong CPUs
//   base-q8_0   below 4 cores, without AVX2 or with little memory
QString cpuModel(const Hardware &hw)
{
    const int cores = hw.cores > 0 ? hw.cores : std::max(1, hw.threads / 2);
    const double ram = hw.ramGb > 0 ? hw.ramGb : 4;
    if (ram < 3 || cores < 2)
        return QStringLiteral("tiny");
    if (!hw.avx2 || cores < 4 || ram < 6)
        return QStringLiteral("base-q8_0");
    if (ram >= 16 && (cores >= 16 || (cores >= 12 && hw.avx512)))
        return QStringLiteral("medium-q8_0");
    return QStringLiteral("small-q8_0");
}

QString modelFamilyPrefix(const QString &name)
{
    static const QRegularExpression quant(QStringLiteral("-q\\d_\\d$"));
    QString n = name;
    n.remove(quant);
    return n;
}

} // namespace

// ------------------------------------------------------------------ models

// Sizes and sha256 values: the Hugging Face file listing of ggerganov/whisper.cpp
// (https://huggingface.co/api/models/ggerganov/whisper.cpp/tree/main, "size" and
// "lfs.oid", which is the sha256 of the file), checked 2026-10-17.
// Memory: whisper.cpp README "Memory usage" table (tiny 273 MB, base 388 MB,
// small 852 MB, medium 2.1 GB, large 3.9 GB); turbo and quantised models are
// estimated as their file size plus the overhead of the full model of the same
// encoder size. Speed and Portuguese quality are relative judgements from the
// OpenAI Whisper paper / model card (Portuguese is one of its best languages;
// turbo is large-v3 with 4 decoder layers, close in accuracy and much faster).
const QList<Model> &catalogue()
{
    static const QList<Model> list = {
        {QStringLiteral("tiny"), 77691713, QStringLiteral("be07e048e1e599ad46341c8d2a135645097a538221678b7acdd1b1919c6e1b21"), 273, 5, 1, false},
        {QStringLiteral("base-q5_1"), 59707625, QStringLiteral("422f1ae452ade6f30a004d7e5c6a43195e4433bc370bf23fac9cc591f01a8898"), 300, 5, 1, true},
        {QStringLiteral("base-q8_0"), 81768585, QStringLiteral("c577b9a86e7e048a0b7eada054f4dd79a56bbfa911fbdacf900ac5b567cbb7d9"), 320, 5, 1, true},
        {QStringLiteral("base"), 147951465, QStringLiteral("60ed5bc3dd14eea856493d334349b405782ddcaf0028d4b5df4088345fba2efe"), 388, 5, 1, false},
        {QStringLiteral("small-q5_1"), 190085487, QStringLiteral("ae85e4a935d7a567bd102fe55afc16bb595bdb618e11b2fc7591bc08120411bb"), 570, 4, 2, true},
        {QStringLiteral("small-q8_0"), 264464607, QStringLiteral("49c8fb02b65e6049d5fa6c04f81f53b867b5ec9540406812c643f177317f779f"), 640, 4, 2, true},
        {QStringLiteral("small"), 487601967, QStringLiteral("1be3a9b2063867b937e64e2ec7483364a79917e157fa98c5d94b5c1fffea987b"), 852, 4, 2, false},
        {QStringLiteral("medium-q5_0"), 539212467, QStringLiteral("19fea4b380c3a618ec4723c3eef2eb785ffba0d0538cf43f8f235e7b3b34220f"), 1100, 2, 3, true},
        {QStringLiteral("medium-q8_0"), 823369779, QStringLiteral("42a1ffcbe4167d224232443396968db4d02d4e8e87e213d3ee2e03095dea6502"), 1400, 2, 3, true},
        {QStringLiteral("medium"), 1533763059, QStringLiteral("6c14d5adee5f86394037b4e4e8b59f1673b6cee10e3cf0b11bbdbee79c156208"), 2100, 2, 3, false},
        {QStringLiteral("large-v3-turbo-q5_0"), 574041195, QStringLiteral("394221709cd5ad1f40c46e6031ca61bce88931e6e088c188294c6d5a55ffa7e2"), 1200, 3, 3, true},
        {QStringLiteral("large-v3-turbo-q8_0"), 874188075, QStringLiteral("317eb69c11673c9de1e1f0d459b253999804ec71ac4c23c17ecf5fbe24e259a1"), 1500, 3, 4, true},
        {QStringLiteral("large-v3-turbo"), 1624555275, QStringLiteral("1fc70f774d38eb169993ac391eea357ef47c88757ef72ee5943879b7e8e2bc69"), 2200, 3, 4, false},
        {QStringLiteral("large-v3-q5_0"), 1081140203, QStringLiteral("d75795ecff3f83b5faa89d1900604ad8c780abd5739fae406de19f23ecd98ad1"), 2100, 1, 4, true},
        {QStringLiteral("large-v3"), 3095033483, QStringLiteral("64d182b440b98d5203c4f9bd541544d84c605196c4f7b845dfa11fb23594d1e2"), 3900, 1, 4, false},
    };
    return list;
}

const Model *findModel(const QString &name)
{
    for (const Model &model : catalogue()) {
        if (model.name == name)
            return &model;
    }
    return nullptr;
}

QString family(const QString &name)
{
    return modelFamilyPrefix(name.trimmed().toLower());
}

int familyRank(const QString &name)
{
    const QString f = family(name);
    if (f == QLatin1String("tiny"))
        return 0;
    if (f == QLatin1String("base"))
        return 1;
    if (f == QLatin1String("small"))
        return 2;
    if (f == QLatin1String("medium"))
        return 3;
    if (f == QLatin1String("large-v3-turbo"))
        return 4;
    if (f.startsWith(QLatin1String("large")))
        return 5;
    return -1;
}

// ------------------------------------------------------------------ hardware

QString Hardware::gpuBackend() const
{
    for (const QString &b : backendsFor(gpuVendor)) {
        if (backends.contains(b))
            return b;
    }
    return {};
}

bool Hardware::gpuUsable() const
{
    return !gpuBackend().isEmpty() && gpuStrongEnough(*this);
}

void parseCpuinfo(const QString &text, Hardware &hw)
{
    QSet<QString> coreIds;
    int processors = 0;
    int cpuCores = 0;
    QString physical;
    bool flagsRead = false;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon < 0)
            continue;
        const QString key = line.left(colon).trimmed();
        const QString value = line.mid(colon + 1).trimmed();
        if (key == QLatin1String("processor")) {
            ++processors;
        } else if (key == QLatin1String("physical id")) {
            physical = value;
        } else if (key == QLatin1String("core id")) {
            coreIds.insert(physical + QLatin1Char(':') + value);
        } else if (key == QLatin1String("cpu cores")) {
            cpuCores = std::max(cpuCores, value.toInt());
        } else if (key == QLatin1String("model name") && hw.cpuName.isEmpty()) {
            hw.cpuName = value.simplified();
        } else if ((key == QLatin1String("flags") || key == QLatin1String("Features")) && !flagsRead) {
            flagsRead = true;
            const QStringList flags = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            hw.avx2 = flags.contains(QLatin1String("avx2"));
            hw.avx512 = flags.contains(QLatin1String("avx512f"));
        }
    }
    hw.threads = processors;
    if (!coreIds.isEmpty())
        hw.cores = int(coreIds.size());
    else if (cpuCores > 0)
        hw.cores = cpuCores;
    else
        hw.cores = processors;
}

double parseMeminfoGb(const QString &text)
{
    static const QRegularExpression re(QStringLiteral("^MemTotal:\\s+(\\d+)\\s+kB"), QRegularExpression::MultilineOption);
    const QRegularExpressionMatch m = re.match(text);
    return m.hasMatch() ? m.captured(1).toDouble() / (1024.0 * 1024.0) : 0;
}

bool parseNvidiaSmi(const QString &text, QString *name, double *vramGb)
{
    bool found = false;
    double best = 0;
    for (const QString &line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const int comma = line.lastIndexOf(QLatin1Char(','));
        if (comma < 0)
            continue;
        bool ok = false;
        const double gb = line.mid(comma + 1).trimmed().toDouble(&ok) / 1024.0;
        if (!ok || (found && gb <= best))
            continue;
        found = true;
        best = gb;
        if (name)
            *name = line.left(comma).trimmed();
        if (vramGb)
            *vramGb = gb;
    }
    return found;
}

QStringList backendsFromFiles(const QStringList &fileNames)
{
    static const QRegularExpression re(QStringLiteral("^libggml-([a-z]+)\\.so(\\.[0-9.]+)?$"));
    QStringList found;
    for (const QString &file : fileNames) {
        const QRegularExpressionMatch m = re.match(QFileInfo(file).fileName());
        if (m.hasMatch() && GpuBackends.contains(m.captured(1)) && !found.contains(m.captured(1)))
            found << m.captured(1);
    }
    std::sort(found.begin(), found.end(), [](const QString &a, const QString &b) {
        return GpuBackends.indexOf(a) < GpuBackends.indexOf(b);
    });
    return found;
}

QString backendPackage(const QString &vendor)
{
    return vendor == QLatin1String("nvidia") ? QStringLiteral("ggml-cuda") : QStringLiteral("ggml-vulkan");
}

QString vendorDisplayName(const QString &vendor)
{
    if (vendor == QLatin1String("nvidia"))
        return QStringLiteral("NVIDIA");
    if (vendor == QLatin1String("amd"))
        return QStringLiteral("AMD");
    if (vendor == QLatin1String("intel"))
        return QStringLiteral("Intel");
    return vendor;
}

Hardware fromSpec(const QString &spec)
{
    Hardware hw;
    for (const QString &part : spec.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const int eq = part.indexOf(QLatin1Char('='));
        if (eq < 0)
            continue;
        const QString key = part.left(eq).trimmed().toLower();
        const QString value = part.mid(eq + 1).trimmed();
        if (key == QLatin1String("ram"))
            hw.ramGb = value.toDouble();
        else if (key == QLatin1String("cores"))
            hw.cores = value.toInt();
        else if (key == QLatin1String("threads"))
            hw.threads = value.toInt();
        else if (key == QLatin1String("avx2"))
            hw.avx2 = value == QLatin1String("1");
        else if (key == QLatin1String("avx512"))
            hw.avx512 = value == QLatin1String("1");
        else if (key == QLatin1String("cpu"))
            hw.cpuName = value;
        else if (key == QLatin1String("gpu"))
            hw.gpuVendor = value.toLower();
        else if (key == QLatin1String("gpuname"))
            hw.gpuName = value;
        else if (key == QLatin1String("vram"))
            hw.vramGb = value.toDouble();
        else if (key == QLatin1String("backends"))
            hw.backends = value.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    }
    if (hw.threads <= 0)
        hw.threads = hw.cores;
    if (hw.gpuName.isEmpty() && !hw.gpuVendor.isEmpty())
        hw.gpuName = vendorDisplayName(hw.gpuVendor) + QStringLiteral(" GPU");
    return hw;
}

QString toSpec(const Hardware &hw)
{
    return QStringLiteral("ram=%1,cores=%2,threads=%3,avx2=%4,avx512=%5,gpu=%6,gpuname=%7,vram=%8,backends=%9")
        .arg(QString::number(hw.ramGb, 'f', 1))
        .arg(hw.cores)
        .arg(hw.threads)
        .arg(hw.avx2 ? 1 : 0)
        .arg(hw.avx512 ? 1 : 0)
        .arg(hw.gpuVendor, hw.gpuName, QString::number(hw.vramGb, 'f', 1), hw.backends.join(QLatin1Char('+')));
}

Hardware probe(const QString &root)
{
    if (root.isEmpty()) {
        const QString spec = qEnvironmentVariable("LINGMO_WHISPER_HW");
        if (!spec.isEmpty())
            return fromSpec(spec);
    }

    Hardware hw;
    parseCpuinfo(readFile(rooted(root, QStringLiteral("/proc/cpuinfo"))), hw);
    if (hw.threads <= 0) {
        hw.threads = QThread::idealThreadCount();
        hw.cores = hw.threads;
    }
    hw.ramGb = parseMeminfoGb(readFile(rooted(root, QStringLiteral("/proc/meminfo"))));

    // NVIDIA: nvidia-smi has the name and the memory (MiB)
    if (root.isEmpty()) {
        QString smi = QStandardPaths::findExecutable(QStringLiteral("nvidia-smi"));
        if (smi.isEmpty())
            smi = QStandardPaths::findExecutable(QStringLiteral("nvidia-smi"), {QStringLiteral("/usr/bin"), QStringLiteral("/opt/bin")});
        if (!smi.isEmpty()) {
            QProcess process;
            process.start(smi, {QStringLiteral("--query-gpu=name,memory.total"), QStringLiteral("--format=csv,noheader,nounits")});
            if (process.waitForFinished(5000) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0) {
                if (parseNvidiaSmi(QString::fromUtf8(process.readAllStandardOutput()), &hw.gpuName, &hw.vramGb))
                    hw.gpuVendor = QStringLiteral("nvidia");
            } else {
                process.kill();
                process.waitForFinished(1000);
            }
        }
    }

    // The PCI vendor of each graphics card: AMD (amdgpu has the VRAM size),
    // Intel, and NVIDIA when nvidia-smi isn't there
    if (hw.gpuVendor != QLatin1String("nvidia")) {
        const QDir drm(rooted(root, QStringLiteral("/sys/class/drm")));
        const QStringList cards = drm.entryList({QStringLiteral("card*")}, QDir::Dirs | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &card : cards) {
            if (card.contains(QLatin1Char('-'))) // connectors: card0-HDMI-A-1
                continue;
            const QString device = drm.absoluteFilePath(card + QStringLiteral("/device/"));
            const QString vendor = readFile(device + QStringLiteral("vendor")).trimmed();
            const QString product = readFile(device + QStringLiteral("product_name")).trimmed();
            if (vendor == QLatin1String("0x10de")) {
                hw.gpuVendor = QStringLiteral("nvidia");
                hw.gpuName = product.isEmpty() ? QStringLiteral("NVIDIA GPU") : product;
                hw.vramGb = 0;
                break; // a discrete NVIDIA card wins over an integrated GPU
            }
            if (vendor == QLatin1String("0x1002")) {
                const double gb = readFile(device + QStringLiteral("mem_info_vram_total")).trimmed().toDouble() / (1024.0 * 1024.0 * 1024.0);
                if (hw.gpuVendor != QLatin1String("amd") || gb > hw.vramGb) {
                    hw.gpuVendor = QStringLiteral("amd");
                    hw.gpuName = product.isEmpty() ? QStringLiteral("AMD GPU") : product;
                    hw.vramGb = gb;
                }
            } else if (vendor == QLatin1String("0x8086") && hw.gpuVendor.isEmpty()) {
                hw.gpuVendor = QStringLiteral("intel");
                hw.gpuName = product.isEmpty() ? QStringLiteral("Intel GPU") : product;
            }
        }
    }

    // ggml 0.26 loads its backends from /usr/lib/ggml (libggml-cuda.so...)
    QStringList files;
    for (const QString &dir : {QStringLiteral("/usr/lib/ggml"), QStringLiteral("/usr/local/lib/ggml"),
                               QStringLiteral("/usr/lib"), QStringLiteral("/usr/local/lib")})
        files << QDir(rooted(root, dir)).entryList({QStringLiteral("libggml-*.so*")}, QDir::Files);
    hw.backends = backendsFromFiles(files);
    return hw;
}

// ------------------------------------------------------------------ recommendation

Recommendation recommend(const Hardware &hw)
{
    Recommendation r;
    const int cores = hw.cores > 0 ? hw.cores : std::max(1, hw.threads);
    if (hw.gpuUsable()) {
        r.model = gpuModel(hw);
        r.gpu = true;
        r.device = hw.gpuName.isEmpty() ? vendorDisplayName(hw.gpuVendor) : hw.gpuName;
        r.reason = hw.vramGb > 0
            ? QCoreApplication::translate("WhisperHw", "Runs on the graphics card with %1 (%2 GB of video memory).")
                  .arg(backendLabel(hw.gpuBackend()), QLocale().toString(hw.vramGb, 'f', hw.vramGb < 10 ? 1 : 0))
            : QCoreApplication::translate("WhisperHw", "Runs on the graphics card with %1.").arg(backendLabel(hw.gpuBackend()));
        return r;
    }

    r.model = cpuModel(hw);
    r.device = QStringLiteral("CPU");
    const QString cpu = QCoreApplication::translate("WhisperHw", "%n processor core(s)", nullptr, cores);
    if (gpuStrongEnough(hw) && hw.gpuBackend().isEmpty()) {
        Hardware upgraded = hw;
        upgraded.backends << (backendPackage(hw.gpuVendor) == QLatin1String("ggml-cuda") ? QStringLiteral("cuda") : QStringLiteral("vulkan"));
        r.upgradePackage = backendPackage(hw.gpuVendor);
        r.upgradeModel = gpuModel(upgraded);
        // what the GPU would do against what the processor does now
        const double onGpu = estimatedSpeed(r.upgradeModel, upgraded, true);
        const double onCpu = estimatedSpeed(r.model, hw, false);
        r.upgradeSpeedup = onGpu > 0 && onCpu > 0 ? onGpu / onCpu : 0;
        const QString vendor = vendorDisplayName(hw.gpuVendor);
        r.upgradeHint = r.upgradeSpeedup >= 2
            ? QCoreApplication::translate("WhisperHw", "Install %1 to use your %2 (up to ~%3× faster)").arg(r.upgradePackage, vendor).arg(int(std::round(r.upgradeSpeedup)))
            : QCoreApplication::translate("WhisperHw", "Install %1 to use your %2").arg(r.upgradePackage, vendor);
        r.reason = QCoreApplication::translate("WhisperHw", "Your %1 can't run whisper yet: ggml has no backend for it, so this model is for the processor (%2).")
                       .arg(hw.gpuName.isEmpty() ? vendor : hw.gpuName, cpu);
    } else if (gpuStrongEnough(hw)) {
        r.reason = QCoreApplication::translate("WhisperHw", "The graphics card has too little video memory, so this model is for the processor (%1).").arg(cpu);
    } else {
        r.reason = QCoreApplication::translate("WhisperHw", "The fastest model that still keeps up with the speech on the processor (%1).").arg(cpu);
    }
    return r;
}

bool tooHeavy(const QString &model, const Hardware &hw)
{
    const int rank = familyRank(model);
    return rank >= 0 && rank > familyRank(recommend(hw).model);
}

double estimatedSpeed(const QString &model, const Hardware &hw, bool gpu)
{
    const Speed *ref = referenceSpeed(model);
    if (!ref)
        return 0;
    if (gpu) {
        // Measured with CUDA; Vulkan and ROCm are slower, more so on small cards
        double factor = 1.0;
        const QString backend = hw.gpuBackend().isEmpty() ? QStringLiteral("cuda") : hw.gpuBackend();
        if (backend == QLatin1String("vulkan"))
            factor = 0.6;
        else if (backend == QLatin1String("hip"))
            factor = 0.8;
        else if (backend == QLatin1String("sycl"))
            factor = 0.5;
        return ref->gpu * factor * DtwCostGpu;
    }
    const int cores = std::clamp(hw.cores > 0 ? hw.cores : std::max(1, hw.threads / 2), 1, 16);
    double factor = double(cores) / ReferenceCores;
    if (!hw.avx2)
        factor *= 0.35;
    else if (hw.avx512)
        factor *= 1.15;
    return ref->cpu * factor * DtwCostCpu;
}

double estimatedSpeed(const QString &model, const Hardware &hw)
{
    return estimatedSpeed(model, hw, hw.gpuUsable());
}

QString speedText(double speed)
{
    if (speed <= 0)
        return {};
    const QLocale locale;
    if (speed >= 1)
        return QCoreApplication::translate("WhisperHw", "~%1× real time").arg(locale.toString(speed, 'f', speed < 10 ? 1 : 0));
    return QCoreApplication::translate("WhisperHw", "~%1 min per minute of audio").arg(locale.toString(1.0 / speed, 'f', 1));
}

QString runSummary(const QString &model, const Hardware &hw)
{
    const bool gpu = hw.gpuUsable();
    QStringList parts = {model, gpu ? QStringLiteral("GPU") : QStringLiteral("CPU")};
    const QString speed = speedText(estimatedSpeed(model, hw, gpu));
    if (!speed.isEmpty())
        parts << speed;
    return parts.join(QStringLiteral(" · "));
}

// ------------------------------------------------------------------ running whisper-cli

int threadCount(const Hardware &hw, bool gpu)
{
    const int threads = hw.threads > 0 ? hw.threads : std::max(1, hw.cores);
    if (gpu)
        return std::clamp(threads, 1, 4);
    return std::clamp(threads, 1, 16);
}

QString dtwPreset(const QString &model)
{
    const QString f = family(model);
    if (f == QLatin1String("tiny") || f == QLatin1String("base") || f == QLatin1String("small") || f == QLatin1String("medium"))
        return f;
    if (f == QLatin1String("large-v3-turbo"))
        return QStringLiteral("large.v3.turbo");
    if (f == QLatin1String("large-v3"))
        return QStringLiteral("large.v3");
    if (f == QLatin1String("large-v2"))
        return QStringLiteral("large.v2");
    if (f == QLatin1String("large-v1") || f == QLatin1String("large"))
        return QStringLiteral("large.v1");
    return {};
}

QStringList runArguments(const Hardware &hw, bool gpu, const QString &model)
{
    // word times from DTW need the attention weights: no flash attention then
    const QString dtw = dtwPreset(model);
    QStringList args = {QStringLiteral("-t"), QString::number(threadCount(hw, gpu))};
    if (dtw.isEmpty())
        args << QStringLiteral("-fa");
    else
        args << QStringLiteral("-nfa") << QStringLiteral("-dtw") << dtw;
    args << QStringList{QStringLiteral("-bs"), QStringLiteral("1"), QStringLiteral("-bo"), QStringLiteral("1"),
                        // against repetition loops over music and game sound: no text
                        // context carried between windows, no "[Música]"-like tokens
                        QStringLiteral("-mc"), QStringLiteral("0"), QStringLiteral("-sns")};
    if (!gpu)
        args << QStringLiteral("-ng");
    return args;
}

// ------------------------------------------------------------------ installing a GPU backend

Command installCommand(const QString &package, const QString &root)
{
    const QString helper = QStringLiteral("/usr/lib/lingmo-store/lingmo-store-helper");
    if (QFileInfo(rooted(root, helper)).isExecutable())
        return {QStringLiteral("pkexec"), {helper, QStringLiteral("install"), package}};
    return {QStringLiteral("pkexec"), {QStringLiteral("pacman"), QStringLiteral("-S"), QStringLiteral("--needed"),
                                       QStringLiteral("--noconfirm"), package}};
}

} // namespace WhisperHw

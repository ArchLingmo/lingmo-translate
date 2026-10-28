/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

#include "setup.h"
#include "paths.h"
#include "whisperhw.h"

#include <QDir>
#include <QFile>
#include <QLocale>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QVariantMap>

namespace
{
// Pinned sources. Models keep their own licences (shown in the app and in Settings › IA).
const char *SileroUrl = "https://github.com/snakers4/silero-vad/raw/v5.1.2/src/silero_vad/data/silero_vad.onnx";
const char *WhisperBase = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-%1.bin";
// OPUS-MT originals (Marian .npz): CTranslate2 converts them with numpy only, no PyTorch
const char *EnRomanceUrl =
    "https://object.pouta.csc.fi/OPUS-MT-models/en-fr+fr_BE+fr_CA+fr_FR+wa+frp+oc+ca+rm+lld+fur+lij+lmo+es+es_AR+es_CL+es_CO+es_CR+es_DO+es_EC+es_ES+es_GT+es_HN+es_MX+es_NI+es_PA+es_PE+es_PR+es_SV+es_UY+es_VE+pt+pt_br+pt_BR+pt_PT+gl+lad+an+mwl+it+it_IT+co+nap+scn+vec+sc+ro+la/opus-2020-04-21.zip";
const char *RomanceEnUrl =
    "https://object.pouta.csc.fi/OPUS-MT-models/fr+fr_BE+fr_CA+fr_FR+wa+frp+oc+ca+rm+lld+fur+lij+lmo+es+es_AR+es_CL+es_CO+es_CR+es_DO+es_EC+es_ES+es_GT+es_HN+es_MX+es_NI+es_PA+es_PE+es_PR+es_SV+es_UY+es_VE+pt+pt_br+pt_BR+pt_PT+gl+lad+an+mwl+it+it_IT+co+nap+scn+vec+sc+ro+la-en/opus-2020-04-01.zip";
// Asian languages → English (CC-BY 4.0, Helsinki-NLP), chained by the engine with en→ROMANCE:
// measured ja→pt chrF 54.3 (Tatoeba) / 44.4 (FLEURS) against 46.8 / 40.5 for the direct jpn-por model
const char *JaEnUrl = "https://object.pouta.csc.fi/OPUS-MT-models/ja-en/opus-2019-12-18.zip";
const char *ZhEnUrl = "https://object.pouta.csc.fi/Tatoeba-MT-models/zho-eng/opus-2020-07-17.zip";
const char *KoEnUrl = "https://object.pouta.csc.fi/Tatoeba-MT-models/kor-eng/opus-2021-02-18.zip";
// piper voices (CC0 datasets): Brazilian Portuguese faber and US English joe
const char *PiperPtUrl = "https://huggingface.co/rhasspy/piper-voices/resolve/main/pt/pt_BR/faber/medium/pt_BR-faber-medium.onnx";
const char *PiperEnUrl = "https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/joe/medium/en_US-joe-medium.onnx";
// The voice engine: exactly what Lingmo Studio installs for dubbing (lingmo-studio/src/app/dubber.cpp)
const char *ChatterboxGit = "git+https://github.com/resemble-ai/chatterbox.git@5de7a54aa4e5e2baadb0182dde554908b48b85c2";
// piper-tts (GPL-3.0) speaks in-process: no AUR package needed
const char *EnginePackages[] = {"numpy>=2", "onnxruntime>=1.20,<2", "ctranslate2>=4.5,<5", "sentencepiece>=0.2,<0.3", "pyyaml>=6", "piper-tts>=1.3,<2"};

QString lastLines(const QString &text, int n)
{
    QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    while (lines.size() > n)
        lines.removeFirst();
    return lines.join(QLatin1Char('\n'));
}
}

Setup::Setup(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
}

bool Setup::has(const QString &c) const
{
    const QString models = Paths::modelsDir();
    if (c == QLatin1String("engine"))
        return QFileInfo::exists(Paths::venvPython()) && QFileInfo::exists(models + QStringLiteral("/silero_vad.onnx"))
            && QFileInfo::exists(Paths::venvDir() + QStringLiteral("/.lingmo-ready"));
    if (c == QLatin1String("stt"))
        return QFileInfo::exists(Paths::whisperModel(QStringLiteral("small-q5_1"))) || QFileInfo::exists(Paths::whisperModel(QStringLiteral("base")));
    if (c.startsWith(QLatin1String("whisper:")))
        return QFileInfo::exists(Paths::whisperModel(c.mid(8)));
    if (c == QLatin1String("mt"))
        return QFileInfo::exists(models + QStringLiteral("/mt-en-romance/model.bin")) && QFileInfo::exists(models + QStringLiteral("/mt-romance-en/model.bin"));
    if (c == QLatin1String("mt-ja") || c == QLatin1String("mt-zh") || c == QLatin1String("mt-ko"))
        return QFileInfo::exists(models + QStringLiteral("/mt-") + c.mid(3) + QStringLiteral("-en/model.bin"));
    if (c == QLatin1String("voices"))
        return QFileInfo::exists(Paths::translateDir() + QStringLiteral("/piper/en_US-joe-medium.onnx.json"))
            && (QFileInfo::exists(Paths::translateDir() + QStringLiteral("/piper/pt_BR-faber-medium.onnx.json"))
                || QFileInfo::exists(QStringLiteral("/usr/share/piper-voices/pt/pt_BR/faber/medium/pt_BR-faber-medium.onnx.json")));
    if (c == QLatin1String("clone"))
        return voiceEngineReady();
    return false;
}

bool Setup::ready() const
{
    return has(QStringLiteral("engine")) && has(QStringLiteral("stt")) && has(QStringLiteral("mt"));
}

bool Setup::voiceEngineReady() const
{
    // the Studio's environment: present if Chatterbox imports there (checked lazily by the engine)
    const QString py = Paths::voiceVenvPython();
    if (!QFileInfo::exists(py))
        return false;
    QDir lib(QFileInfo(py).absolutePath() + QStringLiteral("/../lib"));
    for (const QString &d : lib.entryList({QStringLiteral("python3*")}, QDir::Dirs)) {
        if (QFileInfo::exists(lib.absoluteFilePath(d) + QStringLiteral("/site-packages/chatterbox/mtl_tts.py")))
            return true;
    }
    return false;
}

void Setup::setRecommendedWhisper(const QStringList &models)
{
    if (models == m_recommendedWhisper)
        return;
    m_recommendedWhisper = models;
    Q_EMIT changed();
}

QStringList Setup::defaultComponents() const
{
    QStringList comps = {QStringLiteral("engine"), QStringLiteral("stt"), QStringLiteral("mt"), QStringLiteral("voices")};
    for (const QString &m : m_recommendedWhisper) {
        if (m != QLatin1String("small-q5_1"))
            comps << QStringLiteral("whisper:") + m;
    }
    return comps;
}

QString Setup::pendingSize() const
{
    // approximate download sizes of what is missing
    qint64 total = 0;
    for (const QString &c : defaultComponents()) {
        if (has(c))
            continue;
        if (c == QLatin1String("engine"))
            total += 172000000;
        else if (c == QLatin1String("stt"))
            total += 190085487;
        else if (c == QLatin1String("mt"))
            total += 2 * 288000000; // converted to 82 MB each, the downloads are deleted
        else if (c == QLatin1String("voices"))
            total += 126000000;
        else if (const WhisperHw::Model *m = WhisperHw::findModel(c.mid(8)))
            total += m->bytes;
    }
    return total > 0 ? QLocale().formattedDataSize(total, 1, QLocale::DataSizeSIFormat) : QString();
}

QVariantList Setup::components() const
{
    auto row = [this](const QString &id, const QString &title, const QString &size, const QString &license, bool required) {
        return QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("title"), title}, {QStringLiteral("size"), size},
                           {QStringLiteral("license"), license}, {QStringLiteral("installed"), has(id)},
                           {QStringLiteral("required"), required}, {QStringLiteral("recommended"), false}};
    };
    QVariantList whisper;
    for (const QString &m : m_recommendedWhisper) {
        const WhisperHw::Model *model = WhisperHw::findModel(m);
        if (m == QLatin1String("small-q5_1") || !model)
            continue;
        QVariantMap r = row(QStringLiteral("whisper:") + m, tr("Speech recognition for this computer: Whisper %1").arg(m),
                            QLocale().formattedDataSize(model->bytes, 1, QLocale::DataSizeSIFormat), QStringLiteral("MIT"), false);
        r.insert(QStringLiteral("recommended"), true);
        whisper << r;
    }
    return QVariantList{row(QStringLiteral("engine"), tr("Translation engine and voice detection (Silero VAD)"), QStringLiteral("~170 MB"), QStringLiteral("MIT"), true),
            row(QStringLiteral("stt"), tr("Speech recognition: Whisper small"), QStringLiteral("181 MB"), QStringLiteral("MIT"), true)}
        + whisper + QVariantList{
            row(QStringLiteral("mt"), tr("Instant translation: Opus-MT English ↔ Portuguese"), QStringLiteral("164 MB"), QStringLiteral("CC-BY 4.0 · Helsinki-NLP"), true),
            row(QStringLiteral("mt-ja"), tr("Instant translation from Japanese (anime, dramas): Opus-MT Japanese → English"), QStringLiteral("80 MB"), QStringLiteral("CC-BY 4.0 · Helsinki-NLP"), false),
            row(QStringLiteral("mt-zh"), tr("Instant translation from Chinese: Opus-MT Chinese → English"), QStringLiteral("80 MB"), QStringLiteral("CC-BY 4.0 · Helsinki-NLP"), false),
            row(QStringLiteral("mt-ko"), tr("Instant translation from Korean: Opus-MT Korean → English"), QStringLiteral("80 MB"), QStringLiteral("CC-BY 4.0 · Helsinki-NLP"), false),
            row(QStringLiteral("voices"), tr("Voices for dubbing (piper: Faber in Portuguese, Joe in English)"), QStringLiteral("126 MB"), QStringLiteral("GPL-3.0 · CC0"), true),
            row(QStringLiteral("clone"), tr("Your own voice (Chatterbox, shared with Studio's dubbing)"), QStringLiteral("~6 GB"), QStringLiteral("MIT"), false)};
}

QList<Setup::Step> Setup::plan(const QStringList &wanted) const
{
    QStringList comps = wanted;
    if (comps.isEmpty())
        comps = defaultComponents();
    QList<Step> steps;
    const QString models = Paths::modelsDir();
    const QString uv = QStandardPaths::findExecutable(QStringLiteral("uv"));
    const QString py = Paths::venvPython();

    if ((comps.contains(QLatin1String("engine")) || comps.contains(QLatin1String("mt")) || comps.contains(QLatin1String("mt-ja"))
         || comps.contains(QLatin1String("mt-zh")) || comps.contains(QLatin1String("mt-ko")))
        && !has(QStringLiteral("engine"))) {
        Step venv;
        venv.label = tr("Creating the translation engine…");
        venv.skipIfExists = py;
        if (!uv.isEmpty()) {
            venv.program = uv;
            venv.args = {QStringLiteral("venv"), QStringLiteral("--python"), QStandardPaths::findExecutable(QStringLiteral("python3")), Paths::venvDir()};
        } else {
            venv.program = QStandardPaths::findExecutable(QStringLiteral("python3"));
            venv.args = {QStringLiteral("-m"), QStringLiteral("venv"), Paths::venvDir()};
        }
        steps << venv;
        Step pip;
        pip.label = tr("Installing the translation engine (about 170 MB)…");
        QStringList pkgs;
        for (const char *p : EnginePackages)
            pkgs << QString::fromLatin1(p);
        if (!uv.isEmpty()) {
            pip.program = uv;
            pip.args = QStringList{QStringLiteral("pip"), QStringLiteral("install"), QStringLiteral("--python"), py} + pkgs;
        } else {
            pip.program = py;
            pip.args = QStringList{QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"), QStringLiteral("--no-input")} + pkgs;
        }
        steps << pip;
        Step vad;
        vad.kind = Step::Download;
        vad.label = tr("Downloading voice detection (Silero VAD)…");
        vad.url = QString::fromLatin1(SileroUrl);
        vad.target = models + QStringLiteral("/silero_vad.onnx");
        vad.approxBytes = 2327524;
        steps << vad;
        Step mark;
        mark.label = tr("Checking the translation engine…");
        mark.program = py;
        mark.args = {QStringLiteral("-c"),
                     QStringLiteral("import numpy, onnxruntime, ctranslate2, sentencepiece, piper, pathlib; pathlib.Path(%1).touch()")
                         .arg(QStringLiteral("'") + Paths::venvDir() + QStringLiteral("/.lingmo-ready'"))};
        steps << mark;
    }
    if (comps.contains(QLatin1String("stt")) && !has(QStringLiteral("stt"))) {
        Step w;
        w.kind = Step::Download;
        w.label = tr("Downloading speech recognition (Whisper small, 181 MB)…");
        w.url = QString::fromLatin1(WhisperBase).arg(QStringLiteral("small-q5_1"));
        w.target = Paths::whisperModel(QStringLiteral("small-q5_1"));
        w.approxBytes = 190085487;
        steps << w;
    }
    // the models of the hardware recommendation (hwprofile.h), from the same place
    for (const QString &c : comps) {
        const WhisperHw::Model *model = c.startsWith(QLatin1String("whisper:")) ? WhisperHw::findModel(c.mid(8)) : nullptr;
        if (!model || has(c))
            continue;
        Step w;
        w.kind = Step::Download;
        w.label = tr("Downloading speech recognition (Whisper %1, %2)…").arg(model->name, QLocale().formattedDataSize(model->bytes, 1, QLocale::DataSizeSIFormat));
        w.url = QString::fromLatin1(WhisperBase).arg(model->name);
        w.target = Paths::whisperModel(model->name);
        w.approxBytes = model->bytes;
        w.skipIfExists = w.target;
        steps << w;
    }
    QList<QPair<QString, const char *>> pairs;
    if (comps.contains(QLatin1String("mt")) && !has(QStringLiteral("mt")))
        pairs = {{QStringLiteral("en-romance"), EnRomanceUrl}, {QStringLiteral("romance-en"), RomanceEnUrl}};
    const QList<QPair<QString, const char *>> asian = {{QStringLiteral("ja"), JaEnUrl}, {QStringLiteral("zh"), ZhEnUrl}, {QStringLiteral("ko"), KoEnUrl}};
    for (const auto &a : asian) {
        if (comps.contains(QStringLiteral("mt-") + a.first) && !has(QStringLiteral("mt-") + a.first))
            pairs.append({a.first + QStringLiteral("-en"), a.second});
    }
    if (!pairs.isEmpty()) {
        for (const auto &pair : pairs) {
            const QString zip = Paths::translateDir() + QStringLiteral("/download-") + pair.first + QStringLiteral(".zip");
            const QString unpacked = Paths::translateDir() + QStringLiteral("/download-") + pair.first;
            const QString out = models + QStringLiteral("/mt-") + pair.first;
            Step d;
            d.kind = Step::Download;
            d.label = pair.first == QLatin1String("en-romance")   ? tr("Downloading the English → Portuguese translator (290 MB)…")
                : pair.first == QLatin1String("romance-en") ? tr("Downloading the Portuguese → English translator (290 MB)…")
                : pair.first == QLatin1String("ja-en")      ? tr("Downloading the Japanese → English translator (290 MB)…")
                : pair.first == QLatin1String("zh-en")      ? tr("Downloading the Chinese → English translator (290 MB)…")
                                                            : tr("Downloading the Korean → English translator (290 MB)…");
            d.url = QString::fromLatin1(pair.second);
            d.target = zip;
            d.approxBytes = 288000000;
            d.skipIfExists = out + QStringLiteral("/model.bin");
            steps << d;
            Step c;
            c.label = tr("Preparing the translator for fast use…");
            c.program = py;
            // unpack, convert the Marian weights to CTranslate2 int8, keep the tokenisers, delete the download
            c.args = {QStringLiteral("-I"), QStringLiteral("-c"),
                      QStringLiteral("import sys, zipfile, shutil, pathlib\n"
                                     "from ctranslate2.converters import OpusMTConverter\n"
                                     "zip_, tmp, out = sys.argv[1:4]\n"
                                     "zipfile.ZipFile(zip_).extractall(tmp)\n"
                                     "OpusMTConverter(tmp).convert(out, quantization='int8', force=True)\n"
                                     "for f in ('source.spm', 'target.spm', 'LICENSE', 'README.md'):\n"
                                     "    p = pathlib.Path(tmp, f)\n"
                                     "    if p.exists(): shutil.copy(p, out)\n"
                                     "shutil.rmtree(tmp); pathlib.Path(zip_).unlink()\n"),
                      zip, unpacked, out};
            c.skipIfExists = out + QStringLiteral("/model.bin");
            steps << c;
        }
    }
    if (comps.contains(QLatin1String("voices")) && !has(QStringLiteral("voices"))) {
        const QString dir = Paths::translateDir() + QStringLiteral("/piper/");
        const QList<QPair<const char *, QString>> voices = {{PiperPtUrl, QStringLiteral("pt_BR-faber-medium.onnx")}, {PiperEnUrl, QStringLiteral("en_US-joe-medium.onnx")}};
        for (const auto &v : voices) {
            if (v.second.startsWith(QLatin1String("pt_BR"))
                && QFileInfo::exists(QStringLiteral("/usr/share/piper-voices/pt/pt_BR/faber/medium/pt_BR-faber-medium.onnx")))
                continue; // the system package has it
            Step onnx;
            onnx.kind = Step::Download;
            onnx.label = tr("Downloading a voice for dubbing (63 MB)…");
            onnx.url = QString::fromLatin1(v.first);
            onnx.target = dir + v.second;
            onnx.approxBytes = 63201294;
            onnx.skipIfExists = onnx.target + QStringLiteral(".json");
            steps << onnx;
            Step json = onnx;
            json.url = QString::fromLatin1(v.first) + QStringLiteral(".json");
            json.target = onnx.target + QStringLiteral(".json");
            json.approxBytes = 5000;
            steps << json;
        }
    }
    if (comps.contains(QLatin1String("clone")) && !voiceEngineReady()) {
        // Same environment and pins as Lingmo Studio's dubbing (one install serves both)
        const QString venv = QFileInfo(Paths::voiceVenvPython()).absolutePath() + QStringLiteral("/..");
        const QString vpy = Paths::voiceVenvPython();
        Step v;
        v.label = tr("Creating the voice engine environment…");
        v.skipIfExists = vpy;
        if (!uv.isEmpty()) {
            v.program = uv;
            v.args = {QStringLiteral("venv"), QStringLiteral("--python"), QStringLiteral("3.11"), QDir::cleanPath(venv)};
        } else {
            v.program = QStandardPaths::findExecutable(QStringLiteral("python3.11"));
            v.args = {QStringLiteral("-m"), QStringLiteral("venv"), QDir::cleanPath(venv)};
        }
        steps << v;
        for (bool pypi : {false, true}) {
            Step p;
            p.label = tr("Installing the voice engine (about 6 GB, this takes a while)…");
            const QStringList pkgs = {pypi ? QStringLiteral("chatterbox-tts") : QStringLiteral("chatterbox-tts @ ") + QString::fromLatin1(ChatterboxGit),
                                      QStringLiteral("setuptools<81")};
            if (!uv.isEmpty()) {
                p.program = uv;
                p.args = QStringList{QStringLiteral("pip"), QStringLiteral("install"), QStringLiteral("--python"), vpy} + pkgs;
            } else {
                p.program = vpy;
                p.args = QStringList{QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"), QStringLiteral("--no-input")} + pkgs;
            }
            p.optionalFail = !pypi; // the pinned GitHub build failed: try the PyPI one
            steps << p;
        }
        Step warm;
        warm.label = tr("Downloading the voice model (about 3 GB)…");
        warm.program = vpy;
        warm.args = {Paths::cloneWorker(), QStringLiteral("warmup")};
        steps << warm;
    }
    return steps;
}

void Setup::install(const QStringList &components)
{
    if (m_busy)
        return;
    m_steps = plan(components);
    m_index = 0;
    m_total = m_steps.size();
    m_failed = false;
    m_skipNext = false;
    m_log.clear();
    m_busy = true;
    QDir().mkpath(Paths::modelsDir());
    QDir().mkpath(Paths::whisperDir());
    QDir().mkpath(Paths::translateDir() + QStringLiteral("/piper"));
    Q_EMIT changed();
    next();
}

void Setup::next()
{
    while (m_index < m_steps.size()) {
        const Step s = m_steps.at(m_index);
        if (m_skipNext) {
            // the step before succeeded: its fallback is not needed
            m_skipNext = false;
            ++m_index;
            continue;
        }
        if (!s.skipIfExists.isEmpty() && QFileInfo::exists(s.skipIfExists)) {
            ++m_index;
            continue;
        }
        if (s.kind == Step::Download)
            download(s);
        else
            runStep(s);
        return;
    }
    m_busy = false;
    m_progress = 1;
    m_message = tr("Ready.");
    Q_EMIT changed();
    Q_EMIT finished(true);
}

void Setup::setMessage(const QString &m, double p)
{
    m_message = m;
    m_progress = p;
    Q_EMIT changed();
}

void Setup::runStep(const Step &s)
{
    if (s.program.isEmpty()) {
        fail(tr("Python 3 was not found. Install it with “sudo pacman -S python”."));
        return;
    }
    setMessage(s.label, -1);
    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    m_proc->setProcessEnvironment(env);
    connect(m_proc, &QProcess::readyRead, this, [this, s] {
        const QString text = QString::fromUtf8(m_proc->readAll());
        m_log += text;
        if (m_log.size() > 20000)
            m_log.remove(0, m_log.size() - 20000);
        const QString tail = lastLines(text, 1).simplified().left(90);
        if (!tail.isEmpty() && !tail.startsWith(QLatin1Char('{')))
            setMessage(s.label + QLatin1Char('\n') + tail, -1);
    });
    connect(m_proc, &QProcess::finished, this, [this, s](int code, QProcess::ExitStatus st) {
        m_proc->deleteLater();
        if (st == QProcess::NormalExit && code == 0) {
            ++m_index;
            m_skipNext = s.optionalFail; // skip the fallback that follows
            next();
            return;
        }
        if (s.optionalFail) {
            ++m_index;
            next();
            return;
        }
        fail(tr("The installation failed:") + QLatin1Char('\n') + lastLines(m_log, 4));
    });
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            fail(tr("The installer could not be started."));
    });
    m_proc->start(s.program, s.args);
}

void Setup::download(const Step &s)
{
    setMessage(s.label, 0);
    QDir().mkpath(QFileInfo(s.target).absolutePath());
    m_file = new QFile(s.target + QStringLiteral(".part"), this);
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(tr("Could not write %1").arg(s.target));
        return;
    }
    QNetworkRequest req{QUrl(s.url)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("lingmo-translate/1.0"));
    m_reply = m_net->get(req);
    connect(m_reply, &QNetworkReply::readyRead, this, [this] { m_file->write(m_reply->readAll()); });
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this, s](qint64 got, qint64 total) {
        if (total <= 0)
            total = s.approxBytes;
        if (total > 0)
            setMessage(s.label + QStringLiteral("\n%1 / %2 MB").arg(got / 1048576).arg(total / 1048576), double(got) / double(total));
    });
    connect(m_reply, &QNetworkReply::finished, this, [this, s] {
        m_file->write(m_reply->readAll());
        m_file->close();
        const bool ok = m_reply->error() == QNetworkReply::NoError && m_file->size() > 0;
        const QString err = m_reply->errorString();
        m_reply->deleteLater();
        if (!ok) {
            m_file->remove();
            m_file->deleteLater();
            fail(tr("The download failed: %1").arg(err));
            return;
        }
        QFile::remove(s.target);
        m_file->rename(s.target);
        m_file->deleteLater();
        m_file = nullptr;
        ++m_index;
        next();
    });
}

void Setup::cancel()
{
    if (m_reply)
        m_reply->abort();
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc->deleteLater();
    }
    if (m_busy) {
        m_busy = false;
        m_failed = true;
        m_message = tr("Cancelled. It continues where it stopped the next time.");
        Q_EMIT changed();
    }
}

void Setup::fail(const QString &message)
{
    m_busy = false;
    m_failed = true;
    m_message = message;
    Q_EMIT changed();
    Q_EMIT finished(false);
}

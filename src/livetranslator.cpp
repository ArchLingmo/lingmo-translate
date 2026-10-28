/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

#include "livetranslator.h"

#include <QLocale>
#include "paths.h"
#include "setup.h"
#include "virtualmic.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QLocalSocket>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <algorithm>
#include <memory>

// The engine lowers the video while the translated voice speaks and keeps the original
// volumes in this file; if it dies before putting them back, the app does it
void restoreDucking()
{
    const QString path = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/lingmo-translate-ducked.json");
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject saved = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    for (auto it = saved.begin(); it != saved.end(); ++it) {
        QStringList args{QStringLiteral("set-sink-input-volume"), it.key()};
        // {"vols": [...], "app": ..., "level": ...}; older files: the volumes alone
        const QJsonArray vols = it.value().isObject() ? it.value().toObject().value(QStringLiteral("vols")).toArray() : it.value().toArray();
        for (const QJsonValue &v : vols)
            args << QString::number(v.toInteger());
        QProcess::execute(QStringLiteral("pactl"), args);
    }
    QFile::remove(path);
}

namespace
{
const QString Group = QStringLiteral("Translate/");

QUrl ollamaUrl(const QString &path)
{
    QString host = qEnvironmentVariable("OLLAMA_HOST", QStringLiteral("127.0.0.1:11434"));
    if (!host.contains(QLatin1String("://")))
        host.prepend(QLatin1String("http://"));
    QUrl u(host);
    u.setPath(path);
    return u;
}

QByteArray pactlJson(const QStringList &args)
{
    QProcess p;
    p.start(QStringLiteral("pactl"), QStringList{QStringLiteral("-f"), QStringLiteral("json")} + args);
    p.waitForFinished(4000);
    return p.readAllStandardOutput();
}

QString langName(const QString &code)
{
    static const QHash<QString, QString> names = {{QStringLiteral("en"), QStringLiteral("EN")}, {QStringLiteral("pt"), QStringLiteral("PT")},
                                                  {QStringLiteral("es"), QStringLiteral("ES")}, {QStringLiteral("fr"), QStringLiteral("FR")},
                                                  {QStringLiteral("it"), QStringLiteral("IT")}, {QStringLiteral("de"), QStringLiteral("DE")}};
    return names.value(code, code.toUpper());
}

// Video memory each part needs on the GPU (measured on an RTX 2060; hwprofile.h, README.md);
// whisper's depends on the model: HwProfile::sttVramMb()
constexpr int LlmMb = HwProfile::LlmVramMb;          // qwen3.5:4b Q4_K_M with its default context
constexpr int CloneMb = HwProfile::ChatterboxVramMb; // Chatterbox Multilingual v3 during generation
constexpr int OmniMb = HwProfile::OmniVoiceVramMb;   // OmniVoice fp16 during generation (whisper + OmniVoice peaked at 3.0 GB, README.md)
constexpr int SttMarginMb = 100;                     // whisper's buffers beyond the measured figure

const QString DefaultPiper = QStringLiteral("piper:/usr/share/piper-voices/pt/pt_BR/faber/medium/pt_BR-faber-medium.onnx");
// the speech models offered per mode, fastest first
const QStringList SttChoices = {QStringLiteral("small-q5_1"), QStringLiteral("base"), QStringLiteral("large-v3-turbo-q5_0"),
                                QStringLiteral("large-v3-turbo")};

const QStringList Modes = {QStringLiteral("subtitles"), QStringLiteral("dubbed"), QStringLiteral("speak"), QStringLiteral("call")};

// a mode's pair when nothing is saved: listening translates anything into Portuguese,
// speaking turns my Portuguese into English
QString defaultFrom(const QString &mode)
{
    return mode == QLatin1String("speak") ? QStringLiteral("pt") : QStringLiteral("auto");
}

QString defaultTo(const QString &mode)
{
    return mode == QLatin1String("speak") ? QStringLiteral("en") : QStringLiteral("pt");
}

// Listening dubbed (measured on a 7-minute anime section, see README.md)
constexpr int DubEndSilenceMs = 300;   // end of a line after this pause
constexpr int DubSplitAfterMs = 1500;  // a line longer than this also ends at a shorter pause...
constexpr int DubSplitSilenceMs = 200; // ...of this length
constexpr int DubMaxUtteranceMs = 5000;
constexpr int DubVoiceWaitMs = 0;      // a short line is said at once with the instant translation...
constexpr int DubVoiceWaitLongMs = 1600; // ...a long one waits up to this for the LLM (meaning first)
}

LiveTranslator::LiveTranslator(Setup *setup, QObject *parent)
    : QObject(parent)
    , m_setup(setup)
    , m_vmic(new VirtualMic(this))
    , m_net(new QNetworkAccessManager(this))
    , m_settings(Paths::configFile(), QSettings::IniFormat)
    , m_voiceConfWatcher(new QFileSystemWatcher(this))
{
    connect(m_vmic, &VirtualMic::activeChanged, this, &LiveTranslator::stateChanged);
    connect(this, &LiveTranslator::settingsChanged, this, &LiveTranslator::recommendationChanged);
    connect(this, &LiveTranslator::voicesChanged, this, &LiveTranslator::recommendationChanged);
    connect(m_setup, &Setup::finished, this, &LiveTranslator::settingsChanged); // a speech model arrived
    // what this computer runs best (~/.config/lingmoos/hardware.conf, recomputed when the GPU changed)
    reloadHardware(true);
    migrateLanguages();
    migrateRecommendations();
    // the cloned voice's engine is chosen in Settings: follow it while running (as chotkeys does);
    // the folders too, since QSettings saves by replacing the file (and Voice Studio's consent lives there)
    connect(m_voiceConfWatcher, &QFileSystemWatcher::fileChanged, this, &LiveTranslator::readVoiceConf);
    connect(m_voiceConfWatcher, &QFileSystemWatcher::directoryChanged, this, &LiveTranslator::readVoiceConf);
    readVoiceConf();
    refreshSources();
}

LiveTranslator::~LiveTranslator()
{
    if (m_engine) {
        m_engine->disconnect(this);
        send({{QStringLiteral("cmd"), QStringLiteral("quit")}});
        if (!m_engine->waitForFinished(4000))
            m_engine->kill();
    }
    m_vmic->remove();
}

QVariant LiveTranslator::value(const QString &key, const QVariant &def) const
{
    return m_settings.value(Group + key, def);
}

void LiveTranslator::setValue(const QString &key, const QVariant &v)
{
    if (m_settings.value(Group + key) == v)
        return;
    m_settings.setValue(Group + key, v);
    m_settings.sync();
    Q_EMIT settingsChanged();
    if (key == QLatin1String("voice") || key.endsWith(QLatin1String("/voice")))
        Q_EMIT voicesChanged();
}

void LiveTranslator::migrateRecommendations()
{
    // until 2026-10 one translation choice and one speech model served every mode; now each
    // mode follows the hardware recommendation unless the user set it. An old "quality" (the
    // default then) or "small-q5_1" says nothing; anything else was a choice: kept per mode.
    const QVariant oldQuality = m_settings.value(Group + QStringLiteral("quality"));
    const QVariant oldStt = m_settings.value(Group + QStringLiteral("sttModel"));
    bool changed = false;
    for (const QString &m : Modes) {
        if (oldQuality.toString() == QLatin1String("fast") && !m_settings.contains(Group + m + QStringLiteral("/quality")))
            m_settings.setValue(Group + m + QStringLiteral("/quality"), oldQuality);
        if (oldStt.isValid() && oldStt.toString() != QLatin1String("small-q5_1") && SttChoices.contains(oldStt.toString())
            && !m_settings.contains(Group + m + QStringLiteral("/sttModel")))
            m_settings.setValue(Group + m + QStringLiteral("/sttModel"), oldStt);
    }
    for (const QString &k : {QStringLiteral("quality"), QStringLiteral("sttModel")}) {
        if (m_settings.contains(Group + k)) {
            m_settings.remove(Group + k);
            changed = true;
        }
    }
    // dubbed listening speaks with piper only: a cloned voice chosen there goes back to the default
    if (m_settings.value(Group + QStringLiteral("dubbed/voice")).toString().startsWith(QLatin1String("profile:"))) {
        m_settings.remove(Group + QStringLiteral("dubbed/voice"));
        changed = true;
    }
    if (changed)
        m_settings.sync();
}

void LiveTranslator::reloadHardware(bool probe)
{
    HwProfile::Profile p;
    if (probe) {
        p = HwProfile::current();
    } else {
        // hardware.conf changed on disk (Voice Studio's "Find the best configuration"):
        // taken when it still describes this computer
        p = HwProfile::read();
        if (!p.isValid() || p.version != HwProfile::PolicyVersion || (m_hw.isValid() && !HwProfile::sameHardware(p, m_hw)))
            return;
        bool same = p.tier == m_hw.tier && p.cloneEngine == m_hw.cloneEngine;
        for (const QString &m : Modes) {
            const HwProfile::ModePlan a = p.mode(m), b = m_hw.mode(m);
            same = same && a.sttModel == b.sttModel && a.quality == b.quality && a.voice == b.voice && a.cloneAllowed == b.cloneAllowed
                && a.llmBesideClone == b.llmBesideClone;
        }
        if (same)
            return;
    }
    if (!p.isValid())
        return;
    m_hw = p;
    m_setup->setRecommendedWhisper(m_hw.whisperModels());
    Q_EMIT recommendationChanged();
    Q_EMIT settingsChanged();
}

QString LiveTranslator::modeQuality(const QString &mode) const
{
    const QString v = value(mode + QStringLiteral("/quality")).toString();
    if (v == QLatin1String("quality") || v == QLatin1String("fast"))
        return v;
    const QString rec = m_hw.mode(mode).quality;
    return rec.isEmpty() ? QStringLiteral("fast") : rec;
}

QString LiveTranslator::modeSttModel(const QString &mode) const
{
    const QString v = value(mode + QStringLiteral("/sttModel")).toString();
    if (SttChoices.contains(v))
        return v;
    const QString rec = m_hw.mode(mode).sttModel;
    return rec.isEmpty() ? QStringLiteral("small-q5_1") : rec;
}

void LiveTranslator::setSttModel(const QString &v)
{
    if (SttChoices.contains(v))
        setValue(mode() + QStringLiteral("/sttModel"), v);
}

QVariantList LiveTranslator::sttModels() const
{
    const QString rec = m_hw.mode(mode()).sttModel;
    QVariantList out;
    for (const QString &id : SttChoices) {
        QString title;
        if (id == QLatin1String("small-q5_1"))
            title = tr("Whisper small: fastest");
        else if (id == QLatin1String("base"))
            title = tr("Whisper base: for a slow processor");
        else if (id == QLatin1String("large-v3-turbo-q5_0"))
            title = tr("Whisper large-v3-turbo, compressed: accurate");
        else
            title = tr("Whisper large-v3-turbo: most accurate");
        const WhisperHw::Model *m = WhisperHw::findModel(id);
        out << QVariantMap{{QStringLiteral("id"), id},
                           {QStringLiteral("title"), title},
                           {QStringLiteral("size"), m ? QLocale().formattedDataSize(m->bytes, 1, QLocale::DataSizeSIFormat) : QString()},
                           {QStringLiteral("vramMb"), HwProfile::sttVramMb(id)},
                           {QStringLiteral("installed"), QFileInfo::exists(Paths::whisperModel(id))},
                           {QStringLiteral("recommended"), id == rec}};
    }
    return out;
}

QVariantMap LiveTranslator::recommended() const
{
    const HwProfile::ModePlan p = m_hw.mode(mode());
    return {{QStringLiteral("sttModel"), p.sttModel},
            {QStringLiteral("quality"), p.quality},
            {QStringLiteral("voice"), p.voice},
            {QStringLiteral("cloneAllowed"), p.cloneAllowed},
            {QStringLiteral("llmBesideClone"), p.llmBesideClone},
            {QStringLiteral("vramMb"), p.vramMb},
            {QStringLiteral("tier"), m_hw.tier},
            {QStringLiteral("gpuName"), m_hw.gpuName},
            {QStringLiteral("vramTotalMb"), m_hw.vramMb},
            {QStringLiteral("cuda"), m_hw.cuda},
            {QStringLiteral("cloneEngine"), m_hw.cloneEngine}};
}

QString LiveTranslator::voiceLang() const
{
    // the language the translated voice speaks
    const QString m = mode();
    if (m == QLatin1String("call"))
        return fromLang() == QLatin1String("auto") ? QStringLiteral("en") : fromLang();
    return toLang();
}

QString LiveTranslator::piperFor(const QString &lang) const
{
    // the voice piperOrEspeak() would use for the language (what the voices list shows)
    if (lang == QLatin1String("pt")) {
        const QString system = QStringLiteral("/usr/share/piper-voices/pt/pt_BR/faber/medium/pt_BR-faber-medium.onnx");
        return QStringLiteral("piper:") + (QFileInfo::exists(system) ? system : Paths::translateDir() + QStringLiteral("/piper/pt_BR-faber-medium.onnx"));
    }
    if (lang == QLatin1String("en"))
        return QStringLiteral("piper:") + Paths::translateDir() + QStringLiteral("/piper/en_US-joe-medium.onnx");
    return QStringLiteral("espeak");
}

QString LiveTranslator::recommendedVoice() const
{
    const HwProfile::ModePlan p = m_hw.mode(mode());
    if (p.voice == QLatin1String("none") || mode() == QLatin1String("subtitles"))
        return {};
    if (p.voice == QLatin1String("clone")) {
        // your own voice, the most recent ready profile (none recorded yet: piper meanwhile)
        for (const QVariant &v : profiles()) {
            const QVariantMap m = v.toMap();
            if (m.value(QStringLiteral("ready")).toBool())
                return QStringLiteral("profile:") + m.value(QStringLiteral("name")).toString();
        }
    }
    return piperFor(voiceLang());
}

bool LiveTranslator::followsRecommendation() const
{
    const QString m = mode();
    return !m_settings.contains(Group + m + QStringLiteral("/sttModel")) && !m_settings.contains(Group + m + QStringLiteral("/quality"))
        && (m == QLatin1String("subtitles") || !m_settings.contains(Group + voiceKey()));
}

void LiveTranslator::useRecommended()
{
    const QString m = mode();
    QStringList keys = {m + QStringLiteral("/sttModel"), m + QStringLiteral("/quality")};
    if (m != QLatin1String("subtitles"))
        keys << voiceKey();
    for (const QString &k : keys)
        m_settings.remove(Group + k);
    m_settings.sync();
    Q_EMIT settingsChanged();
    Q_EMIT voicesChanged();
}

void LiveTranslator::setVoice(const QString &v)
{
    // dubbed listening: piper (or eSpeak) only
    if (mode() == QLatin1String("dubbed") && v.startsWith(QLatin1String("profile:")))
        return;
    setValue(voiceKey(), v);
}

void LiveTranslator::migrateLanguages()
{
    // until 2026-10 one pair served every mode ([Translate] from/to): it becomes each mode's
    // starting pair, except where it makes no sense for speaking (it was a listening pair)
    const QVariant oldFrom = m_settings.value(Group + QStringLiteral("from"));
    const QVariant oldTo = m_settings.value(Group + QStringLiteral("to"));
    if (!oldFrom.isValid() && !oldTo.isValid())
        return;
    for (const QString &m : Modes) {
        if (m_settings.contains(Group + m + QStringLiteral("/from")) || m_settings.contains(Group + m + QStringLiteral("/to")))
            continue;
        const QString from = oldFrom.isValid() ? oldFrom.toString() : defaultFrom(m);
        const QString to = oldTo.isValid() ? oldTo.toString() : defaultTo(m);
        if (m == QLatin1String("speak") && (from == QLatin1String("auto") ? QStringLiteral("pt") : from) == to)
            continue; // the defaults (pt → en)
        m_settings.setValue(Group + m + QStringLiteral("/from"), from);
        m_settings.setValue(Group + m + QStringLiteral("/to"), to);
    }
    m_settings.remove(Group + QStringLiteral("from"));
    m_settings.remove(Group + QStringLiteral("to"));
    m_settings.sync();
}

QString LiveTranslator::modeFrom(const QString &mode) const
{
    return value(mode + QStringLiteral("/from"), defaultFrom(mode)).toString();
}

QString LiveTranslator::modeTo(const QString &mode) const
{
    return value(mode + QStringLiteral("/to"), defaultTo(mode)).toString();
}

void LiveTranslator::setMode(const QString &v)
{
    if (v == mode() || !Modes.contains(v))
        return;
    setValue(QStringLiteral("mode"), v);
    m_detected.clear();
    Q_EMIT stateChanged(); // pairLabel, fromShort, sourceDetail: the new mode's pair
}

void LiveTranslator::setToLang(const QString &v)
{
    setValue(mode() + QStringLiteral("/to"), v);
    Q_EMIT stateChanged();
}

// ------------------------------------------------------------------ cloned voice engine

bool LiveTranslator::omniVoiceReady()
{
    // what Voice Studio reports as "omnivoiceAccepted" and installed: its consent, its venv,
    // its worker and the weights it downloaded (CC-BY-NC: never fetched or copied from here)
    QFile f(Paths::voiceStudioConfig());
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject cfg = QJsonDocument::fromJson(f.read(256 * 1024)).object();
    if (cfg.value(QStringLiteral("omnivoice")).toObject().value(QStringLiteral("accepted")).toString().isEmpty())
        return false;
    return QFileInfo::exists(Paths::omniVoicePython()) && QFileInfo::exists(Paths::omniVoiceWorker())
        && QFileInfo::exists(Paths::omniVoiceWeights() + QStringLiteral("/snapshots"));
}

void LiveTranslator::readVoiceConf()
{
    QSettings conf(Paths::voiceConf(), QSettings::IniFormat);
    // not chosen in Settings: the hardware recommendation's engine (OmniVoice when it is on)
    const bool set = conf.contains(QStringLiteral("Voice/cloneEngine"));
    const QString recommended = m_hw.cloneEngine.isEmpty() ? QStringLiteral("chatterbox") : m_hw.cloneEngine;
    const QString wanted = conf.value(QStringLiteral("Voice/cloneEngine"), recommended).toString();
    const bool omni = wanted == QLatin1String("omnivoice") && omniVoiceReady();
    const bool changed = wanted != m_cloneEngineWanted || omni != m_omniReady || set != m_cloneEngineSet;
    m_cloneEngineWanted = wanted;
    m_cloneEngineSet = set;
    m_omniReady = omni;
    // hardware.conf lives beside voice.conf: Voice Studio may have applied a new recommendation
    if (m_hw.isValid())
        reloadHardware(false);
    for (const QString &path : {Paths::voiceConf(), QFileInfo(Paths::voiceConf()).absolutePath(), Paths::voiceStudioConfig(),
                                QFileInfo(Paths::voiceStudioConfig()).absolutePath(), HwProfile::path()}) {
        if (QFileInfo::exists(path) && !m_voiceConfWatcher->files().contains(path) && !m_voiceConfWatcher->directories().contains(path))
            m_voiceConfWatcher->addPath(path);
    }
    if (changed) {
        Q_EMIT cloneEngineChanged();
        Q_EMIT voicesChanged(); // a profile is ready for OmniVoice with its recording alone
        if (omni)
            QTimer::singleShot(0, this, &LiveTranslator::prepareMissingOmniPrompts);
    }
}

QString LiveTranslator::cloneEngine() const
{
    return m_omniReady ? QStringLiteral("omnivoice") : QStringLiteral("chatterbox");
}

bool LiveTranslator::cloneEngineFallback() const
{
    return m_cloneEngineSet && m_cloneEngineWanted == QLatin1String("omnivoice") && !m_omniReady;
}

QString LiveTranslator::voice() const
{
    const QString m = mode();
    if (m == QLatin1String("dubbed")) {
        const QString v = value(QStringLiteral("dubbed/voice")).toString();
        return v.isEmpty() || v.startsWith(QLatin1String("profile:")) ? DefaultPiper : v;
    }
    const QVariant v = value(QStringLiteral("voice"));
    if (v.isValid() && !v.toString().isEmpty())
        return v.toString();
    // not chosen: the recommendation (your cloned voice when the GPU has room and one is recorded)
    if (m == QLatin1String("speak") || m == QLatin1String("call")) {
        const QString rec = recommendedVoice();
        if (rec.startsWith(QLatin1String("profile:")))
            return rec;
    }
    return DefaultPiper;
}

QString LiveTranslator::pairLabel() const
{
    // speaking: my language is never detected ("auto" means Portuguese there)
    const QString f = mode() == QLatin1String("speak") && fromLang() == QLatin1String("auto") ? QStringLiteral("pt") : fromLang();
    const QString from = f == QLatin1String("auto") ? (m_detected.isEmpty() ? tr("Auto") : langName(m_detected)) : langName(f);
    // a call goes both ways: they speak "from", I speak "to"
    return (mode() == QLatin1String("call") ? QStringLiteral("%1⇄%2") : QStringLiteral("%1→%2")).arg(from, langName(toLang()));
}

QStringList LiveTranslator::sourceLanguages() const
{
    // the "From" list (Translate page and the Island's language picker)
    return {QStringLiteral("auto"), QStringLiteral("en"), QStringLiteral("pt"), QStringLiteral("es"), QStringLiteral("fr"),
            QStringLiteral("it"), QStringLiteral("de"), QStringLiteral("ja"), QStringLiteral("zh"), QStringLiteral("ko")};
}

QString LiveTranslator::languageTitle(const QString &code) const
{
    if (code == QLatin1String("auto"))
        return tr("Detect automatically");
    if (code == QLatin1String("en"))
        return tr("English");
    if (code == QLatin1String("pt"))
        return tr("Portuguese (Brazil)");
    if (code == QLatin1String("es"))
        return tr("Spanish");
    if (code == QLatin1String("fr"))
        return tr("French");
    if (code == QLatin1String("it"))
        return tr("Italian");
    if (code == QLatin1String("de"))
        return tr("German");
    if (code == QLatin1String("ja"))
        return tr("Japanese");
    if (code == QLatin1String("zh"))
        return tr("Chinese");
    if (code == QLatin1String("ko"))
        return tr("Korean");
    return code;
}

QString LiveTranslator::fromShort() const
{
    const QString from = mode() == QLatin1String("speak") && fromLang() == QLatin1String("auto") ? QStringLiteral("pt") : fromLang();
    return from == QLatin1String("auto") ? tr("Auto") : langName(from);
}

QString LiveTranslator::sourceDetail() const
{
    const QString from = mode() == QLatin1String("speak") && fromLang() == QLatin1String("auto") ? QStringLiteral("pt") : fromLang();
    if (from != QLatin1String("auto"))
        return tr("%1 (fixed)").arg(languageTitle(from));
    if (m_detected.isEmpty())
        return tr("Detecting the language…");
    QString name = languageTitle(m_detected);
    if (QLocale().language() == QLocale::Portuguese || QLocale().language() == QLocale::Spanish)
        name = name.left(1).toLower() + name.mid(1); // "Detectado: japonês" (names are lowercase mid-sentence there)
    return tr("Detected: %1").arg(name);
}

void LiveTranslator::setFromLang(const QString &v)
{
    if (v == fromLang())
        return;
    setValue(mode() + QStringLiteral("/from"), v);
    // a running session switches at once (whisper, the translators and the voice stay loaded).
    // Speaking: my microphone's stream ("auto" is Portuguese there); a call: the other side's
    if (m_running || m_starting) {
        const bool speak = mode() == QLatin1String("speak");
        send({{QStringLiteral("cmd"), QStringLiteral("lang")},
              {QStringLiteral("stream"), speak ? QStringLiteral("out") : QStringLiteral("in")},
              {QStringLiteral("from"), speak && v == QLatin1String("auto") ? QStringLiteral("pt") : v}});
        m_detected.clear();
    }
    Q_EMIT stateChanged();
}

bool LiveTranslator::virtualMicActive() const
{
    // dubbed listening only mirrors the voice there: the microphone does not speak for the user
    return m_vmic->active() && (mode() == QLatin1String("speak") || mode() == QLatin1String("call"));
}

// ------------------------------------------------------------------ sources

void LiveTranslator::refreshSources()
{
    QVariantList apps;
    QStringList seen;
    for (const QJsonValue &v : QJsonDocument::fromJson(pactlJson({QStringLiteral("list"), QStringLiteral("sink-inputs")})).array()) {
        const QJsonObject props = v.toObject().value(QStringLiteral("properties")).toObject();
        const QString name = props.value(QStringLiteral("application.name")).toString();
        if (name.isEmpty() || name == QLatin1String("Lingmo Tradutor") || seen.contains(name))
            continue;
        seen << name;
        apps << QVariantMap{{QStringLiteral("name"), name},
                            {QStringLiteral("icon"), props.value(QStringLiteral("application.icon_name")).toString()},
                            {QStringLiteral("media"), props.value(QStringLiteral("media.name")).toString()}};
    }
    QVariantList mics;
    for (const QJsonValue &v : QJsonDocument::fromJson(pactlJson({QStringLiteral("list"), QStringLiteral("sources")})).array()) {
        const QJsonObject o = v.toObject();
        const QString name = o.value(QStringLiteral("name")).toString();
        if (name.endsWith(QLatin1String(".monitor")) || name == QLatin1String(VirtualMic::SourceName))
            continue;
        mics << QVariantMap{{QStringLiteral("name"), name}, {QStringLiteral("description"), o.value(QStringLiteral("description")).toString()}};
    }
    // headphones: the default output's port or form factor says so (Bluetooth headsets too)
    bool phones = false;
    QProcess def;
    def.start(QStringLiteral("pactl"), {QStringLiteral("get-default-sink")});
    def.waitForFinished(3000);
    const QString defaultSink = QString::fromUtf8(def.readAllStandardOutput()).trimmed();
    for (const QJsonValue &v : QJsonDocument::fromJson(pactlJson({QStringLiteral("list"), QStringLiteral("sinks")})).array()) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("name")).toString() != defaultSink)
            continue;
        const QString port = o.value(QStringLiteral("active_port")).toString().toLower();
        const QJsonObject props = o.value(QStringLiteral("properties")).toObject();
        const QString form = props.value(QStringLiteral("device.form_factor")).toString();
        phones = port.contains(QLatin1String("headphone")) || port.contains(QLatin1String("headset"))
            || form == QLatin1String("headphone") || form == QLatin1String("headset") || defaultSink.startsWith(QLatin1String("bluez"));
    }
    m_apps = apps;
    m_mics = mics;
    m_headphones = phones;
    Q_EMIT sourcesChanged();
}

QVariantList LiveTranslator::voices() const
{
    QVariantList out;
    for (const QString &root : {QStringLiteral("/usr/share/piper-voices"), Paths::translateDir() + QStringLiteral("/piper")}) {
        QDirIterator it(root, {QStringLiteral("*.onnx")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            const QString base = QFileInfo(path).completeBaseName(); // pt_BR-faber-medium
            const QStringList parts = base.split(QLatin1Char('-'));
            const QString lang = parts.value(0).left(2);
            if (lang != QLatin1String("pt") && lang != QLatin1String("en"))
                continue;
            QString name = parts.value(1);
            if (!name.isEmpty())
                name[0] = name[0].toUpper();
            out << QVariantMap{{QStringLiteral("id"), QStringLiteral("piper:") + path},
                               {QStringLiteral("title"), QStringLiteral("%1 (%2)").arg(name, parts.value(0))},
                               {QStringLiteral("kind"), QStringLiteral("piper")},
                               {QStringLiteral("lang"), lang}};
        }
    }
    for (const QVariant &p : profiles()) {
        const QVariantMap m = p.toMap();
        out << QVariantMap{{QStringLiteral("id"), QStringLiteral("profile:") + m.value(QStringLiteral("name")).toString()},
                           {QStringLiteral("title"), tr("My voice: %1").arg(m.value(QStringLiteral("name")).toString())},
                           {QStringLiteral("kind"), QStringLiteral("clone")},
                           {QStringLiteral("ready"), m.value(QStringLiteral("ready"))}};
    }
    out << QVariantMap{{QStringLiteral("id"), QStringLiteral("espeak")}, {QStringLiteral("title"), tr("eSpeak (robotic, any language)")},
                       {QStringLiteral("kind"), QStringLiteral("espeak")}};
    return out;
}

// ------------------------------------------------------------------ engine process

void LiveTranslator::ensureEngine(std::function<void()> then)
{
    if (m_engine && m_engineReady) {
        then();
        return;
    }
    m_onReady << then;
    if (m_engine)
        return;
    if (!QFileInfo::exists(Paths::venvPython())) {
        m_onReady.clear();
        fail(tr("The translation engine is not installed yet."));
        return;
    }
    m_engine = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    m_engine->setProcessEnvironment(env);
    m_engine->setProcessChannelMode(QProcess::SeparateChannels);
    m_engine->setReadChannel(QProcess::StandardOutput);
    connect(m_engine, &QProcess::readyReadStandardOutput, this, &LiveTranslator::onEngineOutput);
    connect(m_engine, &QProcess::readyReadStandardError, this, [this] {
        const QByteArray err = m_engine->readAllStandardError();
        if (!err.trimmed().isEmpty())
            qWarning().noquote() << "engine:" << QString::fromUtf8(err).trimmed().right(800);
    });
    connect(m_engine, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        restoreDucking();
        m_engine->deleteLater();
        m_engine = nullptr;
        m_engineReady = false;
        if (m_running || m_starting)
            fail(tr("The translation engine stopped (code %1).").arg(code));
        m_running = m_starting = false;
        m_vmic->remove();
        Q_EMIT stateChanged();
    });
    m_engine->start(Paths::venvPython(), {QStringLiteral("-u"), Paths::engineScript()});
}

void LiveTranslator::send(const QJsonObject &cmd)
{
    if (!m_engine)
        return;
    m_engine->write(QJsonDocument(cmd).toJson(QJsonDocument::Compact) + '\n');
}

void LiveTranslator::onEngineOutput()
{
    m_buffer += m_engine->readAllStandardOutput();
    int nl;
    while ((nl = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(nl);
        m_buffer.remove(0, nl + 1);
        const QJsonObject ev = QJsonDocument::fromJson(line).object();
        if (!ev.isEmpty())
            handle(ev);
    }
}

void LiveTranslator::handle(const QJsonObject &ev)
{
    const QString event = ev.value(QStringLiteral("event")).toString();
    if (event == QLatin1String("ready")) {
        m_engineReady = true;
        const auto pending = m_onReady;
        m_onReady.clear();
        for (const auto &f : pending)
            f();
    } else if (event == QLatin1String("state")) {
        const QString stage = ev.value(QStringLiteral("stage")).toString();
        if (ev.contains(QStringLiteral("vramFreeMb")))
            m_vramFree = ev.value(QStringLiteral("vramFreeMb")).toInt();
        if (stage == QLatin1String("loading")) {
            const QString what = ev.value(QStringLiteral("what")).toString();
            setStatus(what == QLatin1String("llm")            ? tr("Loading the quality translator…")
                      : what == QLatin1String("voice-prompt") ? tr("Preparing your voice for OmniVoice (only the first time)…")
                      : what == QLatin1String("voice")        ? tr("Loading your voice…")
                                                              : tr("Loading speech recognition…"));
        } else if (stage == QLatin1String("running")) {
            m_starting = false;
            m_running = true;
            m_paused = false;
            m_sttGpu = ev.value(QStringLiteral("sttGpu")).toBool(m_sttGpu);
            if (ev.contains(QStringLiteral("mtEngine")))
                m_mtEngine = ev.value(QStringLiteral("mtEngine")).toString();
            // everything is loaded: say so plainly, the user waits for this to talk
            setStatus(mode() == QLatin1String("speak") || mode() == QLatin1String("call") ? tr("Ready: speak now") : tr("Listening"));
        } else if (stage == QLatin1String("paused")) {
            m_paused = true;
            setStatus(tr("Paused"));
        } else if (stage == QLatin1String("stopped")) {
            m_running = m_starting = m_paused = false;
            m_vmic->remove();
            setStatus(QString());
        }
        if (ev.contains(QStringLiteral("detected"))) {
            m_detected = ev.value(QStringLiteral("detected")).toString();
        }
        if (ev.contains(QStringLiteral("notice")))
            m_notice = ev.value(QStringLiteral("notice")).toString();
        if (ev.value(QStringLiteral("capture")).toString() == QLatin1String("waiting"))
            setStatus(tr("Waiting for %1 to play sound…").arg(sourceApp()));
        else if (ev.value(QStringLiteral("capture")).toString() == QLatin1String("on") && m_running)
            setStatus(mode() == QLatin1String("speak") || mode() == QLatin1String("call") ? tr("Ready: speak now") : tr("Listening"));
        if (ev.value(QStringLiteral("voice")).toString() == QLatin1String("clone") && ev.value(QStringLiteral("device")).toString() != QLatin1String("cuda"))
            m_notice = tr("Your voice runs on the processor (no room on the GPU): it will be slow.");
        Q_EMIT stateChanged();
    } else if (event == QLatin1String("partial")) {
        if (ev.value(QStringLiteral("dropped")).toBool()) {
            if (m_liveId == ev.value(QStringLiteral("id")).toString()) {
                m_liveText.clear();
                m_liveOriginal.clear();
                Q_EMIT subtitlesChanged();
            }
            return;
        }
        m_liveId = ev.value(QStringLiteral("id")).toString();
        m_liveOriginal = ev.value(QStringLiteral("original")).toString();
        const QString text = ev.value(QStringLiteral("text")).toString();
        m_liveText = text.isEmpty() ? m_liveOriginal : text;
        Q_EMIT subtitlesChanged();
    } else if (event == QLatin1String("draft")) {
        addLine(ev, true);
    } else if (event == QLatin1String("final")) {
        addLine(ev, false);
        m_latency.append(ev.value(QStringLiteral("latency")).toObject());
        while (m_latency.size() > 30)
            m_latency.removeFirst();
        if (ev.value(QStringLiteral("engine")).toString() != QLatin1String("none"))
            m_mtEngine = ev.value(QStringLiteral("engine")).toString();
        const QString lang = ev.value(QStringLiteral("lang")).toString();
        if (!lang.isEmpty() && lang != m_detected) {
            m_detected = lang;
            Q_EMIT stateChanged();
        }
        Q_EMIT latencyChanged();
    } else if (event == QLatin1String("stats")) {
        // the voice's timings for a finished utterance: merge into the last matching row
        const QJsonObject lat = ev.value(QStringLiteral("latency")).toObject();
        if (!m_latency.isEmpty()) {
            QJsonObject last = m_latency.last();
            for (auto it = lat.begin(); it != lat.end(); ++it)
                last.insert(it.key(), it.value());
            m_latency.last() = last;
        }
        Q_EMIT latencyChanged();
    } else if (event == QLatin1String("level")) {
        if (ev.value(QStringLiteral("stream")).toString() == QLatin1String("record")) {
            m_recordLevel = ev.value(QStringLiteral("peak")).toDouble();
            m_recordSeconds = ev.value(QStringLiteral("seconds")).toDouble();
            Q_EMIT recordLevelChanged();
        }
    } else if (event == QLatin1String("record.done")) {
        m_recording = false;
        m_recordResult = ev.toVariantMap();
        Q_EMIT recordChanged();
    } else if (event == QLatin1String("profile.ready")) {
        m_preparing = false;
        setStatus(tr("Voice profile ready"));
        Q_EMIT recordChanged();
        Q_EMIT voicesChanged();
        QTimer::singleShot(0, this, &LiveTranslator::prepareMissingOmniPrompts); // the next voice in use, if any
    } else if (event == QLatin1String("error")) {
        if (m_preparing) {
            m_preparing = false;
            Q_EMIT recordChanged();
        }
        fail(ev.value(QStringLiteral("message")).toString());
    }
}

void LiveTranslator::addLine(const QJsonObject &ev, bool draft)
{
    const QString id = ev.value(QStringLiteral("id")).toString();
    QVariantMap row{{QStringLiteral("id"), id},
                    {QStringLiteral("text"), ev.value(QStringLiteral("text")).toString()},
                    {QStringLiteral("original"), ev.value(QStringLiteral("original")).toString()},
                    {QStringLiteral("draft"), draft},
                    {QStringLiteral("stream"), ev.value(QStringLiteral("stream")).toString()},
                    {QStringLiteral("engine"), ev.value(QStringLiteral("engine")).toString()}};
    bool replaced = false;
    for (QVariant &v : m_lines) {
        if (v.toMap().value(QStringLiteral("id")).toString() == id) {
            v = row; // the quality translation replaces its draft in place
            replaced = true;
        }
    }
    if (!replaced)
        m_lines.append(row);
    while (m_lines.size() > 40)
        m_lines.removeFirst();
    if (m_liveId == id) {
        m_liveText.clear();
        m_liveOriginal.clear();
    }
    Q_EMIT subtitlesChanged();
}

QVariantList LiveTranslator::latency() const
{
    // per stage: the last value and the median of the last 30 utterances, in ms after the speech ended
    const QList<QPair<QString, QString>> stages = {{QStringLiteral("vadFinal"), tr("End of speech detected")},
                                                   {QStringLiteral("sttDone"), tr("Transcribed")},
                                                   {QStringLiteral("mtDraft"), tr("Instant translation")},
                                                   {QStringLiteral("mtFinal"), tr("Final translation")},
                                                   {QStringLiteral("ttsFirst"), tr("Voice synthesized")},
                                                   {QStringLiteral("playStart"), tr("Voice starts playing")}};
    QVariantList out;
    for (const auto &stage : stages) {
        QList<int> values;
        for (const QJsonObject &row : m_latency) {
            if (row.contains(stage.first))
                values << row.value(stage.first).toInt();
        }
        if (values.isEmpty())
            continue;
        const int last = values.last();
        std::sort(values.begin(), values.end());
        out << QVariantMap{{QStringLiteral("stage"), stage.second}, {QStringLiteral("last"), last},
                           {QStringLiteral("median"), values.at(values.size() / 2)}, {QStringLiteral("count"), values.size()}};
    }
    return out;
}

// ------------------------------------------------------------------ session

static QJsonObject piperOrEspeak(const QString &configured, const QString &lang)
{
    // a piper voice for the language being spoken; espeak-ng when there is none
    if (configured.startsWith(QLatin1String("piper:"))) {
        const QString path = configured.mid(6);
        if (QFileInfo::exists(path) && QFileInfo(path).fileName().startsWith(lang))
            return {{QStringLiteral("kind"), QStringLiteral("piper")}, {QStringLiteral("model"), path}};
    }
    QString def = lang == QLatin1String("pt") ? QStringLiteral("/usr/share/piper-voices/pt/pt_BR/faber/medium/pt_BR-faber-medium.onnx")
                                             : Paths::translateDir() + QStringLiteral("/piper/en_US-joe-medium.onnx");
    if (!QFileInfo::exists(def) && lang == QLatin1String("pt"))
        def = Paths::translateDir() + QStringLiteral("/piper/pt_BR-faber-medium.onnx");
    if (lang != QLatin1String("pt") && lang != QLatin1String("en"))
        return {{QStringLiteral("kind"), QStringLiteral("espeak")}, {QStringLiteral("espeak"), lang}};
    if (QFileInfo::exists(def))
        return {{QStringLiteral("kind"), QStringLiteral("piper")}, {QStringLiteral("model"), def}};
    return {{QStringLiteral("kind"), QStringLiteral("espeak")}, {QStringLiteral("espeak"), lang == QLatin1String("pt") ? QStringLiteral("pt-br") : QStringLiteral("en-us")}};
}

QJsonObject LiveTranslator::sessionConfig(QString *problem) const
{
    const QString models = Paths::modelsDir();
    // the mode's model (or a smaller one this session, see checkGpu); while it downloads,
    // the closest one on disk (start() says so)
    const QString sttName = m_sttThisTime.isEmpty() ? sttModel() : m_sttThisTime;
    QString stt = Paths::whisperModel(sttName);
    for (const QString &fallback : {QStringLiteral("large-v3-turbo-q5_0"), QStringLiteral("large-v3-turbo"), QStringLiteral("small-q5_1"),
                                    QStringLiteral("base"), QStringLiteral("small")}) {
        if (QFileInfo::exists(stt))
            break;
        // never a bigger model than the one chosen (it may not fit beside the rest)
        if (HwProfile::sttVramMb(fallback) > HwProfile::sttVramMb(sttName))
            continue;
        stt = Paths::whisperModel(fallback);
    }
    if (!QFileInfo::exists(stt)) {
        for (const QString &any : SttChoices) {
            if (QFileInfo::exists(Paths::whisperModel(any))) {
                stt = Paths::whisperModel(any);
                break;
            }
        }
    }
    const QString sttUsed = QFileInfo(stt).completeBaseName().mid(5); // ggml-<name>
    QJsonObject mt;
    // Opus-MT en→ROMANCE needs a target token; ROMANCE→en reads es/fr/it/pt alike
    const QString enRom = models + QStringLiteral("/mt-en-romance");
    const QString romEn = models + QStringLiteral("/mt-romance-en");
    const QHash<QString, QString> tokens = {{QStringLiteral("pt"), QStringLiteral(">>pt_br<<")}, {QStringLiteral("es"), QStringLiteral(">>es<<")},
                                            {QStringLiteral("fr"), QStringLiteral(">>fr<<")}, {QStringLiteral("it"), QStringLiteral(">>it<<")}};
    for (auto it = tokens.begin(); it != tokens.end(); ++it) {
        mt.insert(QStringLiteral("en-") + it.key(), QJsonObject{{QStringLiteral("path"), enRom}, {QStringLiteral("prefix"), it.value()}});
        mt.insert(it.key() + QStringLiteral("-en"), QJsonObject{{QStringLiteral("path"), romEn}});
    }
    // Japanese, Chinese and Korean → English (the engine chains them with en→pt/es/fr/it):
    // an instant draft for anime and dramas instead of waiting for the LLM
    for (const QString &l : {QStringLiteral("ja"), QStringLiteral("zh"), QStringLiteral("ko")}) {
        const QString dir = models + QStringLiteral("/mt-") + l + QStringLiteral("-en");
        if (QFileInfo::exists(dir + QStringLiteral("/model.bin")))
            mt.insert(l + QStringLiteral("-en"), QJsonObject{{QStringLiteral("path"), dir}});
    }
    QJsonObject paths{{QStringLiteral("vad"), models + QStringLiteral("/silero_vad.onnx")},
                      {QStringLiteral("whisperModel"), stt},
                      {QStringLiteral("mt"), mt}};
    // no room on the GPU after all: a model the processor keeps up with
    for (const QString &cpu : {QStringLiteral("base"), QStringLiteral("small-q5_1")}) {
        if (QFileInfo::exists(Paths::whisperModel(cpu))) {
            paths.insert(QStringLiteral("whisperCpuModel"), Paths::whisperModel(cpu));
            break;
        }
    }

    const QString m = mode();
    const QString from = fromLang();
    const QString to = toLang();
    const int endSilence = value(QStringLiteral("endSilenceMs"), 400).toInt();
    QJsonObject listen{{QStringLiteral("name"), QStringLiteral("in")},
                       {QStringLiteral("from"), from},
                       {QStringLiteral("to"), to},
                       {QStringLiteral("endSilenceMs"), endSilence}};
    QJsonObject src{{QStringLiteral("kind"), source()}};
    if (source() == QLatin1String("app"))
        src.insert(QStringLiteral("app"), sourceApp());
    if (source() == QLatin1String("mic") && !value(QStringLiteral("mic")).toString().isEmpty())
        src.insert(QStringLiteral("device"), value(QStringLiteral("mic")).toString());
    listen.insert(QStringLiteral("source"), src);

    // the voice that speaks for the user
    auto myVoice = [&](const QString &lang) -> QJsonObject {
        const QString v = voice();
        if (v.startsWith(QLatin1String("profile:")) && cloneEngine() == QLatin1String("omnivoice")) {
            // OmniVoice: Voice Studio's worker in Voice Studio's venv; the prompt is made from
            // voice.wav on first use and cached beside it (omnivoice-prompt.pt, docs/voices.md)
            const QString dir = profileDir(v.mid(8));
            if (!QFileInfo::exists(dir + QStringLiteral("/voice.wav"))) {
                if (problem)
                    *problem = tr("The voice profile has no recording. Record it again or pick another voice.");
                return {};
            }
            return {{QStringLiteral("kind"), QStringLiteral("clone")},
                    {QStringLiteral("engine"), QStringLiteral("omnivoice")},
                    {QStringLiteral("python"), Paths::omniVoicePython()},
                    {QStringLiteral("worker"), Paths::omniVoiceWorker()},
                    {QStringLiteral("wav"), dir + QStringLiteral("/voice.wav")},
                    {QStringLiteral("prompt"), dir + QStringLiteral("/omnivoice-prompt.pt")},
                    {QStringLiteral("quality"), voiceQuality()}};
        }
        if (v.startsWith(QLatin1String("profile:"))) {
            const QString conds = profileDir(v.mid(8)) + QStringLiteral("/conds-v3.pt");
            if (!QFileInfo::exists(conds)) {
                if (problem)
                    *problem = tr("The voice profile is not ready yet. Record it again or pick another voice.");
                return {};
            }
            if (!m_setup->voiceEngineReady()) {
                if (problem)
                    *problem = tr("The voice engine is not installed (Models page).");
                return {};
            }
            return {{QStringLiteral("kind"), QStringLiteral("clone")},
                    {QStringLiteral("python"), Paths::voiceVenvPython()},
                    {QStringLiteral("worker"), Paths::cloneWorker()},
                    {QStringLiteral("conds"), conds},
                    {QStringLiteral("model"), QStringLiteral("v3")},
                    {QStringLiteral("quality"), voiceQuality()}};
        }
        if (v == QLatin1String("espeak"))
            return {{QStringLiteral("kind"), QStringLiteral("espeak")}, {QStringLiteral("espeak"), lang == QLatin1String("pt") ? QStringLiteral("pt-br") : lang}};
        return piperOrEspeak(v, lang);
    };

    QJsonArray streams;
    QString mtMode = quality();
    // Listening dubbed (a video, or the other side of a call): the voice in my headphones
    const auto dub = [&](QJsonObject &s, const QJsonObject &voiceCfg) {
        s.insert(QStringLiteral("speak"), true);
        s.insert(QStringLiteral("duck"), duck());
        s.insert(QStringLiteral("voice"), voiceCfg);
        // Tuned on a real anime episode (README.md, "Dubbed latency"): the next line of a fast
        // exchange gets its own translation instead of waiting for the whole exchange, and the
        // voice takes the refined translation only when that costs no time
        s.insert(QStringLiteral("endSilenceMs"), qMin(endSilence, value(QStringLiteral("dubEndSilenceMs"), DubEndSilenceMs).toInt()));
        s.insert(QStringLiteral("splitAfterMs"), value(QStringLiteral("dubSplitAfterMs"), DubSplitAfterMs).toInt());
        s.insert(QStringLiteral("splitSilenceMs"), value(QStringLiteral("dubSplitSilenceMs"), DubSplitSilenceMs).toInt());
        s.insert(QStringLiteral("maxUtteranceMs"), value(QStringLiteral("dubMaxUtteranceMs"), DubMaxUtteranceMs).toInt());
        s.insert(QStringLiteral("voiceWaitMs"), value(QStringLiteral("dubVoiceWaitMs"), DubVoiceWaitMs).toInt());
        s.insert(QStringLiteral("voiceWaitLongMs"), value(QStringLiteral("dubVoiceWaitLongMs"), DubVoiceWaitLongMs).toInt());
    };
    if (m == QLatin1String("subtitles")) {
        streams << listen;
    } else if (m == QLatin1String("dubbed")) {
        // piper (or eSpeak) on the processor: never a cloned voice, so the quality translator
        // keeps the video memory (hardware recommendation, docs/voices.md)
        dub(listen, piperOrEspeak(voice(), to));
        // the dubbed voice also comes out of "Lingmo Tradutor" (OBS, recorders, calls)
        listen.insert(QStringLiteral("mirror"), QLatin1String(VirtualMic::SinkName));
        streams << listen;
    } else {
        // speak / call: the microphone → the virtual microphone
        const QString myLang = m == QLatin1String("call") ? to : (from == QLatin1String("auto") ? QStringLiteral("pt") : from);
        const QString theirLang = m == QLatin1String("call") ? (from == QLatin1String("auto") ? QStringLiteral("en") : from) : to;
        QJsonObject mic{{QStringLiteral("kind"), QStringLiteral("mic")}};
        if (!value(QStringLiteral("mic")).toString().isEmpty())
            mic.insert(QStringLiteral("device"), value(QStringLiteral("mic")).toString());
        const QJsonObject v = myVoice(theirLang);
        if (v.isEmpty())
            return {};
        QJsonObject out{{QStringLiteral("name"), QStringLiteral("out")}, {QStringLiteral("source"), mic},
                        {QStringLiteral("from"), myLang}, {QStringLiteral("to"), theirLang}, {QStringLiteral("speak"), true},
                        {QStringLiteral("voice"), v}, {QStringLiteral("output"), QLatin1String(VirtualMic::SinkName)},
                        // your own words: a short breath does not end the sentence, and a long
                        // sentence is translated whole (cut only past 15 s, at its quietest point)
                        {QStringLiteral("endSilenceMs"), qMax(endSilence, 500)},
                        {QStringLiteral("maxUtteranceMs"), 15000},
                        // a pause after "... e", "... porque" or with no final punctuation yet:
                        // you are thinking, the sentence goes on (up to 1.3 s of silence)
                        {QStringLiteral("holdEndMs"), 1300},
                        // long speech goes out in pieces at its short pauses, so the voice keeps
                        // talking while you go on (simultaneous, like an interpreter)
                        {QStringLiteral("splitAfterMs"), 2500},
                        {QStringLiteral("splitSilenceMs"), 300},
                        // fewer live transcripts while you talk: they share the GPU with your voice
                        {QStringLiteral("partialMs"), 1500},
                        {QStringLiteral("headphones"), headphones()}};
        // the LLM beside the cloned voice only when checkGpu() found room for both
        // (a GPU of 8 GB or more; on 6 GB the recommendation is "fast" there)
        if (v.value(QStringLiteral("kind")).toString() == QLatin1String("clone") && !m_cloneLlm)
            mtMode = QStringLiteral("fast");
        out.insert(QStringLiteral("mt"), mtMode);
        streams << out;
        if (m == QLatin1String("call")) {
            // the other side: whatever plays (the call app) → subtitles in my language
            listen.insert(QStringLiteral("from"), from);
            listen.insert(QStringLiteral("to"), myLang);
            if (source() == QLatin1String("mic"))
                listen.insert(QStringLiteral("source"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("system")}});
            listen.insert(QStringLiteral("mt"), mtMode);
            // hear them dubbed too (piper on the CPU: it fits beside the cloned voice),
            // unless only subtitles are wanted
            if (callDubbed())
                dub(listen, piperOrEspeak(QString(), myLang));
            streams << listen;
        }
    }
    for (int i = 0; i < streams.size(); ++i) {
        QJsonObject s = streams.at(i).toObject();
        if (!s.contains(QStringLiteral("mt")))
            s.insert(QStringLiteral("mt"), mtMode);
        streams[i] = s;
    }
    return {{QStringLiteral("cmd"), QStringLiteral("start")},
            {QStringLiteral("mode"), m},
            {QStringLiteral("mt"), mtMode},
            {QStringLiteral("llmModel"), value(QStringLiteral("llmModel"), QStringLiteral("qwen3.5:4b")).toString()},
            // the GPU when the model fits in the free video memory (engine.py), else the processor
            {QStringLiteral("stt"), QJsonObject{{QStringLiteral("gpu"), QStringLiteral("auto")}, {QStringLiteral("threads"), 6},
                                                {QStringLiteral("model"), sttUsed},
                                                {QStringLiteral("minFreeMb"), HwProfile::sttVramMb(sttUsed) + SttMarginMb}}},
            {QStringLiteral("paths"), paths},
            {QStringLiteral("streams"), streams}};
}

void LiveTranslator::start()
{
    if (m_running || m_starting)
        return;
    m_error.clear();
    m_notice.clear();
    m_gpuCrowded = false;
    m_forceFast = false;
    m_cloneLlm = false;
    m_sttThisTime.clear();
    m_settings.sync(); // Settings or Voice Studio may have changed it
    reloadHardware(true);
    readVoiceConf();
    if (!m_setup->ready()) {
        fail(tr("Download the translation models first (about 650 MB, once)."));
        Q_EMIT showRequested();
        return;
    }
    const QString from = fromLang();
    if ((from == QLatin1String("ja") || from == QLatin1String("zh") || from == QLatin1String("ko")) && !m_setup->has(QStringLiteral("mt-") + from))
        m_notice = tr("For a faster dubbing from %1, download its instant translator in Models (80 MB).").arg(languageTitle(from));
    if (cloneEngineFallback() && (mode() == QLatin1String("speak") || mode() == QLatin1String("call")) && voice().startsWith(QLatin1String("profile:")))
        m_notice = tr("OmniVoice is chosen in Settings but is not turned on in Voice Studio: your voice uses Chatterbox.");
    // the mode's speech model is not downloaded yet: fetched now, the closest one meanwhile
    const QString stt = sttModel();
    if (!QFileInfo::exists(Paths::whisperModel(stt))) {
        if (!m_setup->busy())
            m_setup->install({QStringLiteral("whisper:") + stt});
        m_notice = tr("Downloading the speech model %1 for this mode; a model already on this computer is used meanwhile.").arg(stt);
    }
    m_starting = true;
    setStatus(tr("Checking the video memory…"));
    checkGpu();
}

void LiveTranslator::checkGpu()
{
    // nvidia-smi for the free memory, Ollama for what it holds: decide before loading anything
    auto *smi = new QProcess(this);
    connect(smi, &QProcess::finished, this, [this, smi](int code) {
        smi->deleteLater();
        const QString out = QString::fromUtf8(smi->readAllStandardOutput()).trimmed();
        m_vramFree = code == 0 && !out.isEmpty() ? out.split(QLatin1Char('\n')).first().toInt() : -1;
        QNetworkRequest req(ollamaUrl(QStringLiteral("/api/ps")));
        req.setTransferTimeout(2000);
        QNetworkReply *ps = m_net->get(req);
        connect(ps, &QNetworkReply::finished, this, [this, ps] {
            ps->deleteLater();
            const QJsonArray loaded = QJsonDocument::fromJson(ps->readAll()).object().value(QStringLiteral("models")).toArray();
            const QString llm = value(QStringLiteral("llmModel"), QStringLiteral("qwen3.5:4b")).toString();
            bool llmLoaded = false;
            for (const QJsonValue &v : loaded)
                llmLoaded |= v.toObject().value(QStringLiteral("name")).toString() == llm;
            if (!m_starting) {
                Q_EMIT stateChanged();
                return;
            }
            const bool mic = mode() == QLatin1String("speak") || mode() == QLatin1String("call");
            const bool clone = mic && voice().startsWith(QLatin1String("profile:"));
            const bool omni = clone && cloneEngine() == QLatin1String("omnivoice");
            const int voiceMb = omni ? OmniMb : CloneMb;
            int sttMb = HwProfile::sttVramMb(sttModel()) + SttMarginMb;
            if (m_vramFree >= 0) {
                // the cloned voice needs the room: a smaller whisper this session (e.g. Chatterbox
                // beside large-v3-turbo on 6 GB), when it is on disk
                const QString smaller = QStringLiteral("large-v3-turbo-q5_0");
                if (clone && m_vramFree < sttMb + voiceMb && sttModel() == QLatin1String("large-v3-turbo")
                    && QFileInfo::exists(Paths::whisperModel(smaller)) && m_vramFree >= HwProfile::sttVramMb(smaller) + SttMarginMb + voiceMb) {
                    m_sttThisTime = smaller;
                    sttMb = HwProfile::sttVramMb(smaller) + SttMarginMb;
                    m_notice = tr("Little free video memory beside your voice: speech recognition uses the compressed model this time.");
                }
                // the quality translator beside the cloned voice when both fit (8 GB and more)
                m_cloneLlm = clone && quality() == QLatin1String("quality") && m_vramFree >= sttMb + voiceMb + (llmLoaded ? 0 : LlmMb);
                if (clone && quality() == QLatin1String("quality") && !m_cloneLlm)
                    m_notice = tr("Not enough video memory for the quality translator beside your voice: using the fast one this time.");
                if (clone && m_vramFree < sttMb + voiceMb) {
                    // the user's own models: never unloaded without asking
                    m_starting = false;
                    m_gpuCrowded = true;
                    const QString need = omni ? tr("about 3 GB") : tr("about 4 GB");
                    fail(tr("Your voice needs %1 of video memory and only %2 MB are free. \"Free the GPU\" unloads the AI models (Ollama, Voice Studio), or use a piper voice.").arg(need).arg(m_vramFree));
                    return;
                }
                if (!clone && quality() == QLatin1String("quality") && !llmLoaded && m_vramFree < sttMb + LlmMb) {
                    m_notice = tr("Not enough video memory for the quality translator: using the fast one this time.");
                    m_forceFast = true;
                }
            }
            reallyStart();
        });
    });
    connect(smi, &QProcess::errorOccurred, this, [this, smi](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        smi->deleteLater();
        m_vramFree = -1; // no NVIDIA tools: the engine decides (CPU)
        if (m_starting)
            reallyStart();
    });
    smi->start(QStringLiteral("nvidia-smi"), {QStringLiteral("--query-gpu=memory.free"), QStringLiteral("--format=csv,noheader,nounits")});
}

void LiveTranslator::reallyStart()
{
    QString problem;
    QJsonObject cfg = sessionConfig(&problem);
    if (cfg.isEmpty()) {
        m_starting = false;
        fail(problem);
        return;
    }
    if (m_forceFast) {
        cfg.insert(QStringLiteral("mt"), QStringLiteral("fast"));
        QJsonArray streams = cfg.value(QStringLiteral("streams")).toArray();
        for (int i = 0; i < streams.size(); ++i) {
            QJsonObject s = streams.at(i).toObject();
            s.insert(QStringLiteral("mt"), QStringLiteral("fast"));
            streams[i] = s;
        }
        cfg.insert(QStringLiteral("streams"), streams);
    }
    if (mode() != QLatin1String("subtitles")) {
        QString err;
        if (!m_vmic->create(&err)) {
            m_starting = false;
            fail(err);
            return;
        }
    }
    m_lines.clear();
    m_latency.clear();
    m_liveText.clear();
    m_liveOriginal.clear();
    m_detected.clear();
    Q_EMIT subtitlesChanged();
    Q_EMIT latencyChanged();
    setStatus(tr("Starting…"));
    ensureEngine([this, cfg] { send(cfg); });
}

void LiveTranslator::stop()
{
    if (m_engine)
        send({{QStringLiteral("cmd"), QStringLiteral("stop")}});
    m_running = m_starting = m_paused = false;
    m_vmic->remove();
    setStatus(QString());
}

void LiveTranslator::toggle()
{
    if (m_running || m_starting)
        stop();
    else
        start();
}

void LiveTranslator::setPaused(bool on)
{
    if (!m_running)
        return;
    send({{QStringLiteral("cmd"), on ? QStringLiteral("pause") : QStringLiteral("resume")}});
}

void LiveTranslator::freeGpu()
{
    // Voice Studio's engine (the Reader's "Ouvir", Voice Studio) may hold Chatterbox or
    // OmniVoice: ask it to unload them, as the user asked to free the GPU
    auto *voice = new QLocalSocket(this);
    connect(voice, &QLocalSocket::connected, voice, [voice] {
        voice->write(QJsonDocument(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("engine.unload")}, {QStringLiteral("id"), 1}}).toJson(QJsonDocument::Compact) + '\n');
        voice->flush();
        QTimer::singleShot(2000, voice, &QLocalSocket::disconnectFromServer);
    });
    connect(voice, &QLocalSocket::disconnected, voice, &QObject::deleteLater);
    connect(voice, &QLocalSocket::errorOccurred, voice, &QObject::deleteLater);
    voice->connectToServer(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/lingmo-voice/engine.sock"));

    QNetworkRequest req(ollamaUrl(QStringLiteral("/api/ps")));
    req.setTransferTimeout(5000);
    QNetworkReply *list = m_net->get(req);
    connect(list, &QNetworkReply::finished, this, [this, list] {
        list->deleteLater();
        const QJsonArray models = QJsonDocument::fromJson(list->readAll()).object().value(QStringLiteral("models")).toArray();
        auto pending = std::make_shared<int>(models.size());
        auto done = [this] {
            m_gpuCrowded = false;
            m_error.clear();
            // the driver needs a moment to hand the memory back
            QTimer::singleShot(1500, this, &LiveTranslator::start);
            Q_EMIT stateChanged();
        };
        if (models.isEmpty()) {
            done();
            return;
        }
        for (const QJsonValue &m : models) {
            QNetworkRequest unload(ollamaUrl(QStringLiteral("/api/generate")));
            unload.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            unload.setTransferTimeout(10000);
            QNetworkReply *r = m_net->post(unload, QJsonDocument(QJsonObject{{QStringLiteral("model"), m.toObject().value(QStringLiteral("name")).toString()},
                                                                             {QStringLiteral("keep_alive"), 0}}).toJson(QJsonDocument::Compact));
            connect(r, &QNetworkReply::finished, this, [r, pending, done] {
                r->deleteLater();
                if (--*pending == 0)
                    done();
            });
        }
    });
}

void LiveTranslator::clearError()
{
    m_error.clear();
    Q_EMIT stateChanged();
}

void LiveTranslator::setStatus(const QString &s)
{
    m_status = s;
    Q_EMIT stateChanged();
}

void LiveTranslator::fail(const QString &message)
{
    m_error = message;
    qWarning().noquote() << "lingmo-translate:" << message;
    // started from the shortcut with no window: show why instead of quitting silently
    if (m_starting || !m_running)
        Q_EMIT showRequested();
    Q_EMIT stateChanged();
}

// ------------------------------------------------------------------ voice profiles

QString LiveTranslator::sanitizeName(const QString &name)
{
    QString n = name.simplified();
    n.remove(QRegularExpression(QStringLiteral("[/\\\\\\x00-\\x1f]")));
    n.replace(QLatin1String(".."), QLatin1String("."));
    if (n.startsWith(QLatin1Char('.')))
        n.remove(0, 1);
    return n.left(40);
}

QString LiveTranslator::profileDir(const QString &name) const
{
    return Paths::voicesDir() + QLatin1Char('/') + sanitizeName(name);
}

QVariantList LiveTranslator::profiles() const
{
    QVariantList out;
    QDir dir(Paths::voicesDir());
    for (const QFileInfo &fi : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time)) {
        if (fi.isSymLink() || !QFileInfo::exists(fi.filePath() + QStringLiteral("/voice.wav")))
            continue;
        QFile f(fi.filePath() + QStringLiteral("/profile.json"));
        QVariantMap meta;
        if (f.open(QIODevice::ReadOnly))
            meta = QJsonDocument::fromJson(f.readAll()).object().toVariantMap();
        meta.insert(QStringLiteral("name"), fi.fileName());
        // OmniVoice makes its prompt from the recording on first use: the recording is enough
        meta.insert(QStringLiteral("ready"), QFileInfo::exists(fi.filePath() + (cloneEngine() == QLatin1String("omnivoice") ? QStringLiteral("/voice.wav")
                                                                                                                      : QStringLiteral("/conds-v3.pt"))));
        out << meta;
    }
    return out;
}

QString LiveTranslator::readingText() const
{
    // ~25 s read aloud: varied sounds of Brazilian Portuguese, questions, numbers and a calm pace
    return tr("Olá! Este é o meu perfil de voz para a tradução ao vivo. Eu vou ler este texto com calma, "
              "no meu jeito normal de falar, como numa reunião. Hoje é segunda-feira, são nove e meia da manhã, "
              "e o café ainda está quente. Você consegue me ouvir bem? Precisamos revisar o relatório, "
              "conferir os números do trimestre e marcar a próxima conversa para quinta-feira à tarde. "
              "Obrigado pela atenção e até já!");
}

void LiveTranslator::startRecording(const QString &name, const QString &mic)
{
    const QString clean = sanitizeName(name);
    if (clean.isEmpty() || m_recording)
        return;
    const QString dir = profileDir(clean);
    QDir().mkpath(Paths::voicesDir());
    QFile::setPermissions(Paths::voicesDir(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QDir().mkpath(dir);
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    m_recordName = clean;
    m_recordResult.clear();
    m_recording = true;
    m_recordLevel = 0;
    m_recordSeconds = 0;
    Q_EMIT recordChanged();
    ensureEngine([this, dir, mic] {
        QJsonObject cmd{{QStringLiteral("cmd"), QStringLiteral("record.start")},
                        {QStringLiteral("out"), dir + QStringLiteral("/voice.wav")},
                        {QStringLiteral("seconds"), 30}};
        if (!mic.isEmpty())
            cmd.insert(QStringLiteral("source"), mic);
        send(cmd);
    });
}

void LiveTranslator::stopRecording()
{
    send({{QStringLiteral("cmd"), QStringLiteral("record.stop")}});
}

void LiveTranslator::saveProfile(const QString &name)
{
    const QString dir = profileDir(name);
    if (!QFileInfo::exists(dir + QStringLiteral("/voice.wav")))
        return;
    QSaveFile f(dir + QStringLiteral("/profile.json"));
    if (f.open(QIODevice::WriteOnly)) {
        QJsonObject meta{{QStringLiteral("name"), sanitizeName(name)},
                         {QStringLiteral("created"), QDateTime::currentDateTime().toString(Qt::ISODate)},
                         {QStringLiteral("seconds"), m_recordResult.value(QStringLiteral("seconds")).toDouble()},
                         {QStringLiteral("snrDb"), m_recordResult.value(QStringLiteral("snrDb")).toDouble()},
                         {QStringLiteral("model"), QStringLiteral("chatterbox-mtl-v3")},
                         {QStringLiteral("ownVoice"), true}};
        f.write(QJsonDocument(meta).toJson());
        f.commit();
        QFile::setPermissions(dir + QStringLiteral("/profile.json"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    QFile::remove(dir + QStringLiteral("/conds-v3.pt")); // re-recorded: the old conditionals are stale
    QFile::remove(dir + QStringLiteral("/omnivoice-prompt.pt")); // and so is OmniVoice's prompt
    Q_EMIT voicesChanged();
    if (!m_setup->voiceEngineReady()) {
        fail(tr("The recording is saved. Install the voice engine (Models page) to finish the profile."));
        return;
    }
    m_preparing = true;
    Q_EMIT recordChanged();
    if (cloneEngine() == QLatin1String("omnivoice")) {
        // the OmniVoice prompt now, not on the first translation (~40 s on the processor)
        QFile::remove(dir + QStringLiteral("/omnivoice-prompt.pt"));
        prepareOmniPrompt(dir);
        return;
    }
    ensureEngine([this, dir] {
        send({{QStringLiteral("cmd"), QStringLiteral("profile.prepare")},
              {QStringLiteral("wav"), dir + QStringLiteral("/voice.wav")},
              {QStringLiteral("out"), dir + QStringLiteral("/conds-v3.pt")},
              {QStringLiteral("python"), Paths::voiceVenvPython()},
              {QStringLiteral("worker"), Paths::cloneWorker()},
              {QStringLiteral("model"), QStringLiteral("v3")}});
    });
}

void LiveTranslator::prepareOmniPrompt(const QString &dir)
{
    const QString stt = Paths::whisperModel(value(QStringLiteral("sttModel"), QStringLiteral("small-q5_1")).toString());
    ensureEngine([this, dir, stt] {
        send({{QStringLiteral("cmd"), QStringLiteral("profile.prepare")},
              {QStringLiteral("engine"), QStringLiteral("omnivoice")},
              {QStringLiteral("wav"), dir + QStringLiteral("/voice.wav")},
              {QStringLiteral("out"), dir + QStringLiteral("/omnivoice-prompt.pt")},
              {QStringLiteral("python"), Paths::omniVoicePython()},
              {QStringLiteral("worker"), Paths::omniVoiceWorker()},
              {QStringLiteral("whisperModel"), stt}});
    });
}

void LiveTranslator::prepareMissingOmniPrompts()
{
    // OmniVoice just became the engine (Settings): get the voices in use ready in the
    // background, one at a time, while nothing is translating
    if (!m_omniReady || m_running || m_starting || m_preparing)
        return;
    for (const QString &key : {QStringLiteral("voice"), QStringLiteral("dubbed/voice")}) {
        const QString v = value(key).toString();
        if (!v.startsWith(QLatin1String("profile:")))
            continue;
        const QString dir = profileDir(v.mid(8));
        if (QFileInfo::exists(dir + QStringLiteral("/voice.wav")) && !QFileInfo::exists(dir + QStringLiteral("/omnivoice-prompt.pt"))) {
            m_preparing = true;
            Q_EMIT recordChanged();
            prepareOmniPrompt(dir);
            return; // the next one after profile.ready
        }
    }
}

void LiveTranslator::discardRecording(const QString &name)
{
    const QString dir = profileDir(name);
    if (!QFileInfo::exists(dir + QStringLiteral("/profile.json")))
        QDir(dir).removeRecursively(); // a recording never saved
    m_recordResult.clear();
    Q_EMIT recordChanged();
}

bool LiveTranslator::importProfile(const QString &name, const QString &file, bool confirmedOwnVoice)
{
    // Cloning someone else's voice is not offered: an imported recording needs the explicit confirmation
    if (!confirmedOwnVoice)
        return false;
    const QString clean = sanitizeName(name);
    QString src = file.startsWith(QLatin1String("file://")) ? QUrl(file).toLocalFile() : file;
    if (clean.isEmpty() || !QFileInfo(src).isFile())
        return false;
    const QString dir = profileDir(clean);
    QDir().mkpath(dir);
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    // any format ffmpeg reads → 16 kHz mono wav, at most 30 s
    QProcess ff;
    ff.start(QStringLiteral("ffmpeg"), {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-y"),
                                        QStringLiteral("-i"), src, QStringLiteral("-t"), QStringLiteral("30"), QStringLiteral("-ac"), QStringLiteral("1"),
                                        QStringLiteral("-ar"), QStringLiteral("16000"), dir + QStringLiteral("/voice.wav")});
    if (!ff.waitForFinished(60000) || ff.exitCode() != 0) {
        QDir(dir).removeRecursively();
        return false;
    }
    m_recordResult = {{QStringLiteral("seconds"), 0}, {QStringLiteral("imported"), true}};
    saveProfile(clean);
    return true;
}

bool LiveTranslator::deleteProfile(const QString &name)
{
    const QString clean = sanitizeName(name);
    if (clean.isEmpty())
        return false;
    const QFileInfo fi(profileDir(clean));
    if (!fi.exists() || fi.isSymLink() || fi.absolutePath() != QFileInfo(Paths::voicesDir()).absoluteFilePath())
        return false;
    const bool ok = QDir(fi.absoluteFilePath()).removeRecursively();
    if (voice() == QStringLiteral("profile:") + clean)
        setVoice(QStringLiteral("piper:/usr/share/piper-voices/pt/pt_BR/faber/medium/pt_BR-faber-medium.onnx"));
    Q_EMIT voicesChanged();
    return ok;
}

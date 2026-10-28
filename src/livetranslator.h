/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QProcess>
#include <QSettings>
#include <QVariantMap>
#include <QVariantList>
#include <functional>

#include "hwprofile.h"

class QFileSystemWatcher;
class QNetworkAccessManager;
class Setup;
class VirtualMic;

// Live translation: drives the engine (data/engine/engine.py, one long-lived
// process in its own virtualenv) over JSON lines, and owns everything that
// touches the session: the virtual microphone, the Ollama memory guard, the
// settings shared with Settings › IA.
//
// Modes:
//   subtitles  STT whisper on the GPU + Opus-MT draft on the CPU
//              + qwen3.5 4B refinement on the GPU (3.7 GB) when "quality"
//   dubbed     the same + piper on the CPU (never a cloned voice, so the LLM
//              fits beside it), original audio lowered
//   speak      mic → STT on the GPU → Opus-MT on the CPU → the cloned voice
//              (OmniVoice ~2.6 GB, or Chatterbox ~3.4 GB, on the GPU) or piper → virtual mic.
//              The LLM loads beside the cloned voice only when the free video
//              memory has room for it (checkGpu).
//   call       both at once: the other side → subtitles; you → virtual mic
//
// What each mode uses by default (whisper model, "quality"/"fast", the voice)
// is the hardware recommendation (hwprofile.h, ~/.config/lingmoos/hardware.conf,
// docs/voices.md "Perfil de hardware"); a value the user sets is kept per mode
// ([Translate] <mode>/sttModel, <mode>/quality) and wins over it. On a 6 GB GPU:
//   subtitles, dubbed  large-v3-turbo-q5_0 + Opus-MT + qwen, piper   (~4.6 GB)
//   speak, call        large-v3-turbo + Opus-MT only + cloned voice  (~4.5 GB)
//
// Languages are kept per mode ([Translate] <mode>/from, <mode>/to): listening
// modes translate from → to; "speak" is my language → theirs; "call" keeps
// from = the language they speak and to = the one I speak.
//
// The engine of the cloned voice is a system-wide choice made in Settings
// (~/.config/lingmoos/voice.conf, [Voice] cloneEngine, docs/voices.md); not
// chosen there, the hardware recommendation's engine (when it is ready).
void restoreDucking();

class LiveTranslator : public QObject
{
    Q_OBJECT
    // settings (written to translate.conf, shared with Settings › IA)
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY settingsChanged)
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY settingsChanged)
    Q_PROPERTY(QString sourceApp READ sourceApp WRITE setSourceApp NOTIFY settingsChanged)
    Q_PROPERTY(QString fromLang READ fromLang WRITE setFromLang NOTIFY settingsChanged)
    Q_PROPERTY(QString toLang READ toLang WRITE setToLang NOTIFY settingsChanged)
    Q_PROPERTY(QString quality READ quality WRITE setQuality NOTIFY settingsChanged)
    // the current mode's speech model ("large-v3-turbo-q5_0"), and the ones to choose from:
    // [{id, title, size, installed, recommended}]
    Q_PROPERTY(QString sttModel READ sttModel WRITE setSttModel NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList sttModels READ sttModels NOTIFY settingsChanged)
    // the hardware recommendation for the current mode: {sttModel, quality, voice
    // ("none"|"piper"|"clone"), cloneAllowed, llmBesideClone, vramMb, tier, gpuName,
    // vramTotalMb, cuda, cloneEngine}
    Q_PROPERTY(QVariantMap recommended READ recommended NOTIFY recommendationChanged)
    // the voice id marked "(recommended)" in the current mode ("" = none)
    Q_PROPERTY(QString recommendedVoice READ recommendedVoice NOTIFY recommendationChanged)
    // the current mode has nothing set by the user: it follows the recommendation
    Q_PROPERTY(bool followsRecommendation READ followsRecommendation NOTIFY recommendationChanged)
    Q_PROPERTY(QString voice READ voice WRITE setVoice NOTIFY settingsChanged)
    Q_PROPERTY(QString voiceQuality READ voiceQuality WRITE setVoiceQuality NOTIFY settingsChanged)
    Q_PROPERTY(double duck READ duck WRITE setDuck NOTIFY settingsChanged)
    Q_PROPERTY(bool callDubbed READ callDubbed WRITE setCallDubbed NOTIFY settingsChanged)
    Q_PROPERTY(bool showOriginal READ showOriginal WRITE setShowOriginal NOTIFY settingsChanged)
    Q_PROPERTY(double overlayFontScale READ overlayFontScale WRITE setOverlayFontScale NOTIFY settingsChanged)
    Q_PROPERTY(bool debugPanel READ debugPanel WRITE setDebugPanel NOTIFY settingsChanged)
    Q_PROPERTY(QPoint overlayPos READ overlayPos WRITE setOverlayPos NOTIFY settingsChanged)
    // the cloned voice's engine, from Settings: "chatterbox" | "omnivoice" (what will be used)
    Q_PROPERTY(QString cloneEngine READ cloneEngine NOTIFY cloneEngineChanged)
    // OmniVoice picked in Settings but not usable (not accepted/installed in Voice Studio)
    Q_PROPERTY(bool cloneEngineFallback READ cloneEngineFallback NOTIFY cloneEngineChanged)
    // state
    Q_PROPERTY(bool running READ running NOTIFY stateChanged)
    Q_PROPERTY(bool starting READ starting NOTIFY stateChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY stateChanged)
    Q_PROPERTY(QString detectedLang READ detectedLang NOTIFY stateChanged)
    Q_PROPERTY(QString pairLabel READ pairLabel NOTIFY stateChanged)
    // the source language, for the Island card: "Japanese (fixed)" / "Detected: Japanese"
    Q_PROPERTY(QString sourceDetail READ sourceDetail NOTIFY stateChanged)
    Q_PROPERTY(QString fromShort READ fromShort NOTIFY stateChanged)
    Q_PROPERTY(QStringList sourceLanguages READ sourceLanguages CONSTANT)
    Q_PROPERTY(bool sttOnGpu READ sttOnGpu NOTIFY stateChanged)
    Q_PROPERTY(QString mtEngine READ mtEngine NOTIFY stateChanged)
    Q_PROPERTY(bool virtualMicActive READ virtualMicActive NOTIFY stateChanged)
    Q_PROPERTY(bool headphones READ headphones NOTIFY sourcesChanged)
    Q_PROPERTY(int vramFreeMb READ vramFreeMb NOTIFY stateChanged)
    Q_PROPERTY(bool gpuCrowded READ gpuCrowded NOTIFY stateChanged)
    // subtitles: the line being spoken and the last finished ones
    Q_PROPERTY(QString liveText READ liveText NOTIFY subtitlesChanged)
    Q_PROPERTY(QString liveOriginal READ liveOriginal NOTIFY subtitlesChanged)
    Q_PROPERTY(QVariantList lines READ lines NOTIFY subtitlesChanged)
    Q_PROPERTY(QVariantList latency READ latency NOTIFY latencyChanged)
    // sources
    Q_PROPERTY(QVariantList apps READ apps NOTIFY sourcesChanged)
    Q_PROPERTY(QVariantList mics READ mics NOTIFY sourcesChanged)
    Q_PROPERTY(QVariantList voices READ voices NOTIFY voicesChanged)
    // voice profiles and recording
    Q_PROPERTY(QVariantList profiles READ profiles NOTIFY voicesChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY recordChanged)
    Q_PROPERTY(double recordLevel READ recordLevel NOTIFY recordLevelChanged)
    Q_PROPERTY(double recordSeconds READ recordSeconds NOTIFY recordLevelChanged)
    Q_PROPERTY(QVariantMap recordResult READ recordResult NOTIFY recordChanged)
    Q_PROPERTY(bool preparing READ preparing NOTIFY recordChanged)

public:
    LiveTranslator(Setup *setup, QObject *parent = nullptr);
    ~LiveTranslator() override;

    QString mode() const { return value(QStringLiteral("mode"), QStringLiteral("subtitles")).toString(); }
    void setMode(const QString &v);
    // per mode: subtitles may come from the microphone while dubbing listens to a video
    // (one shared key once left dubbing listening to the microphone)
    QString source() const { return value(mode() + QStringLiteral("/source"), QStringLiteral("system")).toString(); }
    void setSource(const QString &v) { setValue(mode() + QStringLiteral("/source"), v); }
    QString sourceApp() const { return value(QStringLiteral("sourceApp")).toString(); }
    void setSourceApp(const QString &v) { setValue(QStringLiteral("sourceApp"), v); }
    // the current mode's pair
    QString fromLang() const { return modeFrom(mode()); }
    void setFromLang(const QString &v);
    QString toLang() const { return modeTo(mode()); }
    void setToLang(const QString &v);
    QString modeFrom(const QString &mode) const;
    QString modeTo(const QString &mode) const;
    // per mode; not set, the hardware recommendation's
    QString quality() const { return modeQuality(mode()); }
    void setQuality(const QString &v) { setValue(mode() + QStringLiteral("/quality"), v); }
    QString modeQuality(const QString &mode) const;
    QString sttModel() const { return modeSttModel(mode()); }
    void setSttModel(const QString &v);
    QString modeSttModel(const QString &mode) const;
    QVariantList sttModels() const;
    QVariantMap recommended() const;
    QString recommendedVoice() const;
    bool followsRecommendation() const;
    // Forgets what the user set in the current mode (model, translation, voice)
    Q_INVOKABLE void useRecommended();
    const HwProfile::Profile &hardware() const { return m_hw; }
    QString voice() const;
    // dubbed listening has its own voice (piper only: a cloned voice and the LLM don't fit
    // together); speaking and calls share the user's own voice
    QString voiceKey() const { return mode() == QLatin1String("dubbed") ? QStringLiteral("dubbed/voice") : QStringLiteral("voice"); }
    void setVoice(const QString &v);
    // "fast" by default: the wait after you stop matters more than the last bit of polish
    // (OmniVoice 16 steps: the first words ~0.9 s after the translation instead of ~1.7 s)
    QString voiceQuality() const { return value(QStringLiteral("voiceQuality"), QStringLiteral("fast")).toString(); }
    void setVoiceQuality(const QString &v) { setValue(QStringLiteral("voiceQuality"), v); }
    double duck() const { return value(QStringLiteral("duck"), 0.3).toDouble(); }
    void setDuck(double v) { setValue(QStringLiteral("duck"), qBound(0.1, v, 1.0)); }
    // call: hear the other side dubbed (else subtitles only)
    bool callDubbed() const { return value(QStringLiteral("callDubbed"), true).toBool(); }
    void setCallDubbed(bool v) { setValue(QStringLiteral("callDubbed"), v); }
    bool showOriginal() const { return value(QStringLiteral("showOriginal"), true).toBool(); }
    void setShowOriginal(bool v) { setValue(QStringLiteral("showOriginal"), v); }
    double overlayFontScale() const { return value(QStringLiteral("overlayFontScale"), 1.0).toDouble(); }
    void setOverlayFontScale(double v) { setValue(QStringLiteral("overlayFontScale"), qBound(0.7, v, 2.0)); }
    bool debugPanel() const { return value(QStringLiteral("debugPanel"), false).toBool(); }
    void setDebugPanel(bool v) { setValue(QStringLiteral("debugPanel"), v); }
    QPoint overlayPos() const { return value(QStringLiteral("overlayPos"), QPoint(-1, -1)).toPoint(); }
    void setOverlayPos(const QPoint &p) { setValue(QStringLiteral("overlayPos"), p); }

    bool running() const { return m_running; }
    bool starting() const { return m_starting; }
    bool paused() const { return m_paused; }
    QString status() const { return m_status; }
    QString error() const { return m_error; }
    QString notice() const { return m_notice; }
    QString detectedLang() const { return m_detected; }
    QString pairLabel() const;
    QString sourceDetail() const;
    QString fromShort() const;
    QStringList sourceLanguages() const;
    Q_INVOKABLE QString languageTitle(const QString &code) const;
    bool sttOnGpu() const { return m_sttGpu; }
    QString mtEngine() const { return m_mtEngine; }
    bool virtualMicActive() const;
    bool headphones() const { return m_headphones; }
    int vramFreeMb() const { return m_vramFree; }
    bool gpuCrowded() const { return m_gpuCrowded; }
    QString liveText() const { return m_liveText; }
    QString liveOriginal() const { return m_liveOriginal; }
    QVariantList lines() const { return m_lines; }
    QVariantList latency() const;
    QVariantList apps() const { return m_apps; }
    QVariantList mics() const { return m_mics; }
    QVariantList voices() const;
    QVariantList profiles() const;
    bool recording() const { return m_recording; }
    double recordLevel() const { return m_recordLevel; }
    double recordSeconds() const { return m_recordSeconds; }
    QVariantMap recordResult() const { return m_recordResult; }
    bool preparing() const { return m_preparing; }
    QString cloneEngine() const;
    bool cloneEngineFallback() const;
    // OmniVoice accepted and installed in Voice Studio (its config, venv and weights)
    static bool omniVoiceReady();

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void toggle();
    Q_INVOKABLE void setPaused(bool on);
    Q_INVOKABLE void refreshSources();
    Q_INVOKABLE void clearError();
    // VRAM guard: unloads every model Ollama holds (each with keep_alive 0), then checks again
    Q_INVOKABLE void freeGpu();
    Q_INVOKABLE void checkGpu();
    // voice profiles
    Q_INVOKABLE void startRecording(const QString &name, const QString &mic);
    Q_INVOKABLE void stopRecording();
    Q_INVOKABLE void saveProfile(const QString &name);       // after a recording: computes and caches the conditionals
    void prepareOmniPrompt(const QString &dir);
    void prepareMissingOmniPrompts();
    Q_INVOKABLE void discardRecording(const QString &name);
    // Imported audio only with the explicit "this is my own voice" confirmation
    Q_INVOKABLE bool importProfile(const QString &name, const QString &file, bool confirmedOwnVoice);
    Q_INVOKABLE bool deleteProfile(const QString &name);
    Q_INVOKABLE QString readingText() const;
    Q_INVOKABLE static QString sanitizeName(const QString &name);

Q_SIGNALS:
    void settingsChanged();
    void stateChanged();
    void subtitlesChanged();
    void latencyChanged();
    void sourcesChanged();
    void voicesChanged();
    void recordChanged();
    void recordLevelChanged();
    void showRequested();
    void cloneEngineChanged();
    void recommendationChanged();

private:
    QVariant value(const QString &key, const QVariant &def = {}) const;
    void setValue(const QString &key, const QVariant &v);
    void migrateLanguages();
    void migrateRecommendations();
    void reloadHardware(bool probe);
    QString voiceLang() const;
    QString piperFor(const QString &lang) const;
    void readVoiceConf();
    void ensureEngine(std::function<void()> then);
    void send(const QJsonObject &cmd);
    void onEngineOutput();
    void handle(const QJsonObject &ev);
    QJsonObject sessionConfig(QString *problem) const;
    void reallyStart();
    void setStatus(const QString &s);
    void fail(const QString &message);
    void addLine(const QJsonObject &ev, bool draft);
    QString profileDir(const QString &name) const;

    Setup *m_setup;
    VirtualMic *m_vmic;
    QNetworkAccessManager *m_net;
    QSettings m_settings;
    QFileSystemWatcher *m_voiceConfWatcher;
    QString m_cloneEngineWanted;
    bool m_cloneEngineSet = false; // chosen in Settings (else the recommendation's)
    HwProfile::Profile m_hw;
    QString m_sttThisTime; // a smaller model this session (the cloned voice needs the room)
    bool m_omniReady = false;
    bool m_cloneLlm = false; // the quality translator fits beside the cloned voice this time
    QPointer<QProcess> m_engine;
    QByteArray m_buffer;
    QList<std::function<void()>> m_onReady;
    bool m_engineReady = false;
    bool m_running = false;
    bool m_starting = false;
    bool m_paused = false;
    QString m_status;
    QString m_error;
    QString m_notice;
    QString m_detected;
    QString m_mtEngine;
    bool m_sttGpu = false;
    bool m_headphones = false;
    int m_vramFree = -1;
    bool m_gpuCrowded = false;
    bool m_forceFast = false;
    QString m_liveText;
    QString m_liveOriginal;
    QString m_liveId;
    QVariantList m_lines;
    QList<QJsonObject> m_latency; // last finals' stage timings
    QVariantList m_apps;
    QVariantList m_mics;
    bool m_recording = false;
    double m_recordLevel = 0;
    double m_recordSeconds = 0;
    QVariantMap m_recordResult;
    QString m_recordName;
    bool m_preparing = false;
};

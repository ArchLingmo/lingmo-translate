/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

#pragma once

#include <QObject>
#include <QPointer>
#include <QVariantList>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QFile;

// First use: downloads what live translation needs, with progress. Nothing of
// this is in the package or the ISO.
//
//   engine   virtualenv with numpy, onnxruntime, ctranslate2, sentencepiece (~170 MB)
//            + Silero VAD (2 MB, MIT)
//   stt      whisper small (q5_1, 181 MB, MIT), shared with the Assistant
//   whisper:<model>  the whisper models the hardware recommendation uses
//            (hwprofile.h: large-v3-turbo-q5_0 547 MB and large-v3-turbo 1.6 GB on a 6 GB GPU)
//   mt       Opus-MT en→ROMANCE (pt_BR) and ROMANCE→en (CC-BY 4.0), converted to
//            CTranslate2 int8 here (82 MB each; the 290 MB downloads are deleted)
//   voices   piper voices (CC0, 63 MB each): pt_BR faber (unless installed) and en_US joe
//   clone    the voice engine (Chatterbox Multilingual, MIT; ~6 GB), the same
//            environment as Lingmo Studio's dubbing: installed once for both
class Setup : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed) // 0..1, -1 = unknown
    Q_PROPERTY(bool failed READ failed NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)          // enough for subtitles
    Q_PROPERTY(QVariantList components READ components NOTIFY changed)
    Q_PROPERTY(bool voiceEngineReady READ voiceEngineReady NOTIFY changed)
    // what "Download what is needed" still fetches, e.g. "2.4 GB" ("" = nothing)
    Q_PROPERTY(QString pendingSize READ pendingSize NOTIFY changed)

public:
    explicit Setup(QObject *parent = nullptr);

    bool busy() const { return m_busy; }
    QString message() const { return m_message; }
    double progress() const { return m_progress; }
    bool failed() const { return m_failed; }
    bool ready() const;
    bool voiceEngineReady() const;
    QVariantList components() const;
    bool has(const QString &component) const;
    QString pendingSize() const;
    // the whisper models of the hardware recommendation (downloaded with the rest)
    void setRecommendedWhisper(const QStringList &models);

    // components: "engine", "stt", "whisper:<model>", "mt", "voices", "clone";
    // empty = everything but the cloned voice (with the recommended whisper models)
    Q_INVOKABLE void install(const QStringList &components = {});
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void refresh() { Q_EMIT changed(); }

Q_SIGNALS:
    void changed();
    void finished(bool ok);

private:
    struct Step {
        enum Kind { Run, Download } kind = Run;
        QString label;
        QString program;
        QStringList args;
        QString url;
        QString target;
        qint64 approxBytes = 0;
        QString cleanup;   // removed after the step (downloaded archives)
        bool optionalFail = false; // e.g. the pinned GitHub build: fall back to the next step
        QString skipIfExists;
    };
    QList<Step> plan(const QStringList &components) const;
    QStringList defaultComponents() const;
    void next();
    void runStep(const Step &s);
    void download(const Step &s);
    void fail(const QString &message);
    void setMessage(const QString &m, double p);

    QNetworkAccessManager *m_net = nullptr;
    QPointer<QNetworkReply> m_reply;
    QFile *m_file = nullptr;
    QPointer<QProcess> m_proc;
    QList<Step> m_steps;
    int m_index = 0;
    int m_total = 0;
    bool m_busy = false;
    bool m_failed = false;
    bool m_skipNext = false;
    QString m_message;
    double m_progress = -1;
    QString m_log;
    QStringList m_recommendedWhisper;
};

/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

#include "paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace Paths
{
QString dataRoot()
{
    const QString env = qEnvironmentVariable("LINGMO_TRANSLATE_DATA");
    if (!env.isEmpty())
        return env;
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/lingmo");
}

QString translateDir() { return dataRoot() + QStringLiteral("/translate"); }
QString venvDir() { return translateDir() + QStringLiteral("/venv"); }
QString venvPython() { return venvDir() + QStringLiteral("/bin/python"); }
QString modelsDir() { return translateDir() + QStringLiteral("/models"); }
QString whisperDir() { return dataRoot() + QStringLiteral("/whisper-models"); }
QString voicesDir() { return dataRoot() + QStringLiteral("/voices"); }

QString voiceVenvPython()
{
    const QString env = qEnvironmentVariable("LINGMO_TRANSLATE_VOICE_PYTHON");
    return env.isEmpty() ? dataRoot() + QStringLiteral("/dub-venv/bin/python") : env;
}

QString configFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/lingmo/translate.conf");
}

static QString script(const QString &name)
{
    const QString env = qEnvironmentVariable("LINGMO_TRANSLATE_ENGINE_DIR");
    const QStringList dirs = {env,
                              QCoreApplication::applicationDirPath() + QStringLiteral("/../share/lingmo-translate/engine"),
                              QStringLiteral(ENGINE_DIR),
                              QStringLiteral(SOURCE_ENGINE_DIR)};
    for (const QString &dir : dirs) {
        if (!dir.isEmpty() && QFileInfo::exists(dir + QLatin1Char('/') + name))
            return QDir::cleanPath(dir + QLatin1Char('/') + name);
    }
    return QStringLiteral(ENGINE_DIR) + QLatin1Char('/') + name;
}

QString engineScript() { return script(QStringLiteral("engine.py")); }
QString cloneWorker() { return script(QStringLiteral("clone_worker.py")); }

QString whisperModel(const QString &name)
{
    return whisperDir() + QStringLiteral("/ggml-") + name + QStringLiteral(".bin");
}

QString voiceConf()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/lingmoos/voice.conf");
}

QString omniVoicePython()
{
    const QString env = qEnvironmentVariable("LINGMO_OMNIVOICE_VENV");
    return (env.isEmpty() ? dataRoot() + QStringLiteral("/omnivoice-venv") : env) + QStringLiteral("/bin/python");
}

QString omniVoiceWorker()
{
    const QString env = qEnvironmentVariable("LINGMO_OMNIVOICE_WORKER");
    if (!env.isEmpty())
        return env;
    const QStringList candidates = {QCoreApplication::applicationDirPath() + QStringLiteral("/../share/lingmo-voicestudio/engine/workers/omnivoice_worker.py"),
                                    QStringLiteral("/usr/share/lingmo-voicestudio/engine/workers/omnivoice_worker.py")};
    for (const QString &c : candidates) {
        if (QFileInfo::exists(c))
            return QDir::cleanPath(c);
    }
    return candidates.last();
}

QString voiceStudioConfig()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/lingmo/voicestudio.json");
}

QString omniVoiceWeights()
{
    // huggingface_hub: $HF_HOME, else $XDG_CACHE_HOME/huggingface, else ~/.cache/huggingface
    QString hf = qEnvironmentVariable("HF_HOME");
    if (hf.isEmpty()) {
        const QString xdg = qEnvironmentVariable("XDG_CACHE_HOME");
        hf = (xdg.isEmpty() ? QDir::homePath() + QStringLiteral("/.cache") : xdg) + QStringLiteral("/huggingface");
    }
    return hf + QStringLiteral("/hub/models--k2-fsa--OmniVoice");
}
}

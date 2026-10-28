/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

#include "virtualmic.h"

#include <QProcess>

namespace
{
QByteArray pactl(const QStringList &args, int *code = nullptr)
{
    QProcess p;
    p.start(QStringLiteral("pactl"), args);
    p.waitForFinished(5000);
    if (code)
        *code = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    return p.readAllStandardOutput();
}
}

VirtualMic::VirtualMic(QObject *parent)
    : QObject(parent)
{
}

VirtualMic::~VirtualMic()
{
    remove();
}

int VirtualMic::loadModule(const QStringList &args, QString *error)
{
    int code = 0;
    const QByteArray out = pactl(QStringList{QStringLiteral("load-module")} + args, &code);
    const int id = out.trimmed().toInt();
    if (code != 0 || id <= 0) {
        if (error)
            *error = QObject::tr("The audio server refused to create the microphone (%1).").arg(args.value(0));
        return 0;
    }
    return id;
}

bool VirtualMic::sharedExists()
{
    return pactl({QStringLiteral("list"), QStringLiteral("short"), QStringLiteral("sources")}).contains(QByteArray("\t") + SourceName + '\t');
}

bool VirtualMic::ensureShared()
{
    // The microphone for the whole session (login autostart, or the app's first start):
    // unmarked, so no translator run ever removes it, and OBS, Meet or Discord can be set
    // up before translating. Silent until a translation speaks into it.
    if (sharedExists())
        return true;
    cleanupStale();
    const int sink = loadModule({QStringLiteral("module-null-sink"), QStringLiteral("sink_name=%1").arg(QLatin1String(SinkName)),
                                 QStringLiteral("rate=48000"), QStringLiteral("channels=1"),
                                 QStringLiteral("sink_properties='device.description=\"Lingmo Tradutor (uso interno)\" device.icon_name=audio-input-microphone'")},
                                nullptr);
    if (!sink)
        return false;
    const int source = loadModule({QStringLiteral("module-remap-source"), QStringLiteral("master=%1.monitor").arg(QLatin1String(SinkName)),
                                   QStringLiteral("source_name=%1").arg(QLatin1String(SourceName)), QStringLiteral("channels=1"),
                                   QStringLiteral("source_properties='device.description=\"Lingmo Tradutor\" device.icon_name=audio-input-microphone'")},
                                  nullptr);
    if (!source) {
        pactl({QStringLiteral("unload-module"), QString::number(sink)});
        return false;
    }
    return true;
}

bool VirtualMic::create(QString *error)
{
    if (active())
        return true;
    if (ensureShared()) {
        m_shared = true;
        Q_EMIT activeChanged();
        return true;
    }
    // the session microphone could not be made: one of our own, removed when we stop
    cleanupStale();
    // PulseAudio parses the module arguments itself: a value with spaces needs outer quotes
    m_sinkModule = loadModule({QStringLiteral("module-null-sink"), QStringLiteral("sink_name=%1").arg(QLatin1String(SinkName)),
                               QStringLiteral("rate=48000"), QStringLiteral("channels=1"),
                               QStringLiteral("sink_properties='device.description=\"Lingmo Tradutor (uso interno)\" device.icon_name=audio-input-microphone lingmo.translate.owner=app'")},
                              error);
    if (!m_sinkModule)
        return false;
    m_sourceModule = loadModule({QStringLiteral("module-remap-source"), QStringLiteral("master=%1.monitor").arg(QLatin1String(SinkName)),
                                 QStringLiteral("source_name=%1").arg(QLatin1String(SourceName)), QStringLiteral("channels=1"),
                                 QStringLiteral("source_properties='device.description=\"Lingmo Tradutor\" device.icon_name=audio-input-microphone lingmo.translate.owner=app'")},
                                error);
    if (!m_sourceModule) {
        remove();
        return false;
    }
    Q_EMIT activeChanged();
    return true;
}

void VirtualMic::remove()
{
    const bool was = active();
    m_shared = false;
    // the microphone first, so apps see it go before its sink disappears
    if (m_sourceModule > 0)
        pactl({QStringLiteral("unload-module"), QString::number(m_sourceModule)});
    if (m_sinkModule > 0)
        pactl({QStringLiteral("unload-module"), QString::number(m_sinkModule)});
    m_sourceModule = m_sinkModule = 0;
    if (was)
        Q_EMIT activeChanged();
}

int VirtualMic::cleanupStale()
{
    // "index<TAB>name<TAB>argument" lines: the JSON listing of PulseAudio 17 has no module index
    int removed = 0;
    const QList<QByteArray> lines = pactl({QStringLiteral("list"), QStringLiteral("short"), QStringLiteral("modules")}).split('\n');
    // sources before sinks (the remap depends on the sink)
    for (const char *kind : {"module-remap-source", "module-null-sink"}) {
        for (const QByteArray &line : lines) {
            const QList<QByteArray> f = line.split('\t');
            if (f.size() < 3 || f.at(1) != kind)
                continue;
            const QString args = QString::fromUtf8(f.at(2));
            // only modules this app loaded (marked), never the microphone loaded at login
            const bool ours = args.contains(QStringLiteral("lingmo.translate.owner=app"));
            if (ours && (args.contains(QStringLiteral("sink_name=%1").arg(QLatin1String(SinkName)))
                         || args.contains(QStringLiteral("source_name=%1").arg(QLatin1String(SourceName))))) {
                pactl({QStringLiteral("unload-module"), QString::fromUtf8(f.at(0))});
                ++removed;
            }
        }
    }
    return removed;
}

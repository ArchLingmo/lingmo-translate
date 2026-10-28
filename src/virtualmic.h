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
#include <QString>

// The "Lingmo Tradutor" microphone that Meet, Teams and Zoom can pick:
//
//   translated voice → null sink "lingmo_tradutor_out" (nobody hears it)
//                      └ its monitor → remap source "lingmo_tradutor" = the microphone
//
// Created with pactl (PulseAudio protocol: pipewire-pulse on ArchLingmo,
// PulseAudio elsewhere) when speaking starts and removed when it stops. A
// crash leaves the modules behind: they are found by name and removed on the
// next start (and by `lingmo-translate --cleanup`).
class VirtualMic : public QObject
{
    Q_OBJECT
public:
    explicit VirtualMic(QObject *parent = nullptr);
    ~VirtualMic() override;

    static constexpr const char *SinkName = "lingmo_tradutor_out";
    static constexpr const char *SourceName = "lingmo_tradutor";

    bool create(QString *error = nullptr);
    void remove();
    bool active() const { return m_shared || m_sinkModule > 0; }
    // Modules left by an earlier run (crash): returns how many were removed
    static int cleanupStale();
    // The session-wide microphone (made once, never removed): true when it exists
    static bool ensureShared();
    static bool sharedExists();

Q_SIGNALS:
    void activeChanged();

private:
    static int loadModule(const QStringList &args, QString *error);
    int m_sinkModule = 0;
    bool m_shared = false; // the microphone loaded at login: used, never unloaded
    int m_sourceModule = 0;
};

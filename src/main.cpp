/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

// Tradução ao vivo (lingmo-translate)
//
//   lingmo-translate                  the window
//   lingmo-translate --toggle         start/stop with the last settings (Meta+Shift+T)
//   lingmo-translate --start [mode]   subtitles | dubbed | speak | call
//   lingmo-translate --stop
//   lingmo-translate --page translate|voices|models
//   lingmo-translate --background     no window (D-Bus activation for the app actions)
//   lingmo-translate --cleanup        removes a translator microphone left by a crash
//   lingmo-translate --ensure-mic     makes the "Lingmo Tradutor" microphone for the session (login)
//
// One instance per session (com.lingmo.Translate): a second launch hands its
// request over and exits.

#include "livetranslator.h"
#include "setup.h"
#include "virtualmic.h"
#ifdef HAVE_LINGMO_INTENTS
#include "translateintents.h"
#endif

#include <QDBusConnection>
#include <QDBusInterface>
#include <QGuiApplication>
#include <QIcon>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTranslator>

static const QString DBusService = QStringLiteral("com.lingmo.Translate");
static const QString DBusPath = QStringLiteral("/Translate");

class TranslateService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.lingmo.Translate")
public:
    TranslateService(LiveTranslator *t, std::function<void(const QString &)> show, QObject *parent)
        : QObject(parent), m_t(t), m_show(std::move(show)) {}

public Q_SLOTS:
    void Toggle() { m_t->toggle(); }
    void Start(const QString &mode)
    {
        // speaking for the user is never started without the window: it opens instead
        if (mode == QLatin1String("speak") || mode == QLatin1String("call")) {
            m_t->setMode(mode);
            m_show(QStringLiteral("translate"));
            return;
        }
        if (!mode.isEmpty())
            m_t->setMode(mode);
        m_t->start();
    }
    void Stop() { m_t->stop(); }
    void Show(const QString &page) { m_show(page); }
    bool Running() const { return m_t->running(); }

private:
    LiveTranslator *m_t;
    std::function<void(const QString &)> m_show;
};

static void installTranslator(QCoreApplication &app, QTranslator &translator)
{
    for (const QString &dir : {app.applicationDirPath() + QStringLiteral("/../share/lingmo-translate/translations"),
                               app.applicationDirPath(), QStringLiteral(TRANSLATIONS_DIR)}) {
        if (translator.load(QLocale(), QStringLiteral("lingmo-translate"), QStringLiteral("_"), dir)) {
            app.installTranslator(&translator);
            break;
        }
    }
}

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("lingmoos"));
    app.setApplicationName(QStringLiteral("lingmo-translate"));
    app.setDesktopFileName(QStringLiteral("lingmo-translate"));
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("lingmo-translate"), QIcon(QStringLiteral(":/icons/lingmo-translate.svg"))));
    QTranslator translator;
    installTranslator(app, translator);
    app.setApplicationDisplayName(QCoreApplication::translate("main", "Live translation"));

    const QStringList args = app.arguments();
    if (args.contains(QStringLiteral("--ensure-mic")))
        return VirtualMic::ensureShared() ? 0 : 1; // login autostart: the microphone, nothing else
    if (args.contains(QStringLiteral("--cleanup"))) {
        restoreDucking();
        return VirtualMic::cleanupStale() >= 0 ? 0 : 1;
    }
    const bool background = args.contains(QStringLiteral("--background"));
    QString page;
    if (int i = args.indexOf(QStringLiteral("--page")); i > 0 && i + 1 < args.size())
        page = args.at(i + 1);
    QString startMode;
    const int startArg = args.indexOf(QStringLiteral("--start"));
    if (startArg > 0)
        startMode = args.value(startArg + 1).startsWith(QLatin1String("--")) ? QString() : args.value(startArg + 1);

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected() && !bus.registerService(DBusService)) {
        QDBusInterface iface(DBusService, DBusPath, DBusService, bus);
        if (args.contains(QStringLiteral("--toggle")))
            iface.call(QStringLiteral("Toggle"));
        else if (startArg > 0)
            iface.call(QStringLiteral("Start"), startMode);
        else if (args.contains(QStringLiteral("--stop")))
            iface.call(QStringLiteral("Stop"));
        else if (!background)
            iface.call(QStringLiteral("Show"), page);
        return 0;
    }

    // the microphone right after installing too, without waiting for the next login
    VirtualMic::ensureShared();

    Setup setup;
    LiveTranslator live(&setup);
    QObject::connect(&setup, &Setup::finished, &live, [&live](bool) { Q_EMIT live.voicesChanged(); });

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("translator"), &live);
    engine.rootContext()->setContextProperty(QStringLiteral("setup"), &setup);
    engine.rootContext()->setContextProperty(QStringLiteral("startPage"), page);
    // the overlay, the Island activity and the virtual-mic indicator live without the window
    engine.load(QUrl(QStringLiteral("qrc:/qml/Shell.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;
    QObject *shell = engine.rootObjects().first();

    auto show = [&engine, shell](const QString &p) { QMetaObject::invokeMethod(shell, "showWindow", Q_ARG(QVariant, p)); };
    QObject::connect(&live, &LiveTranslator::showRequested, &app, [show] { show(QString()); });

    TranslateService service(&live, show, &app);
    if (bus.isConnected())
        bus.registerObject(DBusPath, &service, QDBusConnection::ExportAllSlots);
#ifdef HAVE_LINGMO_INTENTS
    if (bus.isConnected())
        publishTranslateIntents(&live, &app);
#endif

    // Quit when nothing is left: no window, nothing translating
    // (Shell.qml quits when the window closes while nothing is translating)
    app.setQuitOnLastWindowClosed(false);

    if (args.contains(QStringLiteral("--toggle")) || startArg > 0) {
        if (startMode == QLatin1String("speak") || startMode == QLatin1String("call")) {
            live.setMode(startMode);
            show(QStringLiteral("translate"));
        } else {
            if (!startMode.isEmpty())
                live.setMode(startMode);
            live.start();
        }
    } else if (!background) {
        show(page);
    }
    QObject::connect(&live, &LiveTranslator::stateChanged, &app, [&live, shell] {
        // background instance: exits once translation stops and the window is closed
        const bool windowOpen = shell->property("windowOpen").toBool();
        if (!live.running() && !live.starting() && !windowOpen && !live.recording())
            QMetaObject::invokeMethod(qApp, &QCoreApplication::quit, Qt::QueuedConnection);
    });
    return app.exec();
}

#include "main.moc"

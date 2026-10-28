/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

import QtQuick
import QtQuick.Controls as QQC  // qualified: its Overlay type would hide our Overlay.qml
import QtQuick.Layouts
import QtQuick.Window
import LingmoUI.CompatibleModule 3.0 as LingmoUI

// Everything that lives without the window: the subtitle overlay, the Island
// activity in the status bar and the translator-microphone indicator.
Item {
    id: shell

    property bool windowOpen: mainLoader.item !== null && mainLoader.item.visible
    property bool overlayUnlocked: false
    readonly property bool active: translator.running || translator.starting

    function showWindow(page) {
        if (!mainLoader.active)
            mainLoader.active = true
        const w = mainLoader.item
        if (page && page.length > 0)
            w.openPage(page)
        w.show()
        w.raise()
        w.requestActivate()
    }

    Loader {
        id: mainLoader
        active: false
        source: "qrc:/qml/Main.qml"
        onLoaded: {
            item.shell = shell
            if (startPage.length > 0)
                item.openPage(startPage)
        }
    }

    Connections {
        target: mainLoader.item
        ignoreUnknownSignals: true
        function onClosing() {
            if (!translator.running && !translator.starting && !translator.recording)
                Qt.quit()
        }
    }

    Overlay {
        id: overlay
        shell: shell
        visible: shell.active
    }

    readonly property var modes: [
        { id: "subtitles", title: qsTr("Subtitles"), icon: "media-view-subtitles-symbolic" },
        { id: "dubbed", title: qsTr("Listen dubbed"), icon: "audio-headphones" },
        { id: "speak", title: qsTr("Speak translated"), icon: "audio-input-microphone" },
        { id: "call", title: qsTr("Call"), icon: "call-start" }
    ]
    readonly property string modeIcon: (modes.find(m => m.id === translator.mode) || modes[0]).icon

    readonly property string modeTitle: {
        switch (translator.mode) {
        case "dubbed": return qsTr("Dubbing %1").arg(translator.pairLabel)
        case "speak": return qsTr("Speaking %1").arg(translator.pairLabel)
        case "call": return qsTr("Call %1").arg(translator.pairLabel)
        default: return qsTr("Translating %1").arg(translator.pairLabel)
        }
    }

    LingmoUI.IslandActivity {
        id: island
        active: shell.active
        appId: "lingmo-translate"
        kind: translator.virtualMicActive ? "call" : "generic"
        title: shell.modeTitle
        // what is happening now: loading → "Ready: speak now" → the line being translated
        subtitle: translator.liveText.length > 0 ? translator.liveText
                : translator.status.length > 0 ? translator.status
                : translator.virtualMicActive ? qsTr("Translator microphone on: it speaks for you") : ""
        iconName: translator.virtualMicActive ? "audio-input-microphone" : "preferences-desktop-locale"
        progress: -1
        hints: ({
            startTime: startedAt,
            paused: translator.paused,
            urgent: translator.virtualMicActive,
            // orange while it loads, green once you can talk (or it is listening)
            accent: translator.starting ? LingmoUI.Theme.orangeColor.toString()
                  : translator.running ? LingmoUI.Theme.greenColor.toString() : LingmoUI.Theme.blueColor.toString(),
            indeterminate: translator.starting,
            detail: translator.sourceDetail,
            actionIcons: ({ mode: shell.modeIcon, lang: "preferences-desktop-locale-symbolic", target: "go-next-symbolic" })
        })
        // "lang": the source language ("Auto", "JA"…) switched live, from a small picker
        // "mode": subtitles, dubbed, speak or call, switched without opening the window
        actions: [ { id: "mode", label: qsTr("Mode") }, { id: "lang", label: translator.fromShort },
                   { id: "target", label: translator.toLang.toUpperCase() } ]
                 .concat(translator.paused ? [ { id: "resume", label: qsTr("Resume") }, { id: "stop", label: qsTr("Stop") }, { id: "open", label: qsTr("Open") } ]
                                           : [ { id: "pause", label: qsTr("Pause") }, { id: "stop", label: qsTr("Stop") }, { id: "open", label: qsTr("Open") } ])
        finishMessage: qsTr("Translation stopped")
        property double startedAt: 0
        onActiveChanged: if (active) startedAt = Date.now()
        onActionInvoked: (actionId) => {
            if (actionId === "pause") translator.setPaused(true)
            else if (actionId === "resume") translator.setPaused(false)
            else if (actionId === "stop") translator.stop()
            else if (actionId === "open") shell.showWindow("translate")
            else if (actionId === "lang") langMenu.popup()
            else if (actionId === "mode") modeMenu.popup()
            else if (actionId === "target") targetMenu.popup()
        }
    }

    // The source-language picker opened from the Island card: a Lingmo glass menu at the
    // pointer; it closes on a choice, on Esc, on a click outside or when it loses focus
    LingmoUI.DesktopMenu {
        id: langMenu
        title: translator.mode === "call" ? qsTr("They speak") : qsTr("Spoken language")
        onActiveChanged: if (!active && visible) dismissPopup()

        Shortcut {
            sequences: [StandardKey.Cancel]
            context: Qt.WindowShortcut
            onActivated: langMenu.dismissPopup()
        }

        QQC.Label {
            text: langMenu.title
            color: LingmoUI.Theme.disabledTextColor
            font.pointSize: LingmoUI.Units.fontCaption
            leftPadding: LingmoUI.Units.largeSpacing
            topPadding: LingmoUI.Units.smallSpacing
            bottomPadding: LingmoUI.Units.smallSpacing / 2
        }

        Repeater {
            // the current mode's source: speaking, my own language (never detected)
            model: translator.mode === "speak" ? translator.sourceLanguages.filter(c => c !== "auto") : translator.sourceLanguages
            QQC.MenuItem {
                required property string modelData
                Layout.fillWidth: true
                Layout.preferredWidth: 250  // "Detectar automaticamente" in full
                text: translator.languageTitle(modelData)
                checkable: true
                checked: translator.fromLang === modelData || (translator.mode === "speak" && translator.fromLang === "auto" && modelData === "pt")
                onTriggered: {
                    translator.fromLang = modelData
                    langMenu.dismissPopup()
                }
            }
        }
    }

    // The mode picker from the Island card: the session restarts in the new mode with that
    // mode's own languages and voice
    LingmoUI.DesktopMenu {
        id: modeMenu
        onActiveChanged: if (!active && visible) dismissPopup()

        Shortcut {
            sequences: [StandardKey.Cancel]
            context: Qt.WindowShortcut
            onActivated: modeMenu.dismissPopup()
        }

        QQC.Label {
            text: qsTr("Mode")
            color: LingmoUI.Theme.disabledTextColor
            font.pointSize: LingmoUI.Units.fontCaption
            leftPadding: LingmoUI.Units.largeSpacing
            topPadding: LingmoUI.Units.smallSpacing
            bottomPadding: LingmoUI.Units.smallSpacing / 2
        }

        Repeater {
            model: shell.modes
            QQC.MenuItem {
                required property var modelData
                Layout.fillWidth: true
                Layout.preferredWidth: 220
                text: modelData.title
                icon.name: modelData.icon
                checkable: true
                checked: translator.mode === modelData.id
                onTriggered: {
                    modeMenu.dismissPopup()
                    if (translator.mode === modelData.id)
                        return
                    const running = translator.running || translator.starting
                    if (running)
                        translator.stop()
                    translator.mode = modelData.id
                    if (running)
                        Qt.callLater(translator.start)
                }
            }
        }
    }

    // The language the translation goes to; the session restarts with it
    readonly property var targetCodes: ["pt", "en", "es", "fr", "it"]  // the same as the Translate page
    LingmoUI.DesktopMenu {
        id: targetMenu
        onActiveChanged: if (!active && visible) dismissPopup()

        Shortcut {
            sequences: [StandardKey.Cancel]
            context: Qt.WindowShortcut
            onActivated: targetMenu.dismissPopup()
        }

        QQC.Label {
            text: translator.mode === "call" ? qsTr("I speak") : translator.mode === "speak" ? qsTr("They hear") : qsTr("Translate to")
            color: LingmoUI.Theme.disabledTextColor
            font.pointSize: LingmoUI.Units.fontCaption
            leftPadding: LingmoUI.Units.largeSpacing
            topPadding: LingmoUI.Units.smallSpacing
            bottomPadding: LingmoUI.Units.smallSpacing / 2
        }

        Repeater {
            model: shell.targetCodes
            QQC.MenuItem {
                required property string modelData
                Layout.fillWidth: true
                Layout.preferredWidth: 220
                text: translator.languageTitle(modelData)
                checkable: true
                checked: translator.toLang === modelData
                onTriggered: {
                    targetMenu.dismissPopup()
                    if (translator.toLang === modelData)
                        return
                    const running = translator.running || translator.starting
                    if (running)
                        translator.stop()
                    translator.toLang = modelData
                    if (running)
                        Qt.callLater(translator.start)
                }
            }
        }
    }
}

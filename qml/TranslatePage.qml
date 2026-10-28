/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import LingmoUI.CompatibleModule 3.0 as LingmoUI

Item {
    id: page

    property Item shell
    signal openModels()
    signal openVoices()

    readonly property var modes: ["subtitles", "dubbed", "speak", "call"]
    readonly property var sources: ["system", "app", "mic"]
    // Japanese, Chinese, Korean and German go through the local AI model (no fast draft)
    // shared with the Island picker; speaking, my own language is never detected
    readonly property var fromCodes: translator.mode === "speak" ? translator.sourceLanguages.filter(c => c !== "auto")
                                                                 : translator.sourceLanguages
    readonly property var toCodes: ["pt", "en", "es", "fr", "it"]
    readonly property bool speaks: translator.mode !== "subtitles"
    readonly property bool usesMic: translator.mode === "speak" || translator.mode === "call"
    readonly property bool locked: translator.running || translator.starting
    // the hardware recommendation for this mode (hwprofile.h, ~/.config/lingmoos/hardware.conf)
    readonly property var rec: translator.recommended
    readonly property string recMark: " " + qsTr("(recommended)")
    readonly property bool recClone: rec.voice === "clone"
    readonly property bool cloneChosen: page.usesMic && translator.voice.startsWith("profile:")

    // estimates in GB of 1000 MB, as the measurements are quoted (4602 MB → "4.6 GB")
    function gb(mb) {
        return Number(mb / 1000).toLocaleString(Qt.locale(), 'f', 1)
    }

    function recSummary() {
        const where = rec.tier === "cpu" ? qsTr("no NVIDIA graphics card with CUDA")
                                         : qsTr("%1, %2 GB").arg(rec.gpuName).arg(Math.round(rec.vramTotalMb / 1024))
        const parts = ["Whisper " + rec.sttModel,
                       rec.quality === "quality" ? qsTr("instant draft refined by the AI model") : qsTr("instant translation only")]
        if (rec.voice === "clone")
            parts.push(qsTr("your cloned voice"))
        else if (rec.voice === "piper")
            parts.push(qsTr("a piper voice"))
        let text = qsTr("Recommended for this computer (%1): %2.").arg(where).arg(parts.join(" · "))
        if (rec.vramMb > 0)
            text += " " + qsTr("About %1 GB of video memory.").arg(gb(rec.vramMb))
        else
            text += " " + qsTr("Everything runs on the processor.")
        return text
    }

    function langTitle(code) {
        switch (code) {
        case "auto": return qsTr("Detect automatically")
        case "en": return qsTr("English")
        case "pt": return qsTr("Portuguese (Brazil)")
        case "es": return qsTr("Spanish")
        case "fr": return qsTr("French")
        case "it": return qsTr("Italian")
        case "de": return qsTr("German")
        case "ja": return qsTr("Japanese")
        case "zh": return qsTr("Chinese")
        case "ko": return qsTr("Korean")
        }
        return code
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentHeight: column.implicitHeight + LingmoUI.Units.largeSpacing * 2
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        LingmoUI.WheelHandler { target: flick }

        ColumnLayout {
            id: column
            x: LingmoUI.Units.largeSpacing * 2
            width: flick.width - LingmoUI.Units.largeSpacing * 4
            spacing: LingmoUI.Units.largeSpacing

            LingmoUI.PageHeader {
                Layout.fillWidth: true
                title: qsTr("Live translation")
                subtitle: translator.running ? translator.status + " · " + translator.pairLabel
                                             : qsTr("Subtitles and voice for videos, meetings and calls, on this computer")
                reserveWindowControls: true

                LingmoUI.IconButton {
                    visible: translator.running
                    iconName: translator.paused ? "media-playback-start" : "media-playback-pause"
                    tipText: translator.paused ? qsTr("Resume") : qsTr("Pause")
                    onClicked: translator.setPaused(!translator.paused)
                }
                LingmoUI.ActionButton {
                    text: page.locked ? qsTr("Stop") : qsTr("Start")
                    iconName: page.locked ? "media-playback-stop" : "media-playback-start"
                    primary: !page.locked
                    destructive: page.locked
                    busy: translator.starting
                    enabled: setup.ready || page.locked
                    tipText: qsTr("Meta+Shift+T")
                    onClicked: translator.toggle()
                }
            }

            // ---- messages
            LingmoUI.Banner {
                Layout.fillWidth: true
                type: LingmoUI.Banner.Info
                visible: !setup.ready
                title: qsTr("Download the models once")
                text: qsTr("Speech recognition and translation run on this computer. They need about %1, downloaded only now.").arg(setup.pendingSize || "520 MB")
                LingmoUI.ActionButton { text: qsTr("Download"); primary: true; onClicked: page.openModels() }
            }
            LingmoUI.Banner {
                Layout.fillWidth: true
                type: translator.gpuCrowded ? LingmoUI.Banner.Warning : LingmoUI.Banner.Error
                text: translator.error
                closable: true
                onClosed: translator.clearError()
                LingmoUI.ActionButton {
                    visible: translator.gpuCrowded
                    text: qsTr("Free the GPU")
                    tipText: qsTr("Unloads the AI models held in the video memory (Ollama, Voice Studio)")
                    onClicked: translator.freeGpu()
                }
            }
            LingmoUI.Banner {
                Layout.fillWidth: true
                type: LingmoUI.Banner.Info
                text: translator.notice
            }
            LingmoUI.Banner {
                Layout.fillWidth: true
                type: LingmoUI.Banner.Warning
                visible: page.speaks && !translator.headphones
                title: qsTr("Use headphones")
                text: page.usesMic ? qsTr("Without headphones your microphone hears the other side and the translator translates them as if it were you.")
                                   : qsTr("Without headphones the translated voice and the original mix in the room.")
            }
            LingmoUI.Banner {
                Layout.fillWidth: true
                type: LingmoUI.Banner.Warning
                visible: translator.virtualMicActive
                iconName: "audio-input-microphone"
                title: qsTr("The translator microphone is on")
                text: qsTr("In Meet, Teams or Zoom pick the microphone “Lingmo Tradutor”. It speaks your words, translated. It is removed when you stop.")
            }

            // ---- what to do
            LingmoUI.Card {
                id: modeCard
                Layout.fillWidth: true
                enabled: !page.locked
                ColumnLayout {
                    anchors.fill: parent
                    anchors.leftMargin: modeCard.padding
                    anchors.rightMargin: modeCard.padding
                    anchors.topMargin: LingmoUI.Units.largeSpacing
                    anchors.bottomMargin: LingmoUI.Units.largeSpacing
                    spacing: LingmoUI.Units.largeSpacing

                    Label { text: qsTr("Mode"); font.pointSize: LingmoUI.Units.fontSubheading; font.bold: true }
                    LingmoUI.SegmentedControl {
                        Layout.fillWidth: true
                        fillWidth: true
                        model: [ { text: qsTr("Subtitles"), iconName: "media-view-subtitles-symbolic" },
                                 { text: qsTr("Listen dubbed"), iconName: "audio-headphones" },
                                 { text: qsTr("Speak translated"), iconName: "audio-input-microphone" },
                                 { text: qsTr("Call"), iconName: "call-start" } ]
                        currentIndex: Math.max(0, page.modes.indexOf(translator.mode))
                        onActivated: (index) => translator.mode = page.modes[index]
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: LingmoUI.Theme.secondaryTextColor
                        font.pointSize: LingmoUI.Units.fontCaption
                        text: {
                            switch (translator.mode) {
                            case "dubbed": return qsTr("A translated voice in your headphones over the original, which plays lower. About 1 s behind on short lines, 2–3 s on long ones.")
                            case "speak": return qsTr("You speak, they hear the translation from the “Lingmo Tradutor” microphone, in your own voice or a synthetic one. About 3–5 s.")
                            case "call": return qsTr("Both ways: you hear them translated (dubbed or subtitles), and your words go translated through the translator microphone.")
                            default: return qsTr("Translated subtitles on top of everything, about 1–2 s behind the speech.")
                            }
                        }
                    }

                    // what suits this computer in this mode; anything set by hand can go back to it
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: LingmoUI.Units.largeSpacing
                        LingmoUI.IconItem {
                            Layout.alignment: Qt.AlignTop
                            width: LingmoUI.Units.iconSizes.small
                            height: width
                            source: "cpu"
                            color: LingmoUI.Theme.secondaryTextColor
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: LingmoUI.Theme.secondaryTextColor
                            font.pointSize: LingmoUI.Units.fontCaption
                            text: page.recSummary()
                        }
                        LingmoUI.ActionButton {
                            visible: !translator.followsRecommendation
                            text: qsTr("Use recommended")
                            flat: true
                            tipText: qsTr("Forget what you changed in this mode and use what suits this computer")
                            onClicked: translator.useRecommended()
                        }
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: LingmoUI.Units.largeSpacing
                        rowSpacing: LingmoUI.Units.smallSpacing

                        Label { text: translator.mode === "call" ? qsTr("Their audio") : qsTr("Listen to"); visible: translator.mode !== "speak" }
                        RowLayout {
                            visible: translator.mode !== "speak"
                            Layout.fillWidth: true
                            LingmoUI.SegmentedControl {
                                model: [qsTr("Whole system"), qsTr("One app"), qsTr("Microphone")]
                                currentIndex: Math.max(0, page.sources.indexOf(translator.source))
                                onActivated: (index) => { translator.source = page.sources[index]; translator.refreshSources() }
                            }
                            ComboBox {
                                id: appBox
                                visible: translator.source === "app"
                                Layout.fillWidth: true
                                // the configured app stays in the list even when it is not playing right now
                                model: {
                                    const names = translator.apps.map(a => a.name)
                                    if (translator.sourceApp.length > 0 && names.indexOf(translator.sourceApp) < 0)
                                        names.unshift(translator.sourceApp)
                                    return names
                                }
                                editable: true
                                editText: translator.sourceApp
                                onActivated: translator.sourceApp = currentText
                                onAccepted: translator.sourceApp = editText
                                Component.onCompleted: currentIndex = Math.max(0, model.indexOf(translator.sourceApp))
                            }
                            LingmoUI.IconButton {
                                visible: translator.source === "app"
                                iconName: "view-refresh"
                                tipText: qsTr("Look for apps playing sound")
                                onClicked: translator.refreshSources()
                            }
                        }

                        // each mode keeps its own pair; a call: they speak one language, I speak the other
                        Label { text: translator.mode === "call" ? qsTr("They speak") : translator.mode === "speak" ? qsTr("I speak") : qsTr("From") }
                        ComboBox {
                            Layout.fillWidth: true
                            model: page.fromCodes.map(c => page.langTitle(c))
                            currentIndex: Math.max(0, page.fromCodes.indexOf(translator.mode === "speak" && translator.fromLang === "auto" ? "pt" : translator.fromLang))
                            onActivated: (i) => translator.fromLang = page.fromCodes[i]
                        }
                        Label { text: translator.mode === "call" ? qsTr("I speak") : translator.mode === "speak" ? qsTr("They hear") : qsTr("To") }
                        ComboBox {
                            Layout.fillWidth: true
                            model: page.toCodes.map(c => page.langTitle(c))
                            currentIndex: Math.max(0, page.toCodes.indexOf(translator.toLang))
                            onActivated: (i) => translator.toLang = page.toCodes[i]
                        }

                        Label { text: qsTr("Speech recognition") }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            ComboBox {
                                id: sttBox
                                Layout.fillWidth: true
                                readonly property var items: translator.sttModels
                                model: items.map(m => m.title + " · " + m.size + (m.recommended ? page.recMark : "")
                                                      + (m.installed ? "" : " · " + qsTr("downloaded on first use")))
                                currentIndex: Math.max(0, items.findIndex(m => m.id === translator.sttModel))
                                onActivated: (i) => translator.sttModel = items[i].id
                            }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                color: LingmoUI.Theme.secondaryTextColor
                                font.pointSize: LingmoUI.Units.fontCaption
                                text: page.rec.tier === "cpu"
                                      ? qsTr("Bigger models understand more and are slower on the processor.")
                                      : qsTr("Bigger models understand more and need more video memory: %1 GB for this one.")
                                        .arg(page.gb((sttBox.items[sttBox.currentIndex] || {}).vramMb || 0))
                            }
                        }

                        Label { text: qsTr("Translation") }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            LingmoUI.SegmentedControl {
                                model: [qsTr("Best quality") + (page.rec.quality === "quality" ? page.recMark : ""),
                                        qsTr("Fastest") + (page.rec.quality === "fast" ? page.recMark : "")]
                                currentIndex: translator.quality === "fast" ? 1 : 0
                                onActivated: (i) => translator.quality = i === 1 ? "fast" : "quality"
                            }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                color: LingmoUI.Theme.secondaryTextColor
                                font.pointSize: LingmoUI.Units.fontCaption
                                text: translator.quality === "fast"
                                      ? qsTr("Opus-MT only: about 0.1 s per sentence, sometimes too literal.")
                                      : page.usesMic && !page.rec.llmBesideClone
                                        ? qsTr("An instant draft, then the local AI model refines it (about 1 s more). Your cloned voice uses the fast one when both do not fit in the video memory.")
                                        : qsTr("An instant draft, then the local AI model refines it (about 1 s more).")
                            }
                        }

                        Label { text: qsTr("Voice"); visible: page.speaks }
                        RowLayout {
                            visible: page.speaks
                            Layout.fillWidth: true
                            ComboBox {
                                id: voiceBox
                                Layout.fillWidth: true
                                // dubbed listening speaks with piper: a cloned voice and the AI translator don't fit together
                                readonly property var items: translator.voices.filter(v => page.usesMic || v.kind !== "clone")
                                model: items.map(v => v.title + (v.kind === "clone" && !v.ready ? " " + qsTr("(not ready)") : "")
                                                      + (v.id === translator.recommendedVoice ? page.recMark : ""))
                                currentIndex: Math.max(0, items.findIndex(v => v.id === translator.voice))
                                onActivated: (i) => translator.voice = items[i].id
                            }
                            LingmoUI.ActionButton {
                                visible: page.usesMic
                                text: qsTr("Record my voice")
                                flat: true
                                onClicked: page.openVoices()
                            }
                        }
                        Item { visible: voiceHint.visible; implicitWidth: 1; implicitHeight: 1 }
                        Label {
                            id: voiceHint
                            Layout.fillWidth: true
                            visible: page.speaks && text.length > 0
                            wrapMode: Text.WordWrap
                            font.pointSize: LingmoUI.Units.fontCaption
                            color: page.cloneChosen && !page.rec.cloneAllowed ? LingmoUI.Theme.orangeColor : LingmoUI.Theme.secondaryTextColor
                            text: page.cloneChosen && !page.rec.cloneAllowed
                                  ? qsTr("A cloned voice is not recommended on this computer: without room on a CUDA graphics card it runs on the processor, several seconds per sentence.")
                                  : page.usesMic && page.recClone && !translator.recommendedVoice.startsWith("profile:")
                                    ? qsTr("Recommended: your own cloned voice. Record it once to use it.")
                                    : translator.mode === "dubbed" ? qsTr("Dubbing uses piper voices, so the AI translator fits beside them.") : ""
                        }
                        Label { text: qsTr("Cloned voice"); visible: page.usesMic && translator.voice.startsWith("profile:") }
                        ColumnLayout {
                            visible: page.usesMic && translator.voice.startsWith("profile:")
                            Layout.fillWidth: true
                            spacing: 2
                            LingmoUI.SegmentedControl {
                                model: [qsTr("Best quality"), qsTr("Fast")]
                                currentIndex: translator.voiceQuality === "fast" ? 1 : 0
                                onActivated: (i) => translator.voiceQuality = i === 1 ? "fast" : "quality"
                            }
                            // the engine is one choice for the whole system, made in Settings › AI
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                color: translator.cloneEngineFallback ? LingmoUI.Theme.orangeColor : LingmoUI.Theme.secondaryTextColor
                                font.pointSize: LingmoUI.Units.fontCaption
                                text: translator.cloneEngineFallback
                                      ? qsTr("OmniVoice is not turned on in Voice Studio: using Chatterbox.")
                                      : qsTr("Engine: %1 (change it in Settings › AI › Cloned voice).").arg(translator.cloneEngine === "omnivoice" ? "OmniVoice" : "Chatterbox")
                            }
                        }

                        Label { text: qsTr("Hear them"); visible: translator.mode === "call" }
                        LingmoUI.SegmentedControl {
                            visible: translator.mode === "call"
                            model: [qsTr("Dubbed"), qsTr("Subtitles only")]
                            currentIndex: translator.callDubbed ? 0 : 1
                            onActivated: (i) => translator.callDubbed = i === 0
                        }

                        Label { text: qsTr("Original audio"); visible: translator.mode === "dubbed" || (translator.mode === "call" && translator.callDubbed) }
                        RowLayout {
                            visible: translator.mode === "dubbed" || (translator.mode === "call" && translator.callDubbed)
                            Slider {
                                Layout.fillWidth: true
                                from: 0.1; to: 1.0; stepSize: 0.05
                                value: translator.duck
                                onMoved: translator.duck = value
                            }
                            Label { text: Math.round(translator.duck * 100) + "%"; color: LingmoUI.Theme.secondaryTextColor }
                        }
                    }
                }
            }

            // ---- subtitles
            LingmoUI.Card {
                id: subsCard
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    anchors.leftMargin: subsCard.padding
                    anchors.rightMargin: subsCard.padding
                    anchors.topMargin: LingmoUI.Units.largeSpacing
                    anchors.bottomMargin: LingmoUI.Units.largeSpacing
                    spacing: LingmoUI.Units.smallSpacing

                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Subtitles"); font.pointSize: LingmoUI.Units.fontSubheading; font.bold: true; Layout.fillWidth: true }
                        Switch { text: qsTr("Original text"); checked: translator.showOriginal; onToggled: translator.showOriginal = checked }
                        LingmoUI.ActionButton {
                            text: page.shell && page.shell.overlayUnlocked ? qsTr("Done moving") : qsTr("Move subtitles")
                            flat: !(page.shell && page.shell.overlayUnlocked)
                            primary: page.shell && page.shell.overlayUnlocked
                            enabled: translator.running
                            onClicked: page.shell.overlayUnlocked = !page.shell.overlayUnlocked
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Text size"); color: LingmoUI.Theme.secondaryTextColor }
                        Slider {
                            Layout.fillWidth: true
                            from: 0.7; to: 1.8; stepSize: 0.05
                            value: translator.overlayFontScale
                            onMoved: translator.overlayFontScale = value
                        }
                    }

                    ListView {
                        id: transcript
                        Layout.fillWidth: true
                        Layout.preferredHeight: 200
                        clip: true
                        spacing: LingmoUI.Units.smallSpacing
                        boundsBehavior: Flickable.StopAtBounds
                        model: translator.lines
                        ScrollBar.vertical: ScrollBar {}
                        onCountChanged: positionViewAtEnd()
                        delegate: ColumnLayout {
                            width: ListView.view.width - LingmoUI.Units.largeSpacing
                            spacing: 0
                            Label {
                                Layout.fillWidth: true
                                visible: translator.showOriginal
                                text: modelData.original
                                color: LingmoUI.Theme.secondaryTextColor
                                font.pointSize: LingmoUI.Units.fontCaption
                                wrapMode: Text.WordWrap
                            }
                            Label {
                                Layout.fillWidth: true
                                text: modelData.text + (modelData.draft ? " …" : "")
                                opacity: modelData.draft ? 0.75 : 1
                                font.pointSize: LingmoUI.Units.fontBody
                                wrapMode: Text.WordWrap
                            }
                        }
                        LingmoUI.EmptyState {
                            anchors.centerIn: parent
                            visible: transcript.count === 0
                            iconName: "media-view-subtitles-symbolic"
                            title: translator.running ? qsTr("Listening…") : qsTr("Nothing translated yet")
                            text: translator.running ? translator.liveText : qsTr("Start, and the translated lines appear here and on the screen.")
                        }
                    }
                }
            }

            // ---- debug: latency per stage
            LingmoUI.Card {
                id: debugCard
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    anchors.leftMargin: debugCard.padding
                    anchors.rightMargin: debugCard.padding
                    anchors.topMargin: LingmoUI.Units.largeSpacing
                    anchors.bottomMargin: LingmoUI.Units.largeSpacing
                    spacing: LingmoUI.Units.smallSpacing
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Timings"); font.pointSize: LingmoUI.Units.fontSubheading; font.bold: true; Layout.fillWidth: true }
                        LingmoUI.Badge { visible: translator.running; text: translator.sttOnGpu ? qsTr("Speech: GPU") : qsTr("Speech: CPU"); tint: translator.sttOnGpu ? LingmoUI.Theme.greenColor : LingmoUI.Theme.orangeColor }
                        LingmoUI.Badge { visible: translator.running && translator.mtEngine.length > 0; text: translator.mtEngine === "llm" ? qsTr("Translation: AI") : qsTr("Translation: Opus-MT"); subtle: true }
                        Switch { checked: translator.debugPanel; onToggled: translator.debugPanel = checked }
                    }
                    Label {
                        visible: translator.debugPanel
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: LingmoUI.Theme.secondaryTextColor
                        font.pointSize: LingmoUI.Units.fontCaption
                        text: qsTr("Milliseconds after the speaker stopped: the last sentence and the median.")
                    }
                    GridLayout {
                        visible: translator.debugPanel
                        columns: 3
                        columnSpacing: LingmoUI.Units.largeSpacing * 2
                        rowSpacing: 2
                        Label { text: qsTr("Stage"); color: LingmoUI.Theme.secondaryTextColor; font.pointSize: LingmoUI.Units.fontCaption }
                        Label { text: qsTr("Last"); color: LingmoUI.Theme.secondaryTextColor; font.pointSize: LingmoUI.Units.fontCaption }
                        Label { text: qsTr("Median"); color: LingmoUI.Theme.secondaryTextColor; font.pointSize: LingmoUI.Units.fontCaption }
                        Repeater {
                            model: translator.debugPanel ? translator.latency : []
                            delegate: Repeater {
                                required property var modelData
                                model: [modelData.stage, modelData.last + " ms", modelData.median + " ms"]
                                delegate: Label { required property var modelData; text: modelData; font.pointSize: LingmoUI.Units.fontBody }
                            }
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: LingmoUI.Theme.secondaryTextColor
                font.pointSize: LingmoUI.Units.fontCaption
                text: qsTr("Everything runs on this computer: no audio or text is uploaded.")
            }
        }
    }
}

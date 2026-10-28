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

// Guided recording of a voice profile: a text to read, a level meter, a
// 30-second limit, then a quality check (length, clipping, noise, pauses).
Dialog {
    id: dialog
    modal: true
    anchors.centerIn: parent
    width: Math.min(parent.width - LingmoUI.Units.largeSpacing * 4, 620)
    title: qsTr("Record your voice")
    closePolicy: translator.recording ? Popup.NoAutoClose : Popup.CloseOnEscape

    property string name
    property bool rerecord: false
    readonly property var rec: translator.recordResult
    readonly property bool done: rec && rec.file !== undefined && !translator.recording

    function openFor(existing) {
        name = existing
        rerecord = existing.length > 0
        nameField.text = existing
        open()
        nameField.forceActiveFocus()
    }

    function problemText(p) {
        switch (p) {
        case "short": return qsTr("Too short: read the whole text (at least 18 seconds).")
        case "clipping": return qsTr("Too loud, the sound clips: move a little away from the microphone.")
        case "noisy": return qsTr("Too much background noise: find a quieter place.")
        case "quiet": return qsTr("Too quiet: come closer to the microphone or raise its volume.")
        case "pauses": return qsTr("Too many silences: read continuously.")
        }
        return p
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: LingmoUI.Units.largeSpacing

        TextField {
            id: nameField
            Layout.fillWidth: true
            placeholderText: qsTr("Profile name (e.g. My voice)")
            enabled: !dialog.rerecord && !translator.recording && !dialog.done
            onAccepted: if (text.trim().length > 0 && !translator.recording) translator.startRecording(text, "")
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: LingmoUI.Theme.secondaryTextColor
            text: qsTr("Read this aloud at your normal pace, as in a meeting. Use the microphone you use for calls.")
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: reading.implicitHeight + LingmoUI.Units.largeSpacing * 2
            radius: LingmoUI.Theme.mediumRadius
            color: LingmoUI.Theme.alternateBackgroundColor
            Label {
                id: reading
                anchors.fill: parent
                anchors.margins: LingmoUI.Units.largeSpacing
                text: translator.readingText()
                wrapMode: Text.WordWrap
                font.pointSize: LingmoUI.Units.fontSubheading
                lineHeight: 1.2
            }
        }

        // level meter
        RowLayout {
            Layout.fillWidth: true
            spacing: LingmoUI.Units.largeSpacing
            LingmoUI.IconItem {
                Layout.preferredWidth: 22; Layout.preferredHeight: 22
                source: "audio-input-microphone"
                color: translator.recording ? LingmoUI.Theme.redColor : LingmoUI.Theme.secondaryTextColor
            }
            Rectangle {
                Layout.fillWidth: true
                height: 10
                radius: 5
                color: LingmoUI.Theme.alternateBackgroundColor
                Rectangle {
                    height: parent.height
                    radius: 5
                    width: parent.width * Math.min(1, Math.sqrt(translator.recordLevel))
                    color: translator.recordLevel > 0.95 ? LingmoUI.Theme.redColor
                         : translator.recordLevel > 0.1 ? LingmoUI.Theme.greenColor : LingmoUI.Theme.orangeColor
                    Behavior on width { NumberAnimation { duration: LingmoUI.Units.veryShortDuration } }
                }
            }
            Label {
                text: qsTr("%1 / 30 s").arg(Math.floor(translator.recordSeconds))
                font.pointSize: LingmoUI.Units.fontBody
                color: LingmoUI.Theme.secondaryTextColor
            }
        }

        // result of the check
        LingmoUI.Banner {
            Layout.fillWidth: true
            visible: dialog.done
            type: dialog.done && dialog.rec.ok ? LingmoUI.Banner.Success : LingmoUI.Banner.Warning
            title: dialog.done && dialog.rec.ok ? qsTr("Good recording") : qsTr("The recording could be better")
            text: dialog.done ? (dialog.rec.ok
                                 ? qsTr("%1 s, noise margin %2 dB.").arg(dialog.rec.seconds).arg(dialog.rec.snrDb)
                                 : (dialog.rec.problems || []).map(p => dialog.problemText(p)).join("\n")) : ""
        }
        LingmoUI.Banner {
            Layout.fillWidth: true
            type: LingmoUI.Banner.Info
            visible: translator.preparing
            text: qsTr("Preparing your voice… (once; then every sentence reuses it)")
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: LingmoUI.Theme.secondaryTextColor
            font.pointSize: LingmoUI.Units.fontCaption
            text: qsTr("Only you can create your voice profile. It stays on this computer, readable only by you.")
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            LingmoUI.ActionButton {
                text: qsTr("Cancel")
                visible: !translator.recording
                onClicked: { if (dialog.done) translator.discardRecording(nameField.text); dialog.close() }
            }
            LingmoUI.ActionButton {
                visible: !dialog.done || translator.recording
                text: translator.recording ? qsTr("Stop") : qsTr("Start recording")
                iconName: translator.recording ? "media-playback-stop" : "media-record"
                primary: !translator.recording
                destructive: translator.recording
                enabled: translator.recording || nameField.text.trim().length > 0
                onClicked: translator.recording ? translator.stopRecording() : translator.startRecording(nameField.text, "")
            }
            LingmoUI.ActionButton {
                visible: dialog.done
                text: qsTr("Record again")
                onClicked: translator.startRecording(nameField.text, "")
            }
            LingmoUI.ActionButton {
                visible: dialog.done
                text: qsTr("Save")
                primary: true
                onClicked: { translator.saveProfile(nameField.text); dialog.close() }
            }
        }
    }
}

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
import QtQuick.Dialogs
import LingmoUI.CompatibleModule 3.0 as LingmoUI

Item {
    id: page
    signal openModels()
    function record() { recordDialog.openFor("") }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: LingmoUI.Units.largeSpacing * 2
        anchors.rightMargin: LingmoUI.Units.largeSpacing * 2
        anchors.bottomMargin: LingmoUI.Units.largeSpacing * 2
        spacing: LingmoUI.Units.largeSpacing

        LingmoUI.PageHeader {
            Layout.fillWidth: true
            title: qsTr("Voice profiles")
            subtitle: qsTr("Your own voice, to speak translated in calls")
            reserveWindowControls: true
            LingmoUI.ActionButton { text: qsTr("Import my recording…"); flat: true; onClicked: importDialog.open() }
            LingmoUI.ActionButton { text: qsTr("Record new profile"); primary: true; iconName: "media-record"; onClicked: recordDialog.openFor("") }
        }

        LingmoUI.Banner {
            Layout.fillWidth: true
            type: LingmoUI.Banner.Info
            visible: !setup.voiceEngineReady
            title: qsTr("The voice engine is not installed")
            text: qsTr("Recording works now; to speak with your voice, install the voice engine (about 6 GB, shared with Studio's dubbing).")
            LingmoUI.ActionButton { text: qsTr("Models"); onClicked: page.openModels() }
        }
        LingmoUI.Banner {
            Layout.fillWidth: true
            type: LingmoUI.Banner.Error
            text: translator.error
            closable: true
            onClosed: translator.clearError()
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: LingmoUI.Units.smallSpacing
            boundsBehavior: Flickable.StopAtBounds
            model: translator.profiles
            ScrollBar.vertical: ScrollBar {}
            LingmoUI.WheelHandler { target: list }
            delegate: LingmoUI.Card {
                id: card
                width: ListView.view.width
                hoverEnabled: true
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: card.padding
                    anchors.rightMargin: card.padding
                    anchors.topMargin: LingmoUI.Units.largeSpacing
                    anchors.bottomMargin: LingmoUI.Units.largeSpacing
                    spacing: LingmoUI.Units.largeSpacing
                    LingmoUI.IconItem {
                        Layout.preferredWidth: LingmoUI.Units.iconSizes.medium
                        Layout.preferredHeight: LingmoUI.Units.iconSizes.medium
                        source: "audio-input-microphone"
                        color: LingmoUI.Theme.purpleColor
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label { text: modelData.name; font.pointSize: LingmoUI.Units.fontSubheading; font.bold: true }
                        Label {
                            color: LingmoUI.Theme.secondaryTextColor
                            font.pointSize: LingmoUI.Units.fontCaption
                            text: [modelData.created ? new Date(modelData.created).toLocaleDateString(Qt.locale(), Locale.ShortFormat) : "",
                                   modelData.seconds ? qsTr("%1 s").arg(Math.round(modelData.seconds)) : "",
                                   modelData.snrDb ? qsTr("noise margin %1 dB").arg(Math.round(modelData.snrDb)) : ""].filter(s => s.length).join(" · ")
                        }
                    }
                    LingmoUI.Badge {
                        text: modelData.ready ? qsTr("Ready") : (translator.preparing ? qsTr("Preparing…") : qsTr("Not ready"))
                        tint: modelData.ready ? LingmoUI.Theme.greenColor : LingmoUI.Theme.orangeColor
                    }
                    LingmoUI.ActionButton {
                        visible: !modelData.ready && setup.voiceEngineReady && !translator.preparing
                        text: qsTr("Prepare")
                        onClicked: translator.saveProfile(modelData.name)
                    }
                    LingmoUI.ActionButton {
                        text: translator.voice === "profile:" + modelData.name ? qsTr("In use") : qsTr("Use")
                        enabled: modelData.ready && translator.voice !== "profile:" + modelData.name
                        onClicked: translator.voice = "profile:" + modelData.name
                    }
                    LingmoUI.IconButton { iconName: "media-record"; tipText: qsTr("Record again"); onClicked: recordDialog.openFor(modelData.name) }
                    LingmoUI.IconButton { iconName: "edit-delete"; tipText: qsTr("Delete"); onClicked: { deleteDialog.name = modelData.name; deleteDialog.open() } }
                }
            }
            LingmoUI.EmptyState {
                anchors.centerIn: parent
                visible: list.count === 0
                iconName: "audio-input-microphone"
                title: qsTr("No voice profile yet")
                text: qsTr("Read a short text aloud for about 25 seconds. Only your own voice can be cloned, and it stays on this computer.")
                actionText: qsTr("Record new profile")
                actionPrimary: true
                onActionTriggered: recordDialog.openFor("")
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: LingmoUI.Theme.secondaryTextColor
            font.pointSize: LingmoUI.Units.fontCaption
            text: qsTr("Profiles are kept in ~/.local/share/lingmo/voices, readable only by you. Nothing is uploaded.")
        }
    }

    RecordDialog { id: recordDialog }

    Dialog {
        id: deleteDialog
        property string name
        anchors.centerIn: parent
        modal: true
        title: qsTr("Delete “%1”?").arg(name)
        standardButtons: Dialog.Cancel | Dialog.Ok
        Label { text: qsTr("The recording and the prepared voice are deleted.") }
        onAccepted: translator.deleteProfile(name)
    }

    Dialog {
        id: importDialog
        anchors.centerIn: parent
        modal: true
        width: Math.min(page.width - LingmoUI.Units.largeSpacing * 4, 480)
        title: qsTr("Import a recording of your voice")
        property string file
        standardButtons: Dialog.Cancel | Dialog.Ok
        onOpened: { mine.checked = false; importName.text = ""; file = "" }
        ColumnLayout {
            anchors.fill: parent
            spacing: LingmoUI.Units.largeSpacing
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("20 to 30 seconds of you speaking alone, without music. Cloning someone else's voice is not allowed.")
            }
            TextField { id: importName; Layout.fillWidth: true; placeholderText: qsTr("Profile name") }
            RowLayout {
                Layout.fillWidth: true
                Label { Layout.fillWidth: true; elide: Text.ElideMiddle; text: importDialog.file.length ? importDialog.file : qsTr("No file chosen") }
                LingmoUI.ActionButton { text: qsTr("Choose…"); onClicked: fileDialog.open() }
            }
            CheckBox { id: mine; text: qsTr("This is a recording of my own voice") }
        }
        Component.onCompleted: standardButton(Dialog.Ok).enabled = Qt.binding(() => mine.checked && importName.text.trim().length > 0 && file.length > 0)
        onAccepted: if (!translator.importProfile(importName.text, file, mine.checked)) translator.clearError()
    }
    FileDialog {
        id: fileDialog
        nameFilters: [qsTr("Audio (*.wav *.mp3 *.ogg *.opus *.m4a *.flac)")]
        onAccepted: importDialog.file = selectedFile.toString()
    }
}

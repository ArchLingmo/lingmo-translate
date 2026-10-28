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

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: LingmoUI.Units.largeSpacing * 2
        anchors.rightMargin: LingmoUI.Units.largeSpacing * 2
        anchors.bottomMargin: LingmoUI.Units.largeSpacing * 2
        spacing: LingmoUI.Units.largeSpacing

        LingmoUI.PageHeader {
            Layout.fillWidth: true
            title: qsTr("Models")
            subtitle: qsTr("Downloaded once, used offline")
            reserveWindowControls: true
            // what is needed, with the speech models recommended for this computer
            LingmoUI.ActionButton {
                text: setup.pendingSize === "" ? qsTr("Ready") : qsTr("Download what is needed (%1)").arg(setup.pendingSize)
                primary: setup.pendingSize !== ""
                enabled: setup.pendingSize !== "" && !setup.busy
                busy: setup.busy
                onClicked: setup.install([])
            }
        }

        LingmoUI.Card {
            id: progressCard
            Layout.fillWidth: true
            visible: setup.busy || setup.failed
            ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: progressCard.padding
                anchors.rightMargin: progressCard.padding
                anchors.topMargin: LingmoUI.Units.largeSpacing
                anchors.bottomMargin: LingmoUI.Units.largeSpacing
                RowLayout {
                    Layout.fillWidth: true
                    Label { Layout.fillWidth: true; text: setup.message; wrapMode: Text.WordWrap; color: setup.failed ? LingmoUI.Theme.errorColor : LingmoUI.Theme.textColor }
                    LingmoUI.ActionButton { visible: setup.busy; text: qsTr("Cancel"); onClicked: setup.cancel() }
                }
                ProgressBar {
                    Layout.fillWidth: true
                    visible: setup.busy
                    indeterminate: setup.progress < 0
                    value: Math.max(0, setup.progress)
                }
            }
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: LingmoUI.Units.smallSpacing
            boundsBehavior: Flickable.StopAtBounds
            model: setup.components
            ScrollBar.vertical: ScrollBar {}
            delegate: LingmoUI.Card {
                id: card
                width: ListView.view.width
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: card.padding
                    anchors.rightMargin: card.padding
                    anchors.topMargin: LingmoUI.Units.largeSpacing
                    anchors.bottomMargin: LingmoUI.Units.largeSpacing
                    spacing: LingmoUI.Units.largeSpacing
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label { text: modelData.title; font.pointSize: LingmoUI.Units.fontBody; font.bold: true; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                        Label {
                            text: qsTr("%1 · licence %2").arg(modelData.size).arg(modelData.license) + (modelData.required ? "" : " · " + qsTr("optional"))
                            color: LingmoUI.Theme.secondaryTextColor
                            font.pointSize: LingmoUI.Units.fontCaption
                        }
                    }
                    LingmoUI.Badge {
                        visible: modelData.recommended === true
                        text: qsTr("Recommended for this computer")
                        tint: LingmoUI.Theme.highlightColor
                        subtle: true
                    }
                    LingmoUI.Badge {
                        visible: modelData.installed
                        text: qsTr("Installed")
                        tint: LingmoUI.Theme.greenColor
                        iconName: "emblem-ok-symbolic"
                    }
                    LingmoUI.ActionButton {
                        visible: !modelData.installed
                        text: qsTr("Download")
                        enabled: !setup.busy
                        onClicked: setup.install([modelData.id])
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: LingmoUI.Theme.secondaryTextColor
            font.pointSize: LingmoUI.Units.fontCaption
            text: qsTr("Whisper (MIT) recognises speech; Opus-MT by the University of Helsinki (CC-BY 4.0) translates instantly; the optional AI refinement uses your Ollama model (qwen3.5, Apache-2.0); piper (MIT, CC0 voices) and Chatterbox (MIT) speak. Manage them also in Settings › AI.")
        }
    }
}

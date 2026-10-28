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
import QtQuick.Window
import LingmoUI.CompatibleModule 3.0 as LingmoUI

// The subtitle strip: Lingmo glass, always on top, click-through (clicks go to
// the video or the call under it). "Move subtitles" in the window unlocks it
// for dragging; the position is remembered.
Window {
    id: overlay

    property Item shell
    readonly property bool unlocked: shell ? shell.overlayUnlocked : false

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
           | (unlocked ? 0 : Qt.WindowTransparentForInput)
    color: "transparent"
    title: qsTr("Live subtitles")

    readonly property real scale: translator.overlayFontScale
    width: Math.min(Screen.width - LingmoUI.Units.largeSpacing * 4, Math.max(560, Screen.width * 0.6))
    height: strip.implicitHeight + micPill.height + LingmoUI.Units.smallSpacing * 2

    function place() {
        const p = translator.overlayPos
        if (p.x >= 0 && p.y >= 0 && p.x < Screen.width && p.y < Screen.height) {
            x = p.x
            y = p.y
        } else {
            x = Screen.virtualX + (Screen.width - width) / 2
            y = Screen.virtualY + Screen.height - height - Screen.height * 0.09
        }
    }
    onVisibleChanged: if (visible) place()
    Component.onCompleted: place()

    // the line being spoken, else the last finished one
    readonly property var lastLine: translator.lines.length > 0 ? translator.lines[translator.lines.length - 1] : null
    readonly property string mainText: translator.liveText.length > 0 ? translator.liveText
                                      : (lastLine ? lastLine.text : (translator.starting ? translator.status : qsTr("Listening…")))
    readonly property string originalText: translator.liveText.length > 0 ? translator.liveOriginal : (lastLine ? lastLine.original : "")
    readonly property bool isDraft: translator.liveText.length > 0 || (lastLine ? lastLine.draft : false)

    // the visible indicator that the translator microphone is on (it speaks for the user)
    Rectangle {
        id: micPill
        visible: translator.virtualMicActive
        height: visible ? micRow.implicitHeight + LingmoUI.Units.smallSpacing : 0
        width: micRow.implicitWidth + LingmoUI.Units.largeSpacing * 1.5
        radius: height / 2
        color: LingmoUI.Theme.redColor
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        RowLayout {
            id: micRow
            anchors.centerIn: parent
            spacing: LingmoUI.Units.smallSpacing
            Rectangle {
                width: 8; height: 8; radius: 4
                color: LingmoUI.Theme.overlayTextColor
                SequentialAnimation on opacity {
                    running: micPill.visible && !LingmoUI.Theme.reduceMotion
                    loops: Animation.Infinite
                    NumberAnimation { to: 0.3; duration: 700 }
                    NumberAnimation { to: 1.0; duration: 700 }
                }
            }
            Label {
                text: translator.paused ? qsTr("Translator microphone paused") : qsTr("Translator microphone on")
                color: LingmoUI.Theme.overlayTextColor
                font.pointSize: LingmoUI.Units.fontCaption
                font.bold: true
            }
        }
    }

    LingmoUI.GlassSurface {
        id: strip
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        radius: LingmoUI.Theme.hugeRadius
        elevated: true
        implicitHeight: column.implicitHeight + LingmoUI.Units.largeSpacing * 1.5

        ColumnLayout {
            id: column
            anchors.fill: parent
            anchors.leftMargin: LingmoUI.Units.largeSpacing * 1.5
            anchors.rightMargin: LingmoUI.Units.largeSpacing * 1.5
            anchors.topMargin: LingmoUI.Units.largeSpacing * 0.75
            anchors.bottomMargin: LingmoUI.Units.largeSpacing * 0.75
            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                spacing: LingmoUI.Units.smallSpacing
                visible: translator.showOriginal && overlay.originalText.length > 0
                LingmoUI.Badge {
                    text: translator.pairLabel
                    subtle: true
                }
                Label {
                    Layout.fillWidth: true
                    text: overlay.originalText
                    color: LingmoUI.Theme.secondaryTextColor
                    font.pointSize: LingmoUI.Units.fontCaption * overlay.scale
                    elide: Text.ElideLeft
                    maximumLineCount: 1
                }
            }
            Label {
                id: mainLabel
                Layout.fillWidth: true
                text: overlay.mainText
                color: LingmoUI.Theme.textColor
                opacity: overlay.isDraft ? 0.82 : 1.0
                font.pointSize: LingmoUI.Units.fontHeading * 1.25 * overlay.scale
                font.weight: Font.DemiBold
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideLeft
                horizontalAlignment: Text.AlignHCenter
                Behavior on opacity { NumberAnimation { duration: LingmoUI.Units.shortDuration } }
            }
        }

        // unlocked: drag anywhere on the strip to move it
        Rectangle {
            anchors.fill: parent
            radius: strip.radius
            color: "transparent"
            border.width: overlay.unlocked ? 2 : 0
            border.color: LingmoUI.Theme.highlightColor
            visible: overlay.unlocked
            Label {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: LingmoUI.Units.smallSpacing
                text: qsTr("Drag to move")
                font.pointSize: LingmoUI.Units.fontCaption
                color: LingmoUI.Theme.highlightColor
            }
        }
        DragHandler {
            enabled: overlay.unlocked
            target: null
            onActiveChanged: if (active) overlay.startSystemMove()
        }
    }

    onXChanged: savePos.restart()
    onYChanged: savePos.restart()
    Timer {
        id: savePos
        interval: 600
        onTriggered: if (overlay.unlocked) translator.overlayPos = Qt.point(overlay.x, overlay.y)
    }
}

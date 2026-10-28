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

LingmoUI.Window {
    id: rootWindow
    width: 980
    height: 680
    minimumWidth: 820
    minimumHeight: 560
    title: qsTr("Live translation")
    visible: false

    property Item shell

    background.opacity: LingmoUI.Theme.darkMode ? 0.88 : 0.97
    header.height: LingmoUI.Units.windowHeaderHeight
    contentTopMargin: 0

    LingmoUI.WindowBlur {
        view: rootWindow
        geometry: Qt.rect(rootWindow.x, rootWindow.y, rootWindow.width, rootWindow.height)
        windowRadius: rootWindow.windowRadius
        enabled: true
    }

    function openPage(name) {
        if (name === "record") {
            sideBar.currentIndex = 1
            voicesPage.record()
            return
        }
        const i = ["translate", "voices", "models"].indexOf(name)
        if (i >= 0)
            sideBar.currentIndex = i
    }

    Component.onCompleted: if (!setup.ready) sideBar.currentIndex = 2

    RowLayout {
        anchors.fill: parent
        spacing: 0

        LingmoUI.SettingsSideBar {
            id: sideBar
            Layout.fillHeight: true
            iconSource: "qrc:/icons/lingmo-translate.svg"
            model: ListModel {
                ListElement { title: qsTr("Translate"); iconName: "preferences-desktop-locale"; iconColor: "#0A84FF"; badge: "" }
                ListElement { title: qsTr("Voice profiles"); iconName: "audio-input-microphone"; iconColor: "#BF5AF2"; badge: "" }
                ListElement { title: qsTr("Models"); iconName: "folder-download"; iconColor: "#30D158"; badge: "" }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: sideBar.currentIndex

            TranslatePage { shell: rootWindow.shell; onOpenModels: sideBar.currentIndex = 2; onOpenVoices: sideBar.currentIndex = 1 }
            VoicesPage { id: voicesPage; onOpenModels: sideBar.currentIndex = 2 }
            ModelsPage {}
        }
    }
}

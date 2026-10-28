cmake_minimum_required(VERSION 3.16)
project(lingmo-translate VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 DBus Network LinguistTools)

set(ENGINE_DIR ${CMAKE_INSTALL_FULL_DATADIR}/lingmo-translate/engine)
set(TRANSLATIONS_DIR ${CMAKE_INSTALL_FULL_DATADIR}/lingmo-translate/translations)

add_executable(lingmo-translate
    src/main.cpp
    src/paths.cpp src/paths.h
    src/setup.cpp src/setup.h
    src/livetranslator.cpp src/livetranslator.h
    src/virtualmic.cpp src/virtualmic.h
    src/whisperhw.cpp src/whisperhw.h
    src/hwprofile.cpp src/hwprofile.h
    qml.qrc
)
target_compile_definitions(lingmo-translate PRIVATE
    ENGINE_DIR="${ENGINE_DIR}"
    SOURCE_ENGINE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/engine"
    TRANSLATIONS_DIR="${TRANSLATIONS_DIR}")
target_link_libraries(lingmo-translate PRIVATE
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::DBus Qt6::Network)

# Lingmo Intents: translate.live.start / translate.live.stop on com.lingmo.Translate /Intents
find_package(LingmoIntents 1.0 QUIET)
if(LingmoIntents_FOUND)
    target_sources(lingmo-translate PRIVATE src/translateintents.cpp src/translateintents.h)
    target_compile_definitions(lingmo-translate PRIVATE HAVE_LINGMO_INTENTS)
    target_link_libraries(lingmo-translate PRIVATE LingmoIntents::LingmoIntents)
    lingmo_install_intents(MANIFEST data/intents/lingmo-translate.json SERVICE data/com.lingmo.Translate.service)
else()
    message(STATUS "LingmoIntents not found: built without app actions (intents)")
    install(FILES data/com.lingmo.Translate.service DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
endif()

qt_add_translation(QM_FILES translations/lingmo-translate_pt_BR.ts)
add_custom_target(translations ALL DEPENDS ${QM_FILES})

install(TARGETS lingmo-translate RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${QM_FILES} DESTINATION ${TRANSLATIONS_DIR})
install(PROGRAMS data/engine/engine.py data/engine/clone_worker.py DESTINATION ${ENGINE_DIR})
install(FILES lingmo-translate.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
# the "Lingmo Tradutor" microphone at login (pactl: PulseAudio and pipewire-pulse alike)
install(FILES data/lingmo-translate-mic.desktop DESTINATION /etc/xdg/autostart)
install(FILES icons/lingmo-translate.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

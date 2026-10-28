#pragma once

class LiveTranslator;
class QObject;

// Lingmo Intents on com.lingmo.Translate /Intents: translate.live.start and
// translate.live.stop (data/intents/lingmo-translate.json).
void publishTranslateIntents(LiveTranslator *translator, QObject *parent);

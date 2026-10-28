/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

#include "translateintents.h"
#include "livetranslator.h"

#include <LingmoIntents/intentservice.h>

using LingmoIntents::IntentCall;
using LingmoIntents::IntentResult;

void publishTranslateIntents(LiveTranslator *translator, QObject *parent)
{
    auto *intents = new LingmoIntents::IntentService(QStringLiteral("lingmo-translate"), parent);
    intents->handle(QStringLiteral("translate.live.start"), [translator](const IntentCall &call) {
        const QVariantMap p = call.params();
        // only listening modes: speaking for the user always starts from the window
        translator->setMode(p.value(QStringLiteral("mode"), QStringLiteral("subtitles")).toString());
        translator->setSource(p.value(QStringLiteral("source"), QStringLiteral("system")).toString());
        if (!p.value(QStringLiteral("app")).toString().isEmpty())
            translator->setSourceApp(p.value(QStringLiteral("app")).toString());
        translator->setFromLang(p.value(QStringLiteral("from"), QStringLiteral("auto")).toString());
        translator->setToLang(p.value(QStringLiteral("to"), QStringLiteral("pt")).toString());
        if (translator->running())
            translator->stop();
        translator->start();
        if (!translator->error().isEmpty())
            return IntentResult::failed(translator->error());
        return IntentResult::ok({{QStringLiteral("mode"), translator->mode()}});
    });
    intents->handle(QStringLiteral("translate.live.stop"), [translator](const IntentCall &) {
        translator->stop();
        return IntentResult::ok();
    });
    intents->publish(); // /Intents next to /Translate on com.lingmo.Translate
}

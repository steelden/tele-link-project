/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.
*/
#include "mtslink/lang_overrides.h"

#include "lang/lang_instance.h"

#include <QtCore/QFile>

namespace MtsLink {

void applyLangOverrides() {
	auto &instance = Lang::GetInstance();
	const auto langId = instance.id();
	LOG(("MtsLink Lang: id='%1'").arg(langId));

	if (!langId.startsWith(u"ru"_q)) {
		return;
	}

	QFile file(u":/langs/ru.strings"_q);
	if (!file.open(QIODevice::ReadOnly)) {
		LOG(("MtsLink: ru.strings resource not found"));
		return;
	}
	const auto content = file.readAll();
	if (content.isEmpty()) {
		return;
	}

	instance.loadFromContent(content);
	instance.notifyUpdated();
	LOG(("MtsLink: Applied Russian localization (%1 bytes)")
		.arg(content.size()));
}

} // namespace MtsLink

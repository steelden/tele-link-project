/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.
*/
#include "mtslink/lang_overrides.h"

#include "lang/lang_instance.h"

namespace MtsLink {
namespace {

struct LangOverride {
	const char *lang;
	const char *key;
	const char *value;
};

const LangOverride kOverrides[] = {
	{ "ru", "lng_saved_messages", "\xd0\x98\xd0\xb7\xd0\xb1\xd1\x80\xd0\xb0\xd0\xbd\xd0\xbd\xd0\xbe\xd0\xb5" },
};

} // namespace

void applyLangOverrides() {
	auto &instance = Lang::GetInstance();
	const auto langId = instance.id();

	for (const auto &o : kOverrides) {
		if (langId.startsWith(QLatin1String(o.lang))) {
			instance.overrideValue(
				QByteArray(o.key),
				QByteArray(o.value));
		}
	}
}

} // namespace MtsLink

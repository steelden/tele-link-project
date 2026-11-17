/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.
*/
#include "mtslink/whats_new.h"

#include "mtslink/call_window.h"
#include "core/application.h"
#include "core/version.h"
#include "iv/iv_instance.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonObject>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace MtsLink {
namespace {

constexpr auto kPendingKey = "whatsNewPending";
constexpr auto kShownKey = "whatsNewShown";

// WHATSNEW.md in the root of the (public) repository, by the release tag
// of this version: the history of the versions up to it.
constexpr auto kUrlBase
	= "https://raw.githubusercontent.com/steelden/tele-link-project/";

[[nodiscard]] QString VersionTag() {
	return u"v"_q + QString::fromLatin1(AppVersionStr);
}

void SetSetting(const char *key, int value) {
	auto settings = readLocalSettings();
	settings.insert(QString::fromLatin1(key), value);
	writeLocalSettings(settings);
}

[[nodiscard]] int Setting(const char *key) {
	return readLocalSettings().value(QString::fromLatin1(key)).toInt();
}

void ShowFile(const QByteArray &bytes) {
	const auto folder = cWorkingDir() + u"tdata/whatsnew/"_q;
	QDir().mkpath(folder);
	// The file name is the viewer title.
	const auto path = folder + u"Что нового в TeleLink.md"_q;
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly) || file.write(bytes) < 0) {
		LOG(("MtsLink WhatsNew: can't write '%1'").arg(path));
		return;
	}
	file.close();
	if (Core::App().iv().showMarkdown(path)) {
		LOG(("MtsLink WhatsNew: shown %1").arg(VersionTag()));
		SetSetting(kShownKey, AppVersion);
	} else {
		LOG(("MtsLink WhatsNew: the viewer failed for '%1'").arg(path));
	}
}

} // namespace

void rememberWhatsNewAfterUpdate(int oldVersion) {
	if (oldVersion > 0 && oldVersion < AppVersion) {
		LOG(("MtsLink WhatsNew: updated from %1 to %2"
			).arg(oldVersion
			).arg(AppVersion));
		SetSetting(kPendingKey, AppVersion);
	}
}

void showWhatsNewIfPending() {
	static auto requested = false;
	if (requested
		|| Setting(kPendingKey) != AppVersion
		|| Setting(kShownKey) >= AppVersion) {
		return;
	}
	requested = true;

	static const auto manager = new QNetworkAccessManager();
	const auto url = QString::fromLatin1(kUrlBase)
		+ VersionTag()
		+ u"/WHATSNEW.md"_q;
	LOG(("MtsLink WhatsNew: loading %1").arg(url));
	auto request = QNetworkRequest(QUrl(url));
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	const auto reply = manager->get(request);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (status == 404) {
			// Nothing written for this version: not asked again.
			LOG(("MtsLink WhatsNew: no notes for %1").arg(VersionTag()));
			SetSetting(kShownKey, AppVersion);
			return;
		} else if (reply->error() != QNetworkReply::NoError) {
			// Asked again with the next start.
			LOG(("MtsLink WhatsNew: failed to load %1: %2 (%3)"
				).arg(url
				).arg(reply->errorString()
				).arg(status));
			return;
		}
		const auto bytes = reply->readAll();
		if (bytes.trimmed().isEmpty()) {
			SetSetting(kShownKey, AppVersion);
			return;
		}
		ShowFile(bytes);
	});
}

} // namespace MtsLink

/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.
*/
#include "mtslink/whats_new.h"

#include "mtslink/call_window.h"
#include "core/application.h"
#include "core/version.h"
#include "iv/iv_instance.h"
#include "window/window_controller.h"
#include "mainwindow.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
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

enum class LoadResult {
	Shown,
	NotFound,
	Failed,
};

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

bool ShowFile(const QByteArray &bytes) {
	const auto folder = cWorkingDir() + u"tdata/whatsnew/"_q;
	QDir().mkpath(folder);
	// The file name is the viewer title.
	const auto path = folder + u"Что нового в TeleLink.md"_q;
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly) || file.write(bytes) < 0) {
		LOG(("MtsLink WhatsNew: can't write '%1'").arg(path));
		return false;
	}
	file.close();
	// Above the main window (was hidden under it after a click there).
	const auto primary = Core::App().activePrimaryWindow();
	const auto owner = primary
		? static_cast<QWidget*>(primary->widget().get())
		: nullptr;
	if (!Core::App().iv().showMarkdown(path, QVariant(), owner)) {
		LOG(("MtsLink WhatsNew: the viewer failed for '%1'").arg(path));
		return false;
	}
	LOG(("MtsLink WhatsNew: shown %1").arg(VersionTag()));
	return true;
}

void Load(Fn<void(LoadResult)> done) {
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
			LOG(("MtsLink WhatsNew: no notes for %1").arg(VersionTag()));
			done(LoadResult::NotFound);
			return;
		} else if (reply->error() != QNetworkReply::NoError) {
			LOG(("MtsLink WhatsNew: failed to load %1: %2 (%3)"
				).arg(url
				).arg(reply->errorString()
				).arg(status));
			done(LoadResult::Failed);
			return;
		}
		const auto bytes = reply->readAll();
		if (bytes.trimmed().isEmpty()) {
			done(LoadResult::NotFound);
		} else {
			done(ShowFile(bytes) ? LoadResult::Shown : LoadResult::Failed);
		}
	});
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
	Load([](LoadResult result) {
		// Nothing written for this version: not asked again. A network
		// error: asked again with the next start.
		if (result != LoadResult::Failed) {
			SetSetting(kShownKey, AppVersion);
		}
	});
}

void showWhatsNew(Fn<void(QString)> fail) {
	static auto loading = false;
	if (loading) {
		return;
	}
	loading = true;
	Load([=](LoadResult result) {
		loading = false;
		if (result == LoadResult::Shown) {
			// Seen: not shown again after the start.
			SetSetting(kShownKey, AppVersion);
		} else if (fail) {
			fail((result == LoadResult::NotFound)
				? (u"Нет описания изменений для версии "_q
					+ QString::fromLatin1(AppVersionStr))
				: u"Не удалось загрузить описание изменений"_q);
		}
	});
}

} // namespace MtsLink

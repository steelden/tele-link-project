/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#include "mtslink/env_config.h"

#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QDir>
#include <QtCore/QFile>

namespace MtsLink {

EnvConfig::EnvConfig() : QObject(nullptr) {
}

EnvConfig &EnvConfig::instance() {
	static EnvConfig config;
	return config;
}

void EnvConfig::load(const QString &envConfigUrl) {
	auto *manager = new QNetworkAccessManager(this);
	auto request = QNetworkRequest(QUrl(envConfigUrl));
	auto *reply = manager->get(request);
	QObject::connect(reply, &QNetworkReply::finished, this, [=] {
		reply->deleteLater();
		manager->deleteLater();
		if (reply->error() != QNetworkReply::NoError) {
			LOG(("MtsLink EnvConfig: load failed: %1")
				.arg(reply->errorString()));
			if (loadFromCache()) {
				LOG(("MtsLink EnvConfig: using cached config"));
				Q_EMIT loaded();
			} else {
				Q_EMIT loadFailed();
			}
			return;
		}
		const auto data = reply->readAll();
		parse(data);
		saveToCache(data);
	});
}

void EnvConfig::parse(const QByteArray &data) {
	const auto js = QString::fromUtf8(data);

	const auto start = js.indexOf('{');
	const auto end = js.lastIndexOf('}');
	if (start < 0 || end < 0 || end <= start) {
		LOG(("MtsLink EnvConfig: failed to find JSON object"));
		_loaded = true;
		Q_EMIT loaded();
		return;
	}

	const auto jsonStr = js.mid(start, end - start + 1);
	const auto doc = QJsonDocument::fromJson(jsonStr.toUtf8());
	if (doc.isNull() || !doc.isObject()) {
		LOG(("MtsLink EnvConfig: failed to parse JSON"));
		_loaded = true;
		Q_EMIT loaded();
		return;
	}

	const auto obj = doc.object();
	_wssServerUrl = obj.value("WSS_SERVER_URL").toString();
	_httpsServerUrl = obj.value("HTTPS_SERVER_URL").toString();
	_baseMediaUrl = obj.value("BASE_MEDIA_URL").toString();
	_publicCdnMediaUrl = obj.value("PUBLIC_CDN_MEDIA_URL").toString();
	_webinarHost = obj.value("WEBINAR_HOST").toString();

	LOG(("MtsLink EnvConfig: loaded"
		"\n  wss=%1"
		"\n  https=%2"
		"\n  media=%3"
		"\n  cdn=%4"
		"\n  host=%5")
		.arg(_wssServerUrl)
		.arg(_httpsServerUrl)
		.arg(_baseMediaUrl)
		.arg(_publicCdnMediaUrl)
		.arg(_webinarHost));

	_loaded = true;
	Q_EMIT loaded();
}

void EnvConfig::setCachePath(const QString &path) {
	_cachePath = path;
}

void EnvConfig::saveToCache(const QByteArray &data) {
	if (_cachePath.isEmpty()) {
		return;
	}
	QFile file(_cachePath);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(data);
		LOG(("MtsLink EnvConfig: saved to cache '%1'").arg(_cachePath));
	}
}

bool EnvConfig::loadFromCache() {
	if (_cachePath.isEmpty()) {
		return false;
	}
	QFile file(_cachePath);
	if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
		return false;
	}
	const auto data = file.readAll();
	if (data.isEmpty()) {
		return false;
	}
	parse(data);
	return _loaded;
}

QString EnvConfig::wssServerUrl() const {
	return _wssServerUrl.isEmpty()
		? u"wss://prod-ws-chat.mts-link.ru/v1"_q
		: _wssServerUrl;
}

QString EnvConfig::httpsServerUrl() const {
	return _httpsServerUrl.isEmpty()
		? u"https://gw.mts-link.ru"_q
		: _httpsServerUrl;
}

QString EnvConfig::baseMediaUrl() const {
	return _baseMediaUrl.isEmpty()
		? u"https://prod-storage-chat.mts-link.ru"_q
		: _baseMediaUrl;
}

QString EnvConfig::publicCdnMediaUrl() const {
	return _publicCdnMediaUrl.isEmpty()
		? u"https://prod-cdn-thumb-public-chat.mts-link.ru"_q
		: _publicCdnMediaUrl;
}

QString EnvConfig::webinarHost() const {
	return _webinarHost.isEmpty()
		? u"https://my.mts-link.ru"_q
		: _webinarHost;
}

bool EnvConfig::isLoaded() const {
	return _loaded;
}

} // namespace MtsLink

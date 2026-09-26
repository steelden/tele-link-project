/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#include "mtslink/api/api_auth.h"
#include "mtslink/data_adapters.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkCookie>
#include <QtNetwork/QNetworkCookieJar>
#include <QUuid>

namespace MtsLink::Api {

const QString Auth::kGatewayUrl = "https://gw.mts-link.ru";

Auth::Auth(QObject *parent)
: QObject(parent)
, _deviceId(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
}

Auth::~Auth() = default;

void Auth::getLoginOrganizations(const QString &email) {
	postJson(
		"/ssoExternal/ExternalSSO.GetLoginOrganizationsByEmail",
		QJsonObject{{"email", email}},
		[this](const QJsonObject &response) {
			QList<OrganizationInfo> orgs;
			const auto value = response.value("value").toObject();
			const auto list = value.value("organizations").toArray();
			for (const auto &item : list) {
				const auto obj = item.toObject();
				orgs.push_back({
					.id = obj.value("id").toString(),
					.name = obj.value("name").toString(),
					.domain = obj.value("domain").toString(),
					.ssoLoginUrl = obj.value("ssoLoginUrl").toString(),
				});
			}
			Q_EMIT organizationsReceived(orgs);
		},
		[this](const QString &error) {
			Q_EMIT authFailed(error);
		});
}

void Auth::loginByAuthCode(const QString &authCode) {
	postJson(
		"/accountUcaas/AccountUcaas.LoginByAuthCode",
		QJsonObject{{"authCode", authCode}},
		[this](const QJsonObject &response) {
			const auto value = response.value("value").toObject();
			_accessToken = value.value("accessToken").toString();
			const auto refreshToken = value.value("refreshToken").toString();

			if (_accessToken.isEmpty()) {
				Q_EMIT authFailed("No access token in LoginByAuthCode response");
				return;
			}

			const auto parts = _accessToken.split('.');
			if (parts.size() >= 2) {
				auto payload = QByteArray::fromBase64(
					parts[1].toUtf8(),
					QByteArray::Base64UrlEncoding
						| QByteArray::OmitTrailingEquals);
				const auto jwt = QJsonDocument::fromJson(payload).object();
				_userId = jwt.value("uid").toString();
				_clientId = jwt.value("cid").toString();
				LOG(("MtsLink Auth: userId=%1 clientId=%2")
					.arg(_userId).arg(_clientId));
			}

			Q_EMIT authSuccess({
				.accessToken = _accessToken,
				.userId = _userId,
				.clientId = _clientId,
			});
			Q_EMIT tokenRefreshed(_accessToken);
		},
		[this](const QString &error) {
			Q_EMIT authFailed("LoginByAuthCode failed: " + error);
		});
}

void Auth::refreshTokens() {
	QJsonObject clientMeta;
	clientMeta["appVersion"] = "1.0.0";
	clientMeta["platform"] = "Desktop";
	clientMeta["device"] = "MtsLinkDesktop";
	clientMeta["os"] =
#ifdef Q_OS_WIN
		"Windows";
#elif defined(Q_OS_MACOS)
		"macOS";
#else
		"Linux";
#endif

	postJson(
		"/Account/RefreshUserTokensV2",
		QJsonObject{{"clientMeta", clientMeta}},
		[this](const QJsonObject &response) {
			const auto value = response.value("value").toObject();
			_accessToken = value.value("accessToken").toString();
			_userId = value.value("userId").toString();
			_clientId = value.value("clientId").toString();
			const auto email = value.value("userEmail").toString();

			if (_accessToken.isEmpty()) {
				Q_EMIT authFailed("No access token in response");
				return;
			}

			Q_EMIT authSuccess({
				.accessToken = _accessToken,
				.userId = _userId,
				.clientId = _clientId,
				.userEmail = email,
			});
			Q_EMIT tokenRefreshed(_accessToken);
		},
		[this](const QString &error) {
			Q_EMIT authFailed("RefreshTokens failed: " + error);
		});
}

bool Auth::hasToken() const {
	return !_accessToken.isEmpty();
}

QString Auth::accessToken() const {
	return _accessToken;
}

UserId Auth::userId() const {
	return _userId;
}

ClientId Auth::clientId() const {
	return _clientId;
}

void Auth::setDeviceId(const QString &deviceId) {
	_deviceId = deviceId;
}

void Auth::postJson(
		const QString &path,
		const QJsonObject &body,
		std::function<void(const QJsonObject &)> done,
		std::function<void(const QString &)> fail) {
	QNetworkRequest request(QUrl(kGatewayUrl + path));
	request.setHeader(
		QNetworkRequest::ContentTypeHeader,
		"application/json");
	request.setRawHeader("X-Platform", "Desktop");
	request.setRawHeader("X-App-Version", "1.0.0");
	request.setRawHeader("X-Device", "MtsLinkDesktop");
	request.setRawHeader("X-Device-Id", _deviceId.toUtf8());
#ifdef Q_OS_WIN
	request.setRawHeader("X-Os", "Windows");
#elif defined(Q_OS_MACOS)
	request.setRawHeader("X-Os", "macOS");
#else
	request.setRawHeader("X-Os", "Linux");
#endif

	LOG(("MtsLink Auth: POST %1").arg(path));

	auto *reply = _network.post(
		request,
		QJsonDocument(body).toJson(QJsonDocument::Compact));

	QObject::connect(reply, &QNetworkReply::finished, this,
		[this, reply, done, fail, path]() {
			reply->deleteLater();
			const auto statusCode = reply->attribute(
				QNetworkRequest::HttpStatusCodeAttribute).toInt();
			LOG(("MtsLink Auth: %1 status=%2")
				.arg(path).arg(statusCode));

			const auto cookies = reply->header(
				QNetworkRequest::SetCookieHeader)
				.value<QList<QNetworkCookie>>();
			for (const auto &cookie : cookies) {
				LOG(("MtsLink Auth: Set-Cookie: %1=%2 domain=%3 path=%4")
					.arg(QString::fromUtf8(cookie.name()))
					.arg(QString::fromUtf8(cookie.value().left(20)) + "...")
					.arg(cookie.domain())
					.arg(cookie.path()));
			}
			if (!cookies.isEmpty()) {
				MtsLink::setFileAuthCookies(cookies);
			}

			if (reply->error() != QNetworkReply::NoError) {
				const auto body = reply->readAll();
				LOG(("MtsLink Auth: error body: %1")
					.arg(QString::fromUtf8(body.left(500))));
				if (fail) {
					fail(reply->errorString());
				}
				return;
			}
			const auto data = reply->readAll();
			const auto doc = QJsonDocument::fromJson(data);
			if (doc.isNull()) {
				if (fail) {
					fail("Invalid JSON response");
				}
				return;
			}
			if (done) {
				done(doc.object());
			}
		});
}

QJsonObject Auth::deviceHeaders() const {
	return QJsonObject{
		{"X-Platform", "Desktop"},
		{"X-App-Version", "1.0.0"},
		{"X-Device", "MtsLinkDesktop"},
		{"X-Device-Id", _deviceId},
	};
}

} // namespace MtsLink::Api

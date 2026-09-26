/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#pragma once

#include "mtslink/types.h"

#include <QObject>
#include <QJsonObject>
#include <QtNetwork/QNetworkAccessManager>

namespace MtsLink::Api {

struct AuthResult {
	QString accessToken;
	UserId userId;
	ClientId clientId;
	QString userEmail;
};

struct OrganizationInfo {
	OrganizationId id;
	QString name;
	QString domain;
	QString ssoLoginUrl;
};

class Auth final : public QObject {
	Q_OBJECT

public:
	explicit Auth(QObject *parent = nullptr);
	~Auth();

	void getLoginOrganizations(const QString &email);
	void loginByAuthCode(const QString &authCode);
	void refreshTokens();

	[[nodiscard]] bool hasToken() const;
	[[nodiscard]] QString accessToken() const;
	[[nodiscard]] UserId userId() const;
	[[nodiscard]] ClientId clientId() const;

	void setDeviceId(const QString &deviceId);

Q_SIGNALS:
	void organizationsReceived(const QList<OrganizationInfo> &orgs);
	void authSuccess(const AuthResult &result);
	void authFailed(const QString &error);
	void tokenRefreshed(const QString &newToken);

private:
	void postJson(
		const QString &path,
		const QJsonObject &body,
		std::function<void(const QJsonObject &)> done,
		std::function<void(const QString &)> fail = nullptr);

	[[nodiscard]] QJsonObject deviceHeaders() const;

	QNetworkAccessManager _network;
	QString _accessToken;
	UserId _userId;
	ClientId _clientId;
	QString _deviceId;

	static const QString kGatewayUrl;
};

} // namespace MtsLink::Api

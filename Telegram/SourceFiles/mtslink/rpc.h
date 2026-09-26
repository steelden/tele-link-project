/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#pragma once

#include "mtslink/connection.h"
#include "mtslink/types.h"

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <functional>

namespace MtsLink {

class Rpc final : public QObject {
	Q_OBJECT

public:
	using DoneHandler = std::function<void(const QJsonObject &result)>;
	using FailHandler = std::function<void(const QString &error)>;
	using EventHandler = std::function<void(const QJsonObject &event)>;

	explicit Rpc(QObject *parent = nullptr);
	~Rpc();

	void connectAndAuth(const QString &token);
	void disconnect();

	void call(
		const QString &method,
		const QJsonObject &param,
		DoneHandler done,
		FailHandler fail = nullptr);

	void subscribe(
		const QString &method,
		const QJsonObject &param,
		EventHandler handler);

	[[nodiscard]] bool isConnected() const;
	[[nodiscard]] QString clientId() const;

Q_SIGNALS:
	void connected();
	void disconnected();
	void eventReceived(
		const QString &name,
		const QString &dst,
		const QJsonObject &param);

private:
	void handleMessage(const QJsonObject &message);
	[[nodiscard]] QString generateId() const;

	Connection _connection;
	QHash<QString, DoneHandler> _pending;
	QHash<QString, EventHandler> _subscriptions;
};

} // namespace MtsLink

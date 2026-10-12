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
#include <QTimer>
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

	// A dead or stuck connection: dropped, the session reconnects and the
	// requests are sent again.
	void dropConnection(const QString &reason);

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
	[[nodiscard]] bool isConnecting() const;
	[[nodiscard]] QString clientId() const;

Q_SIGNALS:
	void connected();
	void disconnected();
	void authError(const QString &errorText);
	void eventReceived(
		const QString &name,
		const QString &dst,
		const QJsonObject &param);

private:
	void handleMessage(const QJsonObject &message);
	void cancelPending();
	[[nodiscard]] QString generateId() const;

	// The connection is unstable: the requests wait for it, are sent again
	// after a reconnect and when the answer doesn't come (rate limit).
	void sendCall(const QString &id);
	void sendWaiting();
	void requeuePending(const QString &reason);
	void checkTimeouts();
	void retryRateLimited();
	void failCalls(std::vector<QString> ids, const QString &error);

	Connection _connection;
	struct PendingCall {
		QString method;
		QJsonObject param;
		DoneHandler handler;
		FailHandler failHandler;
		int attempts = 0;
		crl::time sentAt = 0;
		crl::time deadline = 0; // Zero: waits for the connection.
	};
	QHash<QString, PendingCall> _pending;
	QHash<QString, EventHandler> _subscriptions;
	QTimer _timeoutTimer;
	// Different requests without an answer since the last one answered.
	QSet<QString> _timedOut;
};

} // namespace MtsLink

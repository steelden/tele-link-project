/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#include "mtslink/rpc.h"

#include <QtCore/QJsonDocument>

#include <QUuid>
#include <QSet>

namespace MtsLink {
namespace {

// No answer in this time: sent again (the server drops the requests above
// its rate limit without an answer).
constexpr auto kCallTimeout = crl::time(15000);
constexpr auto kMaxAttempts = 4;
// The requests sent just before "rateLimitIsReached": sent again soon.
constexpr auto kRateLimitedRetry = crl::time(2000);
// Different requests unanswered in a row on a "live" connection: stuck.
constexpr auto kTimeoutsToReconnect = 3;

// Bound to the connection (the session subscribes again after a reconnect)
// or not repeatable (would duplicate the result): not sent again after a
// reconnect. The messages are sent again by the session (by clientId).
[[nodiscard]] bool NoRetryAfterReconnect(const QString &method) {
	static const auto result = QSet<QString>{
		u"Organization.GetOrganizationsByUserV2"_q,
		u"Organization.Subscribe"_q,
		u"Counters.GetCounters"_q,
		u"Chat.Subscribe"_q,
		u"MessageScheduler.Subscribe"_q,
		u"Notification.Subscribe"_q,
		u"Chat.GetMyFrequentlyUsedEmojis"_q,
		u"Typing.Typing"_q,
		u"Chat.SendMessageV2"_q,
		u"Chat.ForwardMessage"_q,
		u"Chat.CopyMessage"_q,
		u"Chat.CreateGroupChatV2"_q,
		u"Chat.CreateChannelV2"_q,
		u"MessageScheduler.CreateMessageScheduled"_q,
		u"MessageScheduler.SendMessageScheduledNow"_q,
		u"WebinarApp.CreateCallV2"_q,
	};
	return result.contains(method);
}

// Would duplicate the result if the answer was lost: not sent again.
[[nodiscard]] bool NoRetryAfterTimeout(const QString &method) {
	static const auto result = QSet<QString>{
		u"Typing.Typing"_q,
		u"Chat.ForwardMessage"_q,
		u"Chat.CopyMessage"_q,
		u"Chat.CreateGroupChatV2"_q,
		u"Chat.CreateChannelV2"_q,
		u"MessageScheduler.CreateMessageScheduled"_q,
		u"MessageScheduler.SendMessageScheduledNow"_q,
		u"WebinarApp.CreateCallV2"_q,
	};
	return result.contains(method);
}

// Stale when the connection is back.
[[nodiscard]] bool DropWhenOffline(const QString &method) {
	return (method == u"Typing.Typing"_q);
}

} // namespace

Rpc::Rpc(QObject *parent)
: QObject(parent) {
	_timeoutTimer.setInterval(1000);
	QObject::connect(
		&_timeoutTimer,
		&QTimer::timeout,
		this,
		&Rpc::checkTimeouts);
	_timeoutTimer.start();
	QObject::connect(
		&_connection,
		&Connection::connected,
		this,
		[this](const QString &) {
			_timedOut.clear();
			Q_EMIT connected();
			sendWaiting();
		});
	QObject::connect(
		&_connection,
		&Connection::disconnected,
		this,
		[this] {
			requeuePending(u"disconnected"_q);
			Q_EMIT disconnected();
		});
	QObject::connect(
		&_connection,
		&Connection::controlReceived,
		this,
		[this](const QString &name, const QJsonObject &) {
			if (name == u"rateLimitIsReached"_q) {
				retryRateLimited();
			}
		});
	QObject::connect(
		&_connection,
		&Connection::messageReceived,
		this,
		&Rpc::handleMessage);
	QObject::connect(
		&_connection,
		&Connection::error,
		this,
		&Rpc::authError);
}

Rpc::~Rpc() = default;

void Rpc::connectAndAuth(const QString &token) {
	// The requests of the previous connection are sent with the new one.
	requeuePending(u"reconnect"_q);
	_connection.connectToServer(token);
}

void Rpc::dropConnection(const QString &reason) {
	LOG(("MtsLink RPC: dropping the connection, %1").arg(reason));
	_connection.dropConnection();
}

void Rpc::disconnect() {
	cancelPending();
	_connection.disconnect();
}

void Rpc::cancelPending() {
	auto pending = std::exchange(_pending, {});
	for (auto &call : pending) {
		if (call.failHandler) {
			call.failHandler(u"disconnected"_q);
		}
	}
}

void Rpc::call(
		const QString &method,
		const QJsonObject &param,
		DoneHandler done,
		FailHandler fail) {
	const auto connected = _connection.isConnected();
	if (!connected && DropWhenOffline(method)) {
		return;
	}
	const auto id = generateId();
	_pending.insert(id, PendingCall{
		.method = method,
		.param = param,
		.handler = std::move(done),
		.failHandler = std::move(fail),
	});
	if (connected) {
		sendCall(id);
	} else {
		LOG(("MtsLink RPC: %1 waits for the connection").arg(method));
	}
}

void Rpc::sendCall(const QString &id) {
	const auto i = _pending.find(id);
	if (i == _pending.end()) {
		return;
	}
	auto &call = i.value();
	++call.attempts;
	call.sentAt = crl::now();
	call.deadline = call.sentAt + kCallTimeout;

	// "WebinarApp.CreateCallV2" -> "webinarApp", "Chat.PinChat" -> "chat".
	const auto service = call.method.section('.', 0, 0);
	const auto dst = service.isEmpty()
		? QString()
		: (service.left(1).toLower() + service.mid(1));

	// The same id when sent again: a late answer to any attempt is taken.
	QJsonObject message;
	message["id"] = id;
	message["dst"] = dst;
	message["method"] = call.method;
	message["param"] = call.param;
	message["kind"] = QString("command");

	_connection.sendMessages(QJsonArray{message});
}

void Rpc::sendWaiting() {
	auto waiting = std::vector<QString>();
	for (auto i = _pending.begin(); i != _pending.end(); ++i) {
		if (!i.value().deadline) {
			waiting.push_back(i.key());
		}
	}
	if (waiting.empty()) {
		return;
	}
	LOG(("MtsLink RPC: sending %1 waiting requests").arg(waiting.size()));
	for (const auto &id : waiting) {
		sendCall(id);
	}
}

void Rpc::requeuePending(const QString &reason) {
	auto failed = std::vector<QString>();
	auto requeued = 0;
	for (auto i = _pending.begin(); i != _pending.end(); ++i) {
		auto &call = i.value();
		if (!call.deadline) {
			continue; // Not sent yet.
		} else if (NoRetryAfterReconnect(call.method)
			|| call.attempts >= kMaxAttempts) {
			failed.push_back(i.key());
		} else {
			call.deadline = 0;
			++requeued;
		}
	}
	if (requeued || !failed.empty()) {
		LOG(("MtsLink RPC: %1, %2 requests wait for the connection, "
			"%3 failed").arg(reason).arg(requeued).arg(failed.size()));
	}
	failCalls(std::move(failed), u"disconnected"_q);
}

void Rpc::failCalls(std::vector<QString> ids, const QString &error) {
	// The handlers may call again: taken from the list first.
	auto calls = std::vector<PendingCall>();
	for (const auto &id : ids) {
		const auto i = _pending.find(id);
		if (i != _pending.end()) {
			calls.push_back(std::move(i.value()));
			_pending.erase(i);
		}
	}
	for (const auto &call : calls) {
		if (call.failHandler) {
			call.failHandler(error);
		}
	}
}

void Rpc::checkTimeouts() {
	if (_pending.isEmpty() || !_connection.isConnected()) {
		return;
	}
	const auto now = crl::now();
	auto retry = std::vector<QString>();
	auto failed = std::vector<QString>();
	for (auto i = _pending.begin(); i != _pending.end(); ++i) {
		const auto &call = i.value();
		if (!call.deadline || call.deadline > now) {
			continue;
		}
		LOG(("MtsLink RPC timeout [%1]: no answer in %2 ms, attempt %3"
			).arg(call.method
			).arg(now - call.sentAt
			).arg(call.attempts));
		if (NoRetryAfterTimeout(call.method)
			|| call.attempts >= kMaxAttempts) {
			failed.push_back(i.key());
		} else {
			retry.push_back(i.key());
		}
	}
	if (retry.empty() && failed.empty()) {
		return;
	}
	for (const auto &id : retry) {
		_timedOut.insert(id);
	}
	for (const auto &id : failed) {
		_timedOut.insert(id);
	}
	for (const auto &id : retry) {
		sendCall(id);
	}
	failCalls(std::move(failed), u"timeout"_q);
	if (_timedOut.size() >= kTimeoutsToReconnect) {
		_timedOut.clear();
		dropConnection(u"no answers from the server"_q);
	}
}

void Rpc::retryRateLimited() {
	// The requests above the limit are dropped by the server without an
	// answer, the answered ones are already removed from the list soon.
	const auto now = crl::now();
	auto count = 0;
	for (auto &call : _pending) {
		if (call.deadline && now - call.sentAt < 1500) {
			call.deadline = std::min(call.deadline, now + kRateLimitedRetry);
			++count;
		}
	}
	LOG(("MtsLink RPC: rate limit, %1 recent requests are checked in %2 ms"
		).arg(count
		).arg(kRateLimitedRetry));
}

void Rpc::subscribe(
		const QString &method,
		const QJsonObject &param,
		EventHandler handler) {
	call(method, param, [this, handler](const QJsonObject &) {
		// Subscription confirmed, handler stored separately.
	});
	// TODO: route events to handler based on dst pattern.
}

bool Rpc::isConnected() const {
	return _connection.isConnected();
}

bool Rpc::isConnecting() const {
	return _connection.isConnecting();
}

QString Rpc::clientId() const {
	return _connection.sid();
}

void Rpc::handleMessage(const QJsonObject &message) {
	const auto type = message.value("type").toString();

	if (type == "response") {
		const auto id = message.value("id").toString();
		const auto method = message.value("method").toString();
		// {"error": {name, message}} instead of "result": the handlers get
		// {"type": "RpcError", "value": error}, not an empty "success".
		const auto resultObj = message.contains(u"result"_q)
			? message.value("result").toObject()
			: QJsonObject{
				{ u"type"_q, u"RpcError"_q },
				{ u"value"_q, message.value("error").toObject() },
			};
		const auto resultType = resultObj.value("type").toString();
		const auto it = _pending.find(id);
		if (it != _pending.end()) {
			_timedOut.clear();
			const auto pending = it.value();
			_pending.erase(it);
			if (pending.attempts > 1) {
				LOG(("MtsLink RPC: [%1] answered on attempt %2"
					).arg(pending.method
					).arg(pending.attempts));
			}
			if (!message.contains(u"result"_q)) {
				// No result: the server reports the failure differently.
				LOG(("MtsLink RPC no result [%1]: %2"
					).arg(pending.method
					).arg(QString::fromUtf8(QJsonDocument(message).toJson(
						QJsonDocument::Compact)).left(600)));
			}
			if (resultType == u"BusinessError"_q) {
				const auto val = resultObj.value("value").toObject();
				LOG(("MtsLink RPC error [%1]: %2 — %3"
					).arg(pending.method
					).arg(val.value("code").toString()
					).arg(val.value("message").toString()));
			}
			if (pending.handler) {
				pending.handler(resultObj);
			}
		} else {
			// Any answer: the connection is alive.
			_timedOut.clear();
		}
	} else if (type == "event") {
		const auto name = message.value("name").toString();
		const auto dst = message.value("dst").toString();
		const auto param = message.value("param").toObject();
		Q_EMIT eventReceived(name, dst, param);
	}
}

QString Rpc::generateId() const {
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

} // namespace MtsLink

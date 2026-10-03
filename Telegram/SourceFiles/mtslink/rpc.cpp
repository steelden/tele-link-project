/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#include "mtslink/rpc.h"

#include <QtCore/QJsonDocument>

#include <QUuid>

namespace MtsLink {

Rpc::Rpc(QObject *parent)
: QObject(parent) {
	QObject::connect(
		&_connection,
		&Connection::connected,
		this,
		[this](const QString &) { Q_EMIT connected(); });
	QObject::connect(
		&_connection,
		&Connection::disconnected,
		this,
		&Rpc::disconnected);
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
	cancelPending();
	_connection.connectToServer(token);
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
	const auto id = generateId();
	if (done) {
		_pending.insert(id, { method, std::move(done), std::move(fail) });
	}

	// "WebinarApp.CreateCallV2" -> "webinarApp", "Chat.PinChat" -> "chat".
	const auto service = method.section('.', 0, 0);
	const auto dst = service.isEmpty()
		? QString()
		: (service.left(1).toLower() + service.mid(1));

	QJsonObject message;
	message["id"] = id;
	message["dst"] = dst;
	message["method"] = method;
	message["param"] = param;
	message["kind"] = QString("command");

	_connection.sendMessages(QJsonArray{message});
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
			const auto pending = it.value();
			_pending.erase(it);
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
			pending.handler(resultObj);
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

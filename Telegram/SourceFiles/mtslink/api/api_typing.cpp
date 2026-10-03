/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#include "mtslink/api/api_typing.h"
#include "mtslink/rpc.h"

namespace MtsLink::Api {

Typing::Typing(Rpc *rpc, QObject *parent)
: QObject(parent)
, _rpc(rpc) {
}

void Typing::sendTyping(const ChatId &chatId, const MessageId &threadId) {
	QJsonObject param;
	param["chatId"] = chatId;
	param["isText"] = true;
	if (!threadId.isEmpty()) {
		param["threadId"] = threadId;
	}
	_rpc->call(
		"Typing.Typing",
		param,
		[](const QJsonObject &) {});
}

void Typing::subscribeToThread(
		const ChatId &chatId,
		const MessageId &threadId) {
	_rpc->call(
		"Typing.SubscribeThread",
		QJsonObject{
			{ "chatId", chatId },
			{ "threadId", threadId },
		},
		[](const QJsonObject &) {});
}

void Typing::subscribeToChat(const ChatId &chatId) {
	QJsonObject param;
	param["chatId"] = chatId;
	_rpc->call(
		"Typing.SubscribeChat",
		param,
		[](const QJsonObject &) {});
}

} // namespace MtsLink::Api

/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#include "mtslink/api/api_threads.h"
#include "mtslink/rpc.h"

namespace MtsLink::Api {

Threads::Threads(Rpc *rpc, QObject *parent)
: QObject(parent)
, _rpc(rpc) {
}

void Threads::loadMyThreads(int limit, int offset) {
	_rpc->call(
		"Chat.GetMyThreadsV3",
		QJsonObject{
			{"limit", limit},
			{"offset", offset},
		},
		[this](const QJsonObject &result) {
			const auto value = result.value("value").toObject();
			const auto list = value.value("items").toArray();
			QList<ThreadData> threads;
			threads.reserve(list.size());
			for (const auto &item : list) {
				threads.push_back(parseThread(item.toObject()));
			}
			Q_EMIT threadsLoaded(threads);
		});
}

void Threads::loadThread(const MessageId &threadId) {
	_rpc->call(
		"Chat.GetMyThreadV2",
		QJsonObject{ { "threadId", threadId } },
		[this](const QJsonObject &result) {
			const auto thread = result.value("value").toObject()
				.value("thread").toObject();
			if (!thread.isEmpty()) {
				Q_EMIT threadsLoaded({ parseThread(thread) });
			}
		});
}

void Threads::joinThread(
		const ChatId &chatId,
		const MessageId &threadId) {
	_rpc->call(
		"Chat.JoinToThread",
		QJsonObject{
			{"chatId", chatId},
			{"threadId", threadId},
		},
		[this, chatId, threadId](const QJsonObject &) {
			Q_EMIT threadJoined(chatId, threadId);
		});
}

void Threads::leaveThread(
		const ChatId &chatId,
		const MessageId &threadId) {
	_rpc->call(
		"Chat.LeaveFromThread",
		QJsonObject{
			{"chatId", chatId},
			{"threadId", threadId},
		},
		[this, chatId, threadId](const QJsonObject &) {
			Q_EMIT threadLeft(chatId, threadId);
		});
}

void Threads::setThreadNotifications(
		const ChatId &chatId,
		const MessageId &threadId,
		bool isNotifiable) {
	_rpc->call(
		"Chat.SetThreadNotifications",
		QJsonObject{
			{ "chatId", chatId },
			{ "messageId", threadId },
			{ "isNotifiable", isNotifiable },
		},
		[=](const QJsonObject &result) {
			if (result.value("type").toString() != u"BusinessError"_q) {
				Q_EMIT threadNotificationsChanged(
					chatId,
					threadId,
					isNotifiable);
			}
		});
}

void Threads::getUnreadCounter(const OrganizationId &orgId) {
	_rpc->call(
		"Chat.GetMyUnreadThreadsCounter",
		QJsonObject{{"organizationId", orgId}},
		[this](const QJsonObject &result) {
			const auto value = result.value("value").toObject();
			const auto count = value.value("counter").toInt();
			Q_EMIT unreadCounterLoaded(count);
		});
}

ThreadData Threads::parseThread(const QJsonObject &obj) const {
	const auto chat = obj.value("chat").toObject();
	const auto message = obj.value("message").toObject();
	return {
		.id = obj.value("id").toString(),
		.chatId = chat.value("id").toString(),
		.chatName = chat.value("name").toString(),
		.chatType = parseChatType(chat.value("type").toString()),
		.message = message,
		.isNotifiable = obj.value("isNotifiable").toBool(true),
		.childrenCount = obj.value("childrenCount").toInt(),
		.unreadChildrenCount = obj.value("unreadChildrenCount").toInt(),
		.lastChildId = obj.value("lastChildId").toString(),
	};
}

ChatType Threads::parseChatType(const QString &type) const {
	if (type == "dialog") return ChatType::Dialog;
	if (type == "channel") return ChatType::Channel;
	if (type == "group_chat") return ChatType::GroupChat;
	if (type == "discussion") return ChatType::Discussion;
	return ChatType::Channel;
}

} // namespace MtsLink::Api

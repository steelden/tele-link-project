/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#pragma once

#include "mtslink/types.h"
#include "mtslink/api/api_messages.h"

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>

namespace MtsLink {
class Rpc;
} // namespace MtsLink

namespace MtsLink::Api {

struct ThreadData {
	MessageId id; // = parent message ID = threadId
	ChatId chatId;
	QString chatName;
	ChatType chatType = ChatType::Channel;
	QJsonObject message; // full parent message
	bool isNotifiable = true;
	int childrenCount = 0;
	int unreadChildrenCount = 0;
	MessageId lastChildId;
};

class Threads final : public QObject {
	Q_OBJECT

public:
	explicit Threads(Rpc *rpc, QObject *parent = nullptr);

	void loadMyThreads(int limit = 100, int offset = 0);
	void loadThread(const MessageId &threadId);
	void setThreadNotifications(
		const ChatId &chatId,
		const MessageId &threadId,
		bool isNotifiable);
	void joinThread(const ChatId &chatId, const MessageId &threadId);
	void leaveThread(const ChatId &chatId, const MessageId &threadId);
	void getUnreadCounter(const OrganizationId &orgId);

Q_SIGNALS:
	void threadsLoaded(
		const QList<ThreadData> &threads,
		const QList<MemberProfile> &profiles);
	void threadJoined(const ChatId &chatId, const MessageId &threadId);
	void threadLeft(const ChatId &chatId, const MessageId &threadId);
	void threadNotificationsChanged(
		const ChatId &chatId,
		const MessageId &threadId,
		bool isNotifiable);
	void unreadCounterLoaded(int count);

private:
	[[nodiscard]] ThreadData parseThread(const QJsonObject &obj) const;
	[[nodiscard]] ChatType parseChatType(const QString &type) const;

	Rpc *_rpc = nullptr;
};

} // namespace MtsLink::Api

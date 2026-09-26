/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#pragma once

#include "mtslink/types.h"

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>

namespace MtsLink {
class Rpc;
} // namespace MtsLink

namespace MtsLink::Api {

class Sending final : public QObject {
	Q_OBJECT

public:
	explicit Sending(Rpc *rpc, QObject *parent = nullptr);

	void sendMessage(
		const ChatId &chatId,
		const QString &text,
		const QJsonArray &blocks = {},
		const QJsonArray &mentionsMeta = {},
		const MessageId &replyToMessageId = {},
		const QStringList &fileIds = {},
		const MessageId &parentId = {},
		const QString &clientId = {});

	void deleteMessage(
		const ChatId &chatId,
		const MessageId &messageId);

	void editMessage(
		const ChatId &chatId,
		const MessageId &messageId,
		const QString &text,
		const QJsonArray &blocks = {},
		const QJsonArray &mentionsMeta = {});

	void readMessage(
		const ChatId &chatId,
		const MessageId &messageId);

	void setChatNotifications(
		const ChatId &chatId,
		bool isNotifiable);

	void addReaction(
		const ChatId &chatId,
		const MessageId &messageId,
		const QString &emoji,
		const QString &emojiId);

	void removeReaction(
		const ChatId &chatId,
		const MessageId &messageId,
		const QString &emoji,
		const QString &emojiId);

	void forwardMessage(
		const ChatId &chatId,
		const MessageId &originalMessageId,
		const QString &text = {});

	void pinMessage(
		const ChatId &chatId,
		const MessageId &messageId);

	void unpinMessage(
		const ChatId &chatId,
		const MessageId &messageId);

Q_SIGNALS:
	void messageSent(const ChatId &chatId, const QJsonObject &result);
	void messageDeleteDone(const ChatId &chatId, const MessageId &messageId);
	void messageEditDone(const ChatId &chatId, const MessageId &messageId);

private:
	static constexpr int kMaxForwardRetries = 5;
	static constexpr int kForwardRetryDelayMs = 1000;

	void sendForwardWithRetry(
		const ChatId &chatId,
		const QString &copyMessageId,
		const QString &text,
		int attempt);

	Rpc *_rpc = nullptr;
};

} // namespace MtsLink::Api

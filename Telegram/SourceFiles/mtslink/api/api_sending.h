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
	Rpc *_rpc = nullptr;
};

} // namespace MtsLink::Api

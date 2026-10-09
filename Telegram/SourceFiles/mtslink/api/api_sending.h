/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#pragma once

#include "mtslink/types.h"

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

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
		const QString &clientId = {},
		std::function<void(const QJsonObject&)> done = nullptr,
		std::function<void(const QString&)> fail = nullptr);

	void deleteMessage(
		const ChatId &chatId,
		const MessageId &messageId);

	void editMessage(
		const ChatId &chatId,
		const MessageId &messageId,
		const QString &text,
		const QJsonArray &blocks = {},
		const QJsonArray &mentionsMeta = {});

	// The same message of the same chat is read once (the chat read was
	// sent again on each scroll / redraw of an open chat).
	void readMessage(
		const ChatId &chatId,
		const MessageId &messageId);
	// After a reconnect: the requests could be lost, sent again.
	void forgetSentReads();

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
	QSet<QString> _sentReads;
};

} // namespace MtsLink::Api

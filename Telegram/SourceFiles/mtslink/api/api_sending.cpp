/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#include "mtslink/api/api_sending.h"
#include "mtslink/rpc.h"

#include <QUuid>
#include <QTimer>

namespace MtsLink::Api {

Sending::Sending(Rpc *rpc, QObject *parent)
: QObject(parent)
, _rpc(rpc) {
}

void Sending::sendMessage(
		const ChatId &chatId,
		const QString &text,
		const QJsonArray &blocks,
		const QJsonArray &mentionsMeta,
		const MessageId &replyToMessageId,
		const QStringList &fileIds,
		const MessageId &parentId,
		const QString &clientId) {
	QJsonObject param;
	param["clientId"] = clientId.isEmpty()
		? QUuid::createUuid().toString(QUuid::WithoutBraces)
		: clientId;
	param["chatId"] = chatId;
	param["text"] = text;
	param["isMarkdown"] = true;
	if (blocks.isEmpty()) {
		QJsonArray simpleBlocks;
		QJsonObject block;
		block["type"] = QStringLiteral("TextElement");
		block["value"] = QJsonObject{{"text", text}};
		simpleBlocks.append(block);
		param["blocks"] = simpleBlocks;
	} else {
		param["blocks"] = blocks;
	}
	if (!mentionsMeta.isEmpty()) {
		param["metadata"] = QJsonObject{
			{"type", "TextMetadataV2"},
			{"value", QJsonObject{{"mentions", mentionsMeta}}},
		};
	}
	if (!replyToMessageId.isEmpty()) {
		param["repliedId"] = replyToMessageId;
	}
	if (!fileIds.isEmpty()) {
		QJsonArray arr;
		for (const auto &id : fileIds) {
			arr.append(id);
		}
		param["fileIds"] = arr;
	}
	if (!parentId.isEmpty()) {
		param["parentId"] = parentId;
	}
	_rpc->call(
		"Chat.SendMessageV2",
		param,
		[this, chatId](const QJsonObject &result) {
			Q_EMIT messageSent(chatId, result);
		});
}

void Sending::deleteMessage(
		const ChatId &chatId,
		const MessageId &messageId) {
	QJsonObject param;
	param["chatId"] = chatId;
	param["messageId"] = messageId;
	_rpc->call(
		"Chat.DeleteMessage",
		param,
		[this, chatId, messageId](const QJsonObject &) {
			Q_EMIT messageDeleteDone(chatId, messageId);
		});
}

void Sending::editMessage(
		const ChatId &chatId,
		const MessageId &messageId,
		const QString &text,
		const QJsonArray &blocks,
		const QJsonArray &mentionsMeta) {
	QJsonObject param;
	param["chatId"] = chatId;
	param["messageId"] = messageId;
	param["text"] = text;
	param["isMarkdown"] = true;
	if (blocks.isEmpty()) {
		QJsonArray simpleBlocks;
		QJsonObject block;
		block["type"] = QStringLiteral("TextElement");
		block["value"] = QJsonObject{{"text", text}};
		simpleBlocks.append(block);
		param["blocks"] = simpleBlocks;
	} else {
		param["blocks"] = blocks;
	}
	if (!mentionsMeta.isEmpty()) {
		param["metadata"] = QJsonObject{
			{"type", "TextMetadataV2"},
			{"value", QJsonObject{{"mentions", mentionsMeta}}},
		};
	}
	_rpc->call(
		"Chat.UpdateMessage",
		param,
		[this, chatId, messageId](const QJsonObject &) {
			Q_EMIT messageEditDone(chatId, messageId);
		});
}

void Sending::readMessage(
		const ChatId &chatId,
		const MessageId &messageId) {
	QJsonObject param;
	param["chatId"] = chatId;
	param["lastMessageId"] = messageId;
	_rpc->call(
		"Chat.MarkMessagesAsRead",
		param,
		[](const QJsonObject &) {});
}

void Sending::setChatNotifications(
		const ChatId &chatId,
		bool isNotifiable) {
	QJsonObject param;
	param["chatId"] = chatId;
	param["isNotifiable"] = isNotifiable;
	_rpc->call(
		"Chat.SetChatNotifications",
		param,
		[](const QJsonObject &) {});
}

void Sending::addReaction(
		const ChatId &chatId,
		const MessageId &messageId,
		const QString &emoji,
		const QString &emojiId) {
	QJsonObject param;
	param["chatId"] = chatId;
	param["messageId"] = messageId;
	param["emoji"] = emoji;
	if (!emojiId.isEmpty()) {
		param["emojiId"] = emojiId;
	}
	_rpc->call(
		"Chat.AddReactionMessage",
		param,
		[](const QJsonObject &) {});
}

void Sending::removeReaction(
		const ChatId &chatId,
		const MessageId &messageId,
		const QString &emoji,
		const QString &emojiId) {
	QJsonObject param;
	param["chatId"] = chatId;
	param["messageId"] = messageId;
	param["emoji"] = emoji;
	if (!emojiId.isEmpty()) {
		param["emojiId"] = emojiId;
	}
	_rpc->call(
		"Chat.DeleteReactionMessage",
		param,
		[](const QJsonObject &) {});
}

void Sending::forwardMessage(
		const ChatId &chatId,
		const MessageId &originalMessageId,
		const QString &text) {
	QJsonObject copyParam;
	copyParam["messageId"] = originalMessageId;
	_rpc->call(
		"Chat.CopyMessage",
		copyParam,
		[this, chatId, text](const QJsonObject &result) {
			const auto copyId = result
				.value("value").toObject()
				.value("copyMessageId").toString();
			if (copyId.isEmpty()) {
				LOG(("MtsLink CopyMessage failed: %1"
					).arg(QString::fromUtf8(
						QJsonDocument(result).toJson(
							QJsonDocument::Compact))));
				return;
			}
			sendForwardWithRetry(chatId, copyId, text, 0);
		});
}

void Sending::sendForwardWithRetry(
		const ChatId &chatId,
		const QString &copyMessageId,
		const QString &text,
		int attempt) {
	QJsonObject param;
	param["chatId"] = chatId;
	param["copyMessageId"] = copyMessageId;
	param["forwardClientId"] = QUuid::createUuid()
		.toString(QUuid::WithoutBraces);
	param["clientId"] = QUuid::createUuid()
		.toString(QUuid::WithoutBraces);
	param["text"] = text;
	_rpc->call(
		"Chat.ForwardMessage",
		param,
		[this, chatId, copyMessageId, text, attempt](
				const QJsonObject &result) {
			const auto type = result.value("type").toString();
			if (type == u"BusinessError"_q) {
				const auto val = result.value("value").toObject();
				const auto code = val.value("code").toString();
				if (code == u"REPEAT_THE_REQUEST_LATER"_q
					&& attempt < kMaxForwardRetries) {
					LOG(("MtsLink ForwardMessage retry %1/%2")
						.arg(attempt + 1)
						.arg(kMaxForwardRetries));
					QTimer::singleShot(
						kForwardRetryDelayMs,
						this,
						[this, chatId, copyMessageId, text, attempt] {
							sendForwardWithRetry(
								chatId,
								copyMessageId,
								text,
								attempt + 1);
						});
					return;
				}
			}
			Q_EMIT messageSent(chatId, result);
		});
}

void Sending::pinMessage(
		const ChatId &chatId,
		const MessageId &messageId) {
	QJsonObject param;
	param["chatId"] = chatId;
	param["messageId"] = messageId;
	_rpc->call(
		"Chat.PinMessage",
		param,
		[](const QJsonObject &) {});
}

void Sending::unpinMessage(
		const ChatId &chatId,
		const MessageId &messageId) {
	QJsonObject param;
	param["chatId"] = chatId;
	param["messageId"] = messageId;
	_rpc->call(
		"Chat.UnpinMessage",
		param,
		[](const QJsonObject &) {});
}

} // namespace MtsLink::Api

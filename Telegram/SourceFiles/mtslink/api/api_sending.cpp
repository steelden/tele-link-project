/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#include "mtslink/api/api_sending.h"
#include "mtslink/rpc.h"

#include <QUuid>

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
	param["messageId"] = messageId;
	_rpc->call(
		"Chat.ReadMessage",
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

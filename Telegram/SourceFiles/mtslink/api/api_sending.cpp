/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#include "mtslink/api/api_sending.h"
#include "mtslink/rpc.h"
#include "mtslink/data_adapters.h"
#include "ui/emoji_config.h"

#include <QUuid>
#include <QJsonDocument>
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
		const QString &clientId,
		std::function<void(const QJsonObject&)> done,
		std::function<void(const QString&)> fail) {
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
		[this, chatId, done](const QJsonObject &result) {
			Q_EMIT messageSent(chatId, result);
			if (done) {
				done(result);
			}
		},
		[fail](const QString &error) {
			if (fail) {
				fail(error);
			}
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
		const QJsonArray &mentionsMeta,
		std::function<void()> done,
		std::function<void(const QString &error)> fail) {
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
	// Temporary: an edit with a quote was not applied by the server.
	LOG(("MtsLink Edit: %1 text=[%2] blocks=%3"
		).arg(messageId
		).arg(text
		).arg(QString::fromUtf8(
			QJsonDocument(param["blocks"].toArray()).toJson(
				QJsonDocument::Compact))));
	_rpc->call(
		"Chat.UpdateMessage",
		param,
		[this, chatId, messageId, done, fail](const QJsonObject &result) {
			LOG(("MtsLink Edit: %1 done: %2"
				).arg(messageId
				).arg(QString::fromUtf8(
					QJsonDocument(result).toJson(QJsonDocument::Compact))));
			// A check of the server ("validationError", ...) comes as an
			// answer, not as a failure: the edit is not applied.
			const auto type = result.value(u"type"_q).toString();
			if (type.endsWith(u"Error"_q)) {
				// "validationError: ERR_INVALID_LINK_ELEMENT_URL (blocks):
				// invalid url".
				auto details = QStringList();
				const auto errors = result.value(u"value"_q).toObject().value(
					u"errors"_q).toArray();
				for (const auto &entry : errors) {
					const auto error = entry.toObject();
					auto line = error.value(u"code"_q).toString();
					const auto field = error.value(u"field"_q).toString();
					if (!field.isEmpty()) {
						line += u" ("_q + field + u")"_q;
					}
					const auto message = error.value(u"message"_q).toString();
					if (!message.isEmpty()) {
						line += u": "_q + message;
					}
					details.push_back(line);
				}
				if (fail) {
					fail(details.isEmpty()
						? type
						: (type + u"\n"_q + details.join(u"\n"_q)));
				}
				return;
			}
			Q_EMIT messageEditDone(chatId, messageId);
			if (done) {
				done();
			}
		},
		[messageId, fail](const QString &error) {
			LOG(("MtsLink Edit: %1 failed: %2").arg(messageId, error));
			if (fail) {
				fail(error);
			}
		});
}

namespace {

// The same read is not sent again sooner: it was sent once for each
// connection, and a read the server didn't apply (the chat stayed unread)
// was never sent again.
constexpr auto kReadResendDelay = crl::time(10000);

QHash<QString, crl::time> ReadSentAt;

// The read queue: one request of a chat in flight, the next one (the
// newest) waits for its answer or for the resend delay.
QSet<ChatId> ReadsInFlight;
QHash<ChatId, MessageId> ReadsQueued;
QSet<ChatId> ReadsForced;
QSet<ChatId> ReadsTimerSet;

} // namespace

void Sending::readMessage(
		const ChatId &chatId,
		const MessageId &messageId) {
	const auto key = chatId + ':' + messageId;
	const auto now = crl::now();
	const auto forced = ReadsForced.contains(chatId);
	const auto recent = _sentReads.contains(key)
		&& (now - ReadSentAt.value(key) < kReadResendDelay);
	if (ReadsInFlight.contains(chatId) || (recent && forced)) {
		// Queued: the newest read of the chat is sent after the answer
		// to the request in flight or after the delay.
		if (forced || !recent) {
			ReadsQueued.insert(chatId, messageId);
			LOG(("MtsLink ReadRequest: %1 till %2 queued (in flight %3)"
				).arg(chatId, messageId
				).arg(Logs::b(ReadsInFlight.contains(chatId))));
			if (recent && !ReadsTimerSet.contains(chatId)) {
				ReadsTimerSet.insert(chatId);
				const auto wait = kReadResendDelay
					- (now - ReadSentAt.value(key));
				QTimer::singleShot(int(wait), this, [=] {
					ReadsTimerSet.remove(chatId);
					sendQueuedRead(chatId);
				});
			}
		}
		return;
	} else if (recent) {
		return;
	}
	ReadsForced.remove(chatId);
	_sentReads.insert(key);
	ReadSentAt.insert(key, now);
	ReadsInFlight.insert(chatId);
	QJsonObject param;
	param["chatId"] = chatId;
	param["lastMessageId"] = messageId;
	// Temporary: the chat stays unread after the read.
	LOG(("MtsLink ReadRequest: %1 till %2").arg(chatId, messageId));
	_rpc->call(
		"Chat.MarkMessagesAsRead",
		param,
		[=](const QJsonObject &result) {
			const auto type = result.value(u"type"_q).toString();
			LOG(("MtsLink ReadRequest: %1 till %2 result: %3"
				).arg(chatId, messageId
				).arg(QString::fromUtf8(QJsonDocument(result).toJson(
					QJsonDocument::Compact)).left(500)));
			if (type == u"RpcError"_q || type == u"BusinessError"_q) {
				_sentReads.remove(key);
				ReadSentAt.remove(key);
			}
			ReadsInFlight.remove(chatId);
			sendQueuedRead(chatId);
		},
		[=](const QString &error) {
			LOG(("MtsLink ReadRequest: %1 till %2 failed: %3"
				).arg(chatId, messageId, error));
			// Lost with the connection: sent again.
			_sentReads.remove(key);
			ReadSentAt.remove(key);
			ReadsInFlight.remove(chatId);
			sendQueuedRead(chatId);
		});
}

void Sending::sendQueuedRead(const ChatId &chatId) {
	if (ReadsInFlight.contains(chatId)) {
		return;
	}
	const auto i = ReadsQueued.find(chatId);
	if (i == ReadsQueued.end()) {
		return;
	}
	const auto messageId = i.value();
	const auto key = chatId + ':' + messageId;
	const auto left = kReadResendDelay
		- (crl::now() - ReadSentAt.value(key));
	if (_sentReads.contains(key) && left > 0) {
		// The same read was just sent: after the delay.
		if (!ReadsTimerSet.contains(chatId)) {
			ReadsTimerSet.insert(chatId);
			QTimer::singleShot(int(left), this, [=] {
				ReadsTimerSet.remove(chatId);
				sendQueuedRead(chatId);
			});
		}
		return;
	}
	ReadsQueued.erase(i);
	_sentReads.remove(key);
	ReadsForced.remove(chatId);
	readMessage(chatId, messageId);
}

void Sending::forceNextRead(const ChatId &chatId) {
	LOG(("MtsLink ReadRequest: %1 forced").arg(chatId));
	ReadsForced.insert(chatId);
}

void Sending::forgetSentReads() {
	_sentReads.clear();
	ReadSentAt.clear();
	ReadsInFlight.clear();
	ReadsQueued.clear();
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

namespace {

// The other clients take the emoji from the reaction events: not a real
// emoji ("empty" from the broken map) is replaced by the one of the id.
[[nodiscard]] QString SentEmoji(const QString &emoji, const QString &id) {
	if (!emoji.isEmpty() && Ui::Emoji::Find(emoji)) {
		return emoji;
	}
	const auto result = MtsLink::idToEmoji(id);
	LOG(("MtsLink Reaction: sending '%1' replaced by '%2' for %3"
		).arg(emoji
		).arg(result
		).arg(id));
	return result;
}

} // namespace

void Sending::addReaction(
		const ChatId &chatId,
		const MessageId &messageId,
		const QString &emoji,
		const QString &emojiId) {
	QJsonObject param;
	param["chatId"] = chatId;
	param["messageId"] = messageId;
	param["emoji"] = SentEmoji(emoji, emojiId);
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
	param["emoji"] = SentEmoji(emoji, emojiId);
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

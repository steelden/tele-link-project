/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#include "mtslink/api/api_messages.h"
#include "mtslink/data_adapters.h"
#include "mtslink/rpc.h"

#include <QtCore/QJsonDocument>

namespace MtsLink::Api {

Messages::Messages(Rpc *rpc, QObject *parent)
: QObject(parent)
, _rpc(rpc) {
}

void Messages::load(
		const ChatId &chatId,
		const MessageId &fromMessageId,
		int limit) {
	QJsonObject param;
	param["chatId"] = chatId;
	param["limit"] = limit;
	if (!fromMessageId.isEmpty()) {
		param["from"] = fromMessageId;
		param["direction"] = QStringLiteral("Before");
	}

	const auto isOlder = !fromMessageId.isEmpty();
	_loadingChats.insert(chatId);
	const auto timer = QPointer<QTimer>(new QTimer(this));
	timer->setSingleShot(true);
	timer->start(10000);
	QObject::connect(timer, &QTimer::timeout, this, [=] {
		// No answer: repeated with the reconnect or the next open.
		LOG(("MtsLink Messages: load of %1 timed out").arg(chatId));
		_loadingChats.remove(chatId);
		if (!isOlder) {
			_failedChats.insert(chatId);
		}
		timer->deleteLater();
	});
	const auto finishTimer = [timer] {
		if (timer) {
			timer->stop();
			timer->deleteLater();
		}
	};
	_rpc->call(
		"Chat.GetMessagesV2",
		param,
		[this, chatId, isOlder, finishTimer](const QJsonObject &result) {
			finishTimer();
			_loadingChats.remove(chatId);
			if (!isOlder && result.value("type").toString() != u"BusinessError"_q) {
				_loadedOnceChats.insert(chatId);
				_failedChats.remove(chatId);
			}
			const auto value = result.value("value").toObject();
			const auto msgArray = value.value("messages").toArray();
			const auto profilesArray =
				value.value("memberProfiles").toArray();

			const auto rawCount = msgArray.size();
			MessageId rawLastId;
			if (!msgArray.isEmpty()) {
				rawLastId = msgArray.last().toObject()
					.value("id").toString();
				if (!isOlder) {
					_newestRawIds.insert(
						chatId,
						msgArray.first().toObject().value("id").toString());
				}
			}

			QList<MessageData> messages;
			messages.reserve(rawCount);
			for (const auto &item : msgArray) {
				auto msg = parseMessage(item.toObject());
				if (msg.isDeleted) {
					// Deleted on the server: removed locally (kept in the
					// cache when deleted while TeleLink was closed).
					Q_EMIT deletedMessageSeen(
						msg.chatId.isEmpty() ? chatId : msg.chatId,
						msg.id);
					continue;
				}
				if (msg.chatId.isEmpty()) {
					msg.chatId = chatId;
				}
				messages.push_back(std::move(msg));
			}

			QList<MemberProfile> profiles;
			profiles.reserve(profilesArray.size());
			for (const auto &item : profilesArray) {
				profiles.push_back(parseProfile(item.toObject()));
			}

			if (isOlder) {
				Q_EMIT olderMessagesLoaded(
					chatId, messages, profiles, rawLastId, rawCount);
			} else {
				Q_EMIT messagesLoaded(
					chatId, messages, profiles, rawLastId, rawCount);
			}
		},
		[this, chatId, finishTimer](const QString &error) {
			finishTimer();
			LOG(("MtsLink Messages: load of %1 failed: %2").arg(chatId, error));
			_loadingChats.remove(chatId);
			_failedChats.insert(chatId);
		});
}

void Messages::loadPreview(const ChatId &chatId, int limit) {
	_rpc->call(
		"Chat.GetMessagesV2",
		QJsonObject{ { "chatId", chatId }, { "limit", limit } },
		[this, chatId, limit](const QJsonObject &result) {
			const auto value = result.value("value").toObject();
			const auto msgArray = value.value("messages").toArray();
			QList<MessageData> messages;
			for (const auto &item : msgArray) {
				auto msg = parseMessage(item.toObject());
				if (msg.isDeleted) {
					// Deleted on the server: removed locally (kept in the
					// cache when deleted while TeleLink was closed).
					Q_EMIT deletedMessageSeen(
						msg.chatId.isEmpty() ? chatId : msg.chatId,
						msg.id);
					continue;
				}
				if (msg.chatId.isEmpty()) {
					msg.chatId = chatId;
				}
				messages.push_back(std::move(msg));
			}
			QList<MemberProfile> profiles;
			for (const auto &item : value.value("memberProfiles").toArray()) {
				profiles.push_back(parseProfile(item.toObject()));
			}
			Q_EMIT previewLoaded(
				chatId,
				messages,
				profiles,
				int(msgArray.size()),
				limit);
		});
}

MessageData Messages::parseMessage(const QJsonObject &obj) const {
	QList<FileData> files;
	const auto filesArray = obj.value("files").toArray();
	for (const auto &f : filesArray) {
		files.push_back(parseFile(f.toObject()));
	}

	const auto thread = obj.value("thread").toObject();

	QList<MentionInfo> mentions;
	auto mentionsArray = obj.value("metadata").toObject()
		.value("value").toObject()
		.value("mentions").toArray();
	if (mentionsArray.isEmpty()) {
		mentionsArray = obj.value("mentions").toArray();
	}
	for (const auto &m : mentionsArray) {
		const auto mo = m.toObject();
		const auto type = mo.value("type").toString();
		if (type == "User") {
			mentions.push_back({
				.userId = mo.value("id").toString(),
				.name = mo.value("name").toString(),
			});
		}
	}

	QList<ReactionData> reactions;
	const auto reactionsArray = obj.value("reactions").toArray();
	for (const auto &r : reactionsArray) {
		const auto ro = r.toObject();
		const auto eid = ro.value("emojiId").toString();
		const auto emj = ro.value("emoji").toString();
		MtsLink::setEmojiIdMapping(eid, emj);
		reactions.push_back({
			.emojiId = eid,
			.emoji = emj,
			.count = ro.value("count").toInt(),
			.selected = ro.value("selected").toBool(),
		});
	}

	const auto msgType = [&] {
		const auto t = obj.value("type").toString();
		if (t == "Call") return MessageType::Call;
		if (t == "System") return MessageType::System;
		if (t == "Forward") return MessageType::Forward;
		return MessageType::Text;
	}();

	auto callMeta = [&]() -> std::optional<CallMetadata> {
		if (msgType != MessageType::Call) {
			return std::nullopt;
		}
		const auto meta = obj.value("metadata").toObject();
		if (meta.value("type").toString() != "CallMetadata") {
			return std::nullopt;
		}
		const auto v = meta.value("value").toObject();
		return CallMetadata{
			.status = v.value("status").toString(),
			.joinLink = v.value("joinLink").toString(),
			.webinarEventId = v.value("webinarEventId").toString(),
			.duration = int(v.value("duration").toDouble() / 1000),
			.statusReason = v.value("statusReasonV2").toString(
				v.value("statusReason").toString()),
		};
	}();

	return {
		.id = obj.value("id").toString(),
		.chatId = obj.value("chatId").toString(),
		.authorId = obj.value("authorId").toString(),
		.text = obj.value("text").toString(),
		.markdown = obj.value("markdown").toString(),
		.type = msgType,
		.blocks = obj.value("blocks").toArray(),
		.files = files,
		.mentions = mentions,
		.reactions = reactions,
		.createdAt = qint64(obj.value("createdAt").toDouble()),
		.updatedAt = qint64(obj.value("updatedAt").toDouble()),
		.isDeleted = obj.value("isDeleted").toBool(),
		.isRead = obj.contains("isRead")
			? std::make_optional(obj.value("isRead").toBool())
			: std::nullopt,
		.repliedMessageId =
			obj.value("repliedMessage").toObject().value("id").toString(),
		.parentId = [&] {
			auto id = obj.value("parentMessage").toObject().value("id").toString();
			return id.isEmpty() ? obj.value("parentId").toString() : id;
		}(),
		.threadChildrenCount = thread.value("childrenCount").toInt(),
		.threadUnreadCount = thread.value("unreadChildrenCount").toInt(),
		.forward = [&]() -> std::optional<ForwardInfo> {
			const auto fwd = obj.value("forward").toObject();
			if (fwd.isEmpty()) {
				return std::nullopt;
			}
			return ForwardInfo{
				.authorId = fwd.value("authorId").toString(),
				.messageId = fwd.value("messageId").toString(),
				.chatId = fwd.value("chatId").toString(),
				.createdAt = qint64(fwd.value("createdAt").toDouble()),
			};
		}(),
		.callMeta = std::move(callMeta),
	};
}

FileData Messages::parseFile(const QJsonObject &obj) const {
	return ParseFileData(obj);
}

FileData ParseFileData(const QJsonObject &obj) {
	const auto meta = obj.value("meta").toObject().value("value").toObject();
	const auto voiceMeta = obj.value("voiceMeta").toObject();
	auto waveform = QVector<int>();
	for (const auto &value : voiceMeta.value("waveform").toArray()) {
		waveform.push_back(value.toInt());
	}
	return {
		.id = obj.value("id").toString(),
		.name = obj.value("name").toString(),
		.url = obj.value("url").toString(),
		.size = qint64(obj.value("size").toDouble()),
		.mime = obj.value("mime").toString(),
		.width = meta.value("width").toInt(),
		.height = meta.value("height").toInt(),
		.voice = !voiceMeta.isEmpty(),
		.duration = voiceMeta.value("duration").toInt(),
		.waveform = std::move(waveform),
	};
}

MemberProfile Messages::parseProfile(const QJsonObject &obj) const {
	return ParseMemberProfile(obj);
}

void ParseProfileDetails(const QJsonObject &profile, MemberProfile &result) {
	result.position = profile.value("position").toString();
	result.department = profile.value("department").toString();
	result.additionalFields.clear();
	for (const auto &field : profile.value("additionalFields").toArray()) {
		const auto object = field.toObject();
		const auto id = object.value("profileFieldId").toString();
		if (!id.isEmpty()) {
			result.additionalFields.push_back({
				id,
				object.value("value").toString(),
			});
		}
	}
	result.detailsKnown = true;
}

MemberProfile ParseMemberProfile(const QJsonObject &obj) {
	const auto roleStr = obj.value("role").toString();
	auto result = MemberProfile{
		.userId = obj.value("userId").toString(),
		.organizationId = obj.value("organizationId").toString(),
		.email = obj.value("email").toString(),
		.firstName = obj.value("firstName").toString(),
		.lastName = obj.value("lastName").toString(),
		.displayName = obj.value("displayName").toString(),
		// Same unreliable "presence" / "inCall" as in chat member profiles.
		.presence = MemberPresence::Unknown,
		.avatarFileId = obj.value("avatarFileId").toString(),
		.role = (roleStr == "Owner")
			? MemberRole::Owner
			: (roleStr == "Admin")
				? MemberRole::Admin
				: (roleStr == "Guest")
					? MemberRole::Guest
					: MemberRole::Member,
	};
	if (obj.contains("customStatus")) {
		result.customStatus = obj.value("customStatus").toObject();
		result.customStatusKnown = true;
	}
	return result;
}

void Messages::search(
		const ChatId &chatId,
		const QString &query,
		int limit) {
	QJsonObject param;
	param["chatId"] = chatId;
	param["query"] = query;
	param["limit"] = limit;

	_rpc->call(
		"Chat.SearchMessagesV2",
		param,
		[this, chatId](const QJsonObject &result) {
			const auto value = result.value("value").toObject();
			const auto msgArray = value.value("messages").toArray();
			const auto profilesArray =
				value.value("memberProfiles").toArray();
			const auto total = value.value("total").toInt(
				msgArray.size());

			QList<MessageData> messages;
			messages.reserve(msgArray.size());
			for (const auto &item : msgArray) {
				auto msg = parseMessage(item.toObject());
				if (msg.isDeleted) {
					// Deleted on the server: removed locally (kept in the
					// cache when deleted while TeleLink was closed).
					Q_EMIT deletedMessageSeen(
						msg.chatId.isEmpty() ? chatId : msg.chatId,
						msg.id);
					continue;
				}
				if (msg.chatId.isEmpty()) {
					msg.chatId = chatId;
				}
				messages.push_back(std::move(msg));
			}

			QList<MemberProfile> profiles;
			profiles.reserve(profilesArray.size());
			for (const auto &item : profilesArray) {
				profiles.push_back(parseProfile(item.toObject()));
			}

			Q_EMIT searchCompleted(chatId, messages, profiles, total);
		});
}

void Messages::searchGlobal(
		const QString &query,
		const OrganizationId &organizationId,
		int from,
		int size,
		GlobalSearchDone done) {
	_rpc->call(
		"Chat.SearchMessagesV4",
		QJsonObject{
			{ "query", query },
			{ "organizationId", organizationId },
			{ "from", from },
			{ "size", size },
			{ "previewCharsSize", 255 },
		},
		[=](const QJsonObject &result) {
			const auto value = result.value("value").toObject();
			const auto items = value.value("items").toArray();
			QList<MessageData> messages;
			for (const auto &item : items) {
				const auto obj = item.toObject();
				auto msg = parseMessage(obj);
				if (msg.isDeleted || msg.id.isEmpty()) {
					continue;
				}
				if (msg.parentId.isEmpty()) {
					msg.parentId = obj.value("threadId").toString();
				}
				messages.push_back(std::move(msg));
			}
			QList<MemberProfile> profiles;
			for (const auto &item : value.value("memberProfiles").toArray()) {
				profiles.push_back(parseProfile(item.toObject()));
			}
			done(
				std::move(messages),
				std::move(profiles),
				value.value("total").toInt(),
				int(items.size()));
		},
		[=](const QString &) { done({}, {}, 0, 0); });
}

void Messages::loadPinned(const ChatId &chatId, int limit) {
	if (_loadingPinnedChats.contains(chatId)) {
		return;
	}
	_loadingPinnedChats.insert(chatId);

	QJsonObject param;
	param["chatId"] = chatId;
	param["limit"] = limit;

	_rpc->call(
		"Chat.GetPinnedMessagesV2",
		param,
		[this, chatId](const QJsonObject &result) {

			const auto value = result.value("value").toObject();
			const auto msgArray = value.value("messages").toArray();
			const auto profilesArray =
				value.value("memberProfiles").toArray();
			const auto total = value.value("total").toInt(
				msgArray.size());

			QList<MessageData> messages;
			messages.reserve(msgArray.size());
			for (const auto &item : msgArray) {
				auto msg = parseMessage(item.toObject());
				if (msg.isDeleted) {
					// Deleted on the server: removed locally (kept in the
					// cache when deleted while TeleLink was closed).
					Q_EMIT deletedMessageSeen(
						msg.chatId.isEmpty() ? chatId : msg.chatId,
						msg.id);
					continue;
				}
				if (msg.chatId.isEmpty()) {
					msg.chatId = chatId;
				}
				messages.push_back(std::move(msg));
			}

			QList<MemberProfile> profiles;
			profiles.reserve(profilesArray.size());
			for (const auto &item : profilesArray) {
				profiles.push_back(parseProfile(item.toObject()));
			}

			Q_EMIT pinnedMessagesLoaded(chatId, messages, profiles, total);
		});
}

void Messages::reloadPinned(const ChatId &chatId) {
	_loadingPinnedChats.remove(chatId);
	loadPinned(chatId);
}

void Messages::loadThread(
		const ChatId &chatId,
		const MessageId &parentId,
		const MessageId &fromMessageId,
		int limit) {
	QJsonObject param;
	param["chatId"] = chatId;
	param["parentId"] = parentId;
	param["limit"] = limit;
	if (!fromMessageId.isEmpty()) {
		param["from"] = fromMessageId;
		param["direction"] = QStringLiteral("Before");
	}

	_rpc->call(
		"Chat.GetMessagesV2",
		param,
		[this, chatId, parentId](const QJsonObject &result) {
			const auto value = result.value("value").toObject();
			const auto msgArray = value.value("messages").toArray();
			const auto profilesArray =
				value.value("memberProfiles").toArray();

			QList<MessageData> messages;
			messages.reserve(msgArray.size());
			for (const auto &item : msgArray) {
				auto msg = parseMessage(item.toObject());
				if (msg.isDeleted) {
					// Deleted on the server: removed locally (kept in the
					// cache when deleted while TeleLink was closed).
					Q_EMIT deletedMessageSeen(
						msg.chatId.isEmpty() ? chatId : msg.chatId,
						msg.id);
					continue;
				}
				if (msg.chatId.isEmpty()) {
					msg.chatId = chatId;
				}
				messages.push_back(std::move(msg));
			}

			QList<MemberProfile> profiles;
			profiles.reserve(profilesArray.size());
			for (const auto &item : profilesArray) {
				profiles.push_back(parseProfile(item.toObject()));
			}

			Q_EMIT threadMessagesLoaded(chatId, parentId, messages, profiles);
		});
}

void Messages::loadThreadPage(
		const ChatId &chatId,
		const MessageId &parentId,
		const MessageId &fromMessageId,
		bool after,
		int limit,
		ThreadPageDone done) {
	_rpc->call(
		"Chat.GetMessagesV2",
		QJsonObject{
			{ "chatId", chatId },
			{ "parentId", parentId },
			{ "from", fromMessageId },
			{ "direction", after ? u"After"_q : u"Before"_q },
			{ "limit", limit },
		},
		[=](const QJsonObject &result) {
			const auto value = result.value("value").toObject();
			auto messages = QList<MessageData>();
			for (const auto &item : value.value("messages").toArray()) {
				auto msg = parseMessage(item.toObject());
				if (msg.isDeleted) {
					// Deleted on the server: removed locally (kept in the
					// cache when deleted while TeleLink was closed).
					Q_EMIT deletedMessageSeen(
						msg.chatId.isEmpty() ? chatId : msg.chatId,
						msg.id);
					continue;
				}
				if (msg.chatId.isEmpty()) {
					msg.chatId = chatId;
				}
				messages.push_back(std::move(msg));
			}
			auto profiles = QList<MemberProfile>();
			for (const auto &item
					: value.value("memberProfiles").toArray()) {
				profiles.push_back(parseProfile(item.toObject()));
			}
			done(std::move(messages), std::move(profiles));
		},
		[=](const QString &) {
			done({}, {});
		});
}

void Messages::loadAround(
		const ChatId &chatId,
		const MessageId &messageId,
		int limit) {
	const auto half = limit / 2;

	const auto emitEmpty = [=] {
		Q_EMIT aroundMessagesLoaded(chatId, messageId, {}, {});
	};
	const auto emitResult = [=](const QJsonObject &r) {
		const auto v = r.value("value").toObject();
		const auto arr = v.value("messages").toArray();
		const auto profArr = v.value("memberProfiles").toArray();

		QList<MessageData> messages;
		messages.reserve(arr.size());
		for (const auto &item : arr) {
			auto msg = parseMessage(item.toObject());
			if (msg.isDeleted) {
				// Deleted on the server: removed locally (kept in the
				// cache when deleted while TeleLink was closed).
				Q_EMIT deletedMessageSeen(
					msg.chatId.isEmpty() ? chatId : msg.chatId,
					msg.id);
				continue;
			}
			if (msg.chatId.isEmpty()) {
				msg.chatId = chatId;
			}
			messages.push_back(std::move(msg));
		}

		QList<MemberProfile> profiles;
		profiles.reserve(profArr.size());
		for (const auto &item : profArr) {
			profiles.push_back(parseProfile(item.toObject()));
		}

		Q_EMIT aroundMessagesLoaded(chatId, messageId, messages, profiles);
	};
	const auto firstId = [](const QJsonObject &result) {
		const auto list = result.value("value").toObject()
			.value("messages").toArray();
		return list.isEmpty()
			? QString()
			: list.first().toObject().value("id").toString();
	};
	const auto request = [=](
			const QString &from,
			const QString &direction,
			int count,
			Fn<void(const QJsonObject&)> done) {
		_rpc->call(
			"Chat.GetMessagesV2",
			QJsonObject{
				{ "chatId", chatId },
				{ "from", from },
				{ "direction", direction },
				{ "limit", count },
			},
			std::move(done),
			[=](const QString &) { emitEmpty(); });
	};

	request(messageId, u"Before"_q, 1, [=](const QJsonObject &result) {
		if (const auto prevId = firstId(result); !prevId.isEmpty()) {
			request(prevId, u"After"_q, half, emitResult);
			return;
		}
		// The target is the first message in the chat: nothing before it,
		// so take the next one and load "Before" it, which includes it.
		request(messageId, u"After"_q, 1, [=](const QJsonObject &next) {
			if (const auto nextId = firstId(next); !nextId.isEmpty()) {
				request(nextId, u"Before"_q, std::max(half, 1), emitResult);
			} else {
				emitEmpty();
			}
		});
	});
}

bool Messages::isLoading(const ChatId &chatId) const {
	return _loadingChats.contains(chatId);
}

bool Messages::loadedOnce(const ChatId &chatId) const {
	return _loadedOnceChats.contains(chatId);
}

void Messages::retryFailedLoads() {
	auto chats = std::exchange(_failedChats, {});
	for (const auto &chatId : chats) {
		load(chatId);
	}
}

} // namespace MtsLink::Api

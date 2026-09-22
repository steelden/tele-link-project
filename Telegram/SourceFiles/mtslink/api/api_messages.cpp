/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#include "mtslink/api/api_messages.h"
#include "mtslink/rpc.h"

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

	LOG(("MtsLink Paging: RPC params=%1")
		.arg(QString::fromUtf8(
			QJsonDocument(param).toJson(QJsonDocument::Compact))));

	const auto isOlder = !fromMessageId.isEmpty();
	_loadingChats.insert(chatId);
	_rpc->call(
		"Chat.GetMessagesV2",
		param,
		[this, chatId, isOlder](const QJsonObject &result) {
			_loadingChats.remove(chatId);
			LOG(("MtsLink Paging: result type=%1, keys=%2")
				.arg(result.value("type").toString())
				.arg(result.keys().join(",")));
			const auto value = result.value("value").toObject();
			const auto msgArray = value.value("messages").toArray();
			if (!msgArray.isEmpty()) {
				const auto first = msgArray.first().toObject();
				const auto last = msgArray.last().toObject();
				LOG(("MtsLink Paging: msgs=%1, first.id=%2 t=%3, last.id=%4 t=%5")
					.arg(msgArray.size())
					.arg(first.value("id").toString())
					.arg(qint64(first.value("createdAt").toDouble()))
					.arg(last.value("id").toString())
					.arg(qint64(last.value("createdAt").toDouble())));
			}
			const auto profilesArray =
				value.value("memberProfiles").toArray();

			const auto rawCount = msgArray.size();
			MessageId rawLastId;
			if (!msgArray.isEmpty()) {
				rawLastId = msgArray.last().toObject()
					.value("id").toString();
			}

			QList<MessageData> messages;
			messages.reserve(rawCount);
			for (const auto &item : msgArray) {
				auto msg = parseMessage(item.toObject());
				if (msg.isDeleted) {
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

	return {
		.id = obj.value("id").toString(),
		.chatId = obj.value("chatId").toString(),
		.authorId = obj.value("authorId").toString(),
		.text = obj.value("text").toString(),
		.markdown = obj.value("markdown").toString(),
		.type = [&] {
			const auto t = obj.value("type").toString();
			if (t == "Call") return MessageType::Call;
			if (t == "System") return MessageType::System;
			if (t == "Forward") return MessageType::Forward;
			return MessageType::Text;
		}(),
		.blocks = obj.value("blocks").toArray(),
		.files = files,
		.mentions = mentions,
		.createdAt = qint64(obj.value("createdAt").toDouble()),
		.updatedAt = qint64(obj.value("updatedAt").toDouble()),
		.isDeleted = obj.value("isDeleted").toBool(),
		.repliedMessageId =
			obj.value("repliedMessage").toObject().value("id").toString(),
		.parentId = [&] {
			auto id = obj.value("parentMessage").toObject().value("id").toString();
			return id.isEmpty() ? obj.value("parentId").toString() : id;
		}(),
		.threadChildrenCount = thread.value("childrenCount").toInt(),
		.threadUnreadCount = thread.value("unreadChildrenCount").toInt(),
	};
}

FileData Messages::parseFile(const QJsonObject &obj) const {
	const auto meta = obj.value("meta").toObject().value("value").toObject();
	return {
		.id = obj.value("id").toString(),
		.name = obj.value("name").toString(),
		.url = obj.value("url").toString(),
		.size = qint64(obj.value("size").toDouble()),
		.mime = obj.value("mime").toString(),
		.width = meta.value("width").toInt(),
		.height = meta.value("height").toInt(),
	};
}

MemberProfile Messages::parseProfile(const QJsonObject &obj) const {
	const auto roleStr = obj.value("role").toString();
	const auto presenceStr = obj.value("presence").toString();
	return {
		.userId = obj.value("userId").toString(),
		.organizationId = obj.value("organizationId").toString(),
		.email = obj.value("email").toString(),
		.firstName = obj.value("firstName").toString(),
		.lastName = obj.value("lastName").toString(),
		.displayName = obj.value("displayName").toString(),
		.presence = (presenceStr == "Online")
			? MemberPresence::Online
			: (presenceStr == "Away")
				? MemberPresence::Away
				: MemberPresence::Offline,
		.avatarFileId = obj.value("avatarFileId").toString(),
		.role = (roleStr == "Owner")
			? MemberRole::Owner
			: (roleStr == "Admin")
				? MemberRole::Admin
				: (roleStr == "Guest")
					? MemberRole::Guest
					: MemberRole::Member,
	};
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

void Messages::loadPinned(const ChatId &chatId, int limit) {
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

bool Messages::isLoading(const ChatId &chatId) const {
	return _loadingChats.contains(chatId);
}

} // namespace MtsLink::Api

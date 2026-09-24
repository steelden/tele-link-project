/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#include "mtslink/api/api_channels.h"
#include "mtslink/rpc.h"

namespace MtsLink::Api {

Channels::Channels(Rpc *rpc, QObject *parent)
: QObject(parent)
, _rpc(rpc) {
}

void Channels::loadMyChannels() {
	_rpc->call(
		"Chat.GetMyChannelsV3",
		QJsonObject{},
		[this](const QJsonObject &result) {
			LOG(("MtsLink Channels: GetMyChannelsV3 raw keys: %1")
				.arg(result.keys().join(", ")));
			const auto value = result.value("value").toObject();
			LOG(("MtsLink Channels: value keys: %1")
				.arg(value.keys().join(", ")));
			const auto list = value.value("items").toArray();
			if (!list.isEmpty()) {
				const auto first = QJsonDocument(list.first().toObject()).toJson(QJsonDocument::Compact);
				LOG(("MtsLink Channels: first channel item: %1").arg(QString::fromUtf8(first.left(500))));
			}
			QList<ChannelData> channels;
			channels.reserve(list.size());
			for (const auto &item : list) {
				channels.push_back(parseChat(item.toObject()));
			}
			Q_EMIT channelsLoaded(channels);
		});
}

void Channels::loadMyDialogsAndGroupChats() {
	_rpc->call(
		"Chat.GetMyDialogsAndGroupChatsV2",
		QJsonObject{},
		[this](const QJsonObject &result) {
			const auto value = result.value("value").toObject();
			const auto list = value.value("items").toArray();
			const auto profiles = value.value("memberProfiles").toArray();
			LOG(("MtsLink Channels: dialogs count=%1, profiles=%2")
				.arg(list.size()).arg(profiles.size()));
			if (!profiles.isEmpty()) {
				const auto first = QJsonDocument(profiles.first().toObject()).toJson(QJsonDocument::Compact);
				LOG(("MtsLink Channels: first profile: %1").arg(QString::fromUtf8(first.left(500))));
			}

			QHash<QString, QString> chatNames;
			QHash<QString, QString> userAvatars;
			for (const auto &p : profiles) {
				const auto prof = p.toObject();
				const auto chatId = prof.value("chatId").toString();
				const auto uid = prof.value("userId").toString();
				auto name = prof.value("displayName").toString();
				if (name.isEmpty()) {
					name = prof.value("firstName").toString()
						+ " " + prof.value("lastName").toString();
					name = name.trimmed();
				}
				if (!chatId.isEmpty() && !name.isEmpty()) {
					chatNames.insert(chatId, name);
				}
				const auto avatar = prof.value("avatarFileId").toString();
				if (!uid.isEmpty() && !avatar.isEmpty()) {
					userAvatars.insert(uid, avatar);
				}
			}
			// Build userId→name map from profiles.
			QHash<QString, QString> userNames;
			for (const auto &p : profiles) {
				const auto prof = p.toObject();
				const auto uid = prof.value("userId").toString();
				const auto first = prof.value("firstName").toString();
				const auto last = prof.value("lastName").toString();
				auto name = (first + " " + last).trimmed();
				if (name.isEmpty()) {
					name = prof.value("displayName").toString();
				}
				if (!uid.isEmpty() && !name.isEmpty()) {
					userNames.insert(uid, name);
				}
			}
			LOG(("MtsLink Channels: userNames mapped: %1").arg(userNames.size()));

			for (int i = 0; i < qMin(3, int(list.size())); ++i) {
				const auto raw = QJsonDocument(list[i].toObject()).toJson(QJsonDocument::Compact);
				LOG(("MtsLink Channels: dialog[%1]: %2").arg(i).arg(QString::fromUtf8(raw.left(600))));
			}

			QList<ChannelData> dialogs;
			dialogs.reserve(list.size());
			for (const auto &item : list) {
				const auto obj = item.toObject();
				auto ch = parseChat(obj);
				const auto inner = obj.contains("value")
					? obj.value("value").toObject()
					: obj;
				ch.interlocutorId = inner.value("interlocutorId").toString();
				if (ch.type == ChatType::Favorites && ch.name.isEmpty()) {
					ch.name = QString::fromUtf8("\xd0\x98\xd0\xb7\xd0\xb1\xd1\x80\xd0\xb0\xd0\xbd\xd0\xbd\xd0\xbe\xd0\xb5");
				} else if (ch.name.isEmpty()) {
					ch.name = userNames.value(ch.interlocutorId);
				}
				if (ch.avatarFileId.isEmpty()
					&& !ch.interlocutorId.isEmpty()) {
					ch.avatarFileId = userAvatars.value(ch.interlocutorId);
				}
				dialogs.push_back(std::move(ch));
			}
			Q_EMIT dialogsLoaded(dialogs);
		});
}

void Channels::loadChatInfo(const ChatId &chatId) {
	_rpc->call(
		"Chat.GetChatV3",
		QJsonObject{{"chatId", chatId}},
		[this](const QJsonObject &result) {
			const auto value = result.value("value").toObject();
			auto ch = parseChat(value);
			if (ch.type == ChatType::Dialog && ch.name.isEmpty()) {
				const auto profiles = value.value("memberProfiles").toArray();
				for (const auto &p : profiles) {
					const auto prof = p.toObject();
					const auto uid = prof.value("userId").toString();
					if (uid == ch.interlocutorId) {
						auto name = prof.value("displayName").toString();
						if (name.isEmpty()) {
							name = (prof.value("firstName").toString()
								+ " " + prof.value("lastName").toString()).trimmed();
						}
						ch.name = name;
						if (ch.avatarFileId.isEmpty()) {
							ch.avatarFileId = prof.value("avatarFileId").toString();
						}
						break;
					}
				}
			}
			Q_EMIT chatInfoLoaded(ch);
		});
}

ChannelData Channels::parseChat(const QJsonObject &obj) const {
	auto src = obj;
	auto chatType = ChatType::Channel;

	if (src.contains("value") && src.contains("type")) {
		const auto wrapper = src.value("type").toString();
		src = src.value("value").toObject();
		if (wrapper.contains("Dialog")) {
			chatType = ChatType::Dialog;
		} else if (wrapper.contains("GroupChat")) {
			chatType = ChatType::GroupChat;
		} else if (wrapper.contains("Favorites")) {
			chatType = ChatType::Favorites;
		} else if (wrapper.contains("Channel")) {
			chatType = ChatType::Channel;
		}
	}
	if (src.contains("type")) {
		chatType = parseChatType(src.value("type").toString());
	}

	return {
		.id = src.value("chatId").toString(),
		.name = src.value("name").toString(),
		.description = src.value("description").toString(),
		.type = chatType,
		.organizationId = src.value("organizationId").toString(),
		.unreadCount = src.value("unreadMessageCount").toInt(),
		.lastMessageId = src.value("lastMessageId").toString(),
		.lastMessageText = src.value("lastMessageText").toString(),
		.lastMessageTimestamp = qint64(
			src.value("lastUpdatedAt").toDouble()),
		.avatarFileId = src.value("coverFileId").toString(),
		.isMuted = !src.value("isNotifiable").toBool(true),
		.isPinned = src.value("pinPosition").toInt() > 0,
		.isReadOnly = src.value("isReadOnly").toBool(false),
		.pinnedMessageCount = src.value("pinnedMessageCount").toInt(),
		.memberRole = src.value("memberRole").toString(),
		.interlocutorId = src.value("interlocutorId").toString(),
		.memberCount = src.value("membersCount").toInt(),
	};
}

ChatType Channels::parseChatType(const QString &type) const {
	if (type == "Dialog") return ChatType::Dialog;
	if (type == "Channel") return ChatType::Channel;
	if (type == "GroupChat") return ChatType::GroupChat;
	if (type == "Discussion") return ChatType::Discussion;
	if (type == "Favorites") return ChatType::Favorites;
	if (type == "Team") return ChatType::Team;
	return ChatType::Channel;
}

} // namespace MtsLink::Api

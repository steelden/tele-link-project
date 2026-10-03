/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#include "mtslink/api/api_channels.h"
#include "mtslink/rpc.h"

#include <QtCore/QJsonDocument>

namespace MtsLink::Api {

namespace {

QJsonArray UsersWithOrganization(
		const QStringList &userIds,
		const OrganizationId &organizationId) {
	auto result = QJsonArray();
	for (const auto &userId : userIds) {
		result.push_back(QJsonObject{
			{ "userId", userId },
			{ "organizationId", organizationId },
		});
	}
	return result;
}

} // namespace

Channels::Channels(Rpc *rpc, QObject *parent)
: QObject(parent)
, _rpc(rpc) {
}

void Channels::loadMyChannels() {
	_rpc->call(
		"Chat.GetMyChannelsV3",
		QJsonObject{},
		[this](const QJsonObject &result) {
			const auto value = result.value("value").toObject();
			const auto list = value.value("items").toArray();
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
		.isPublic = src.value("isPublic").toBool(false),
		.isPublicKnown = src.contains("isPublic"),
		.pinnedMessageCount = src.value("pinnedMessageCount").toInt(),
		.memberRole = src.value("memberRole").toString(),
		.interlocutorId = src.value("interlocutorId").toString(),
		.memberCount = src.value("membersCount").toInt(),
	};
}

void Channels::pinChat(const ChatId &chatId) {
	_rpc->call(
		"Chat.PinChat",
		QJsonObject{{"chatId", chatId}},
		[this, chatId](const QJsonObject &result) {
			const auto value = result.value("value").toObject();
			const auto pinPosition = value.value("pinPosition").toInt();
			Q_EMIT chatPinned(chatId, pinPosition);
		});
}

void Channels::unpinChat(const ChatId &chatId) {
	_rpc->call(
		"Chat.UnpinChat",
		QJsonObject{{"chatId", chatId}},
		[this, chatId](const QJsonObject &) {
			Q_EMIT chatUnpinned(chatId);
		});
}

void Channels::createCall(
		const ChatId &chatId,
		const QString &name,
		std::function<void(QString joinLink)> done,
		std::function<void()> fail) {
	_rpc->call(
		"WebinarApp.CreateCallV2",
		QJsonObject{ { "chatId", chatId }, { "name", name } },
		[=](const QJsonObject &result) {
			const auto joinLink = result.value("value").toObject()
				.value("joinLink").toString();
			if (result.value("type").toString() == QStringLiteral("Call")
				&& !joinLink.isEmpty()) {
				done(joinLink);
			} else {
				fail();
			}
		},
		[=](const QString &) { fail(); });
}

void Channels::simpleCall(
		const QString &method,
		const QJsonObject &param,
		Done done) {
	_rpc->call(
		method,
		param,
		[=](const QJsonObject &result) {
			const auto type = result.value("type").toString();
			LOG(("MtsLink Channels: %1 -> %2"
				).arg(method
				).arg(QString::fromUtf8(QJsonDocument(
					result
				).toJson(QJsonDocument::Compact)).left(400)));
			// "BusinessError", "validationError" and alike.
			const auto failed = type.contains(
				QStringLiteral("error"),
				Qt::CaseInsensitive);
			if (done) {
				done(!failed);
			}
		},
		[=](const QString &) {
			if (done) {
				done(false);
			}
		});
}

void Channels::addAdministrators(
		const ChatId &chatId,
		const QStringList &userIds,
		Done done) {
	simpleCall("Chat.AddChannelAdministrators", QJsonObject{
		{ "chatId", chatId },
		{ "users", QJsonArray::fromStringList(userIds) },
	}, std::move(done));
}

void Channels::removeAdministrators(
		const ChatId &chatId,
		const QStringList &userIds,
		Done done) {
	simpleCall("Chat.RemoveChannelAdministrators", QJsonObject{
		{ "chatId", chatId },
		{ "users", QJsonArray::fromStringList(userIds) },
	}, std::move(done));
}

void Channels::giveOwnership(
		const ChatId &chatId,
		const QString &userId,
		Done done) {
	simpleCall("Chat.GiveChannelOwnership", QJsonObject{
		{ "chatId", chatId },
		{ "userId", userId },
	}, std::move(done));
}


void Channels::addUsers(
		const ChatId &chatId,
		const QStringList &userIds,
		const OrganizationId &organizationId,
		Done done) {
	simpleCall("Chat.AddUsersAndTeamsToChannelV2", QJsonObject{
		{ "chatId", chatId },
		{ "users", UsersWithOrganization(userIds, organizationId) },
		{ "teams", QJsonArray() },
	}, std::move(done));
}

void Channels::removeUsers(
		const ChatId &chatId,
		const QStringList &userIds,
		const OrganizationId &organizationId,
		Done done) {
	simpleCall("Chat.RemoveUsersFromChannelV2", QJsonObject{
		{ "chatId", chatId },
		{ "users", UsersWithOrganization(userIds, organizationId) },
	}, std::move(done));
}

void Channels::createChannel(
		const QString &name,
		const QString &description,
		bool isPublic,
		bool isReadOnly,
		const OrganizationId &organizationId,
		std::function<void(std::optional<ChannelData>)> done) {
	_rpc->call(
		"Chat.CreateChannelV2",
		QJsonObject{
			{ "name", name },
			{ "description", description },
			{ "isPublic", isPublic },
			{ "isReadOnly", isReadOnly },
			{ "organizationId", organizationId },
		},
		[=](const QJsonObject &result) {
			if (result.value("type").toString() != QStringLiteral("Channel")) {
				done(std::nullopt);
				return;
			}
			auto channel = parseChat(result);
			channel.type = ChatType::Channel;
			const auto value = result.value("value").toObject();
			if (channel.memberRole.isEmpty()) {
				channel.memberRole = value.value("ownerID").toString();
			}
			if (channel.id.isEmpty()) {
				done(std::nullopt);
			} else {
				done(channel);
			}
		},
		[=](const QString &) { done(std::nullopt); });
}

void Channels::addChannelCover(
		const ChatId &chatId,
		const FileId &fileId,
		Done done) {
	simpleCall("Chat.AddChannelCover", QJsonObject{
		{ "channelId", chatId },
		{ "fileId", fileId },
	}, std::move(done));
}

void Channels::leaveChat(const ChatId &chatId, Done done) {
	simpleCall("Chat.LeaveFromChatV2", QJsonObject{
		{ "chatId", chatId },
	}, std::move(done));
}

void Channels::deleteChannel(const ChatId &chatId, Done done) {
	simpleCall("Chat.DeleteChannel", QJsonObject{
		{ "chatId", chatId },
	}, std::move(done));
}

void Channels::joinChat(const ChatId &chatId, Done done) {
	simpleCall("Chat.JoinToChat", QJsonObject{
		{ "chatId", chatId },
	}, std::move(done));
}

void Channels::createGroupChat(
		const QString &name,
		const OrganizationId &organizationId,
		const QStringList &userIds,
		std::function<void(std::optional<ChannelData>)> done) {
	_rpc->call(
		"Chat.CreateGroupChatV2",
		QJsonObject{
			{ "name", name },
			{ "organizationId", organizationId },
			{ "usersWithOrg", UsersWithOrganization(userIds, organizationId) },
		},
		[=](const QJsonObject &result) {
			if (result.value("type").toString()
				!= QStringLiteral("GroupChat")) {
				done(std::nullopt);
				return;
			}
			auto chat = parseChat(result);
			chat.type = ChatType::GroupChat;
			const auto value = result.value("value").toObject();
			if (chat.memberRole.isEmpty()) {
				chat.memberRole = value.value("ownerID").toString();
			}
			if (chat.id.isEmpty()) {
				done(std::nullopt);
			} else {
				done(chat);
			}
		},
		[=](const QString &) { done(std::nullopt); });
}

void Channels::addGroupChatCover(
		const ChatId &chatId,
		const FileId &fileId,
		Done done) {
	simpleCall("Chat.AddGroupChatCover", QJsonObject{
		{ "chatId", chatId },
		{ "fileId", fileId },
	}, std::move(done));
}

void Channels::addGroupUsers(
		const ChatId &chatId,
		const QStringList &userIds,
		const OrganizationId &organizationId,
		Done done) {
	simpleCall("Chat.AddUsersToGroupChatV2", QJsonObject{
		{ "chatId", chatId },
		{ "users", UsersWithOrganization(userIds, organizationId) },
	}, std::move(done));
}

void Channels::removeGroupUsers(
		const ChatId &chatId,
		const QStringList &userIds,
		const OrganizationId &organizationId,
		Done done) {
	simpleCall("Chat.RemoveUsersFromGroupChatV2", QJsonObject{
		{ "chatId", chatId },
		{ "users", UsersWithOrganization(userIds, organizationId) },
	}, std::move(done));
}

void Channels::giveGroupOwnership(
		const ChatId &chatId,
		const QString &userId,
		Done done) {
	simpleCall("Chat.GiveGroupChatOwnership", QJsonObject{
		{ "chatId", chatId },
		{ "userId", userId },
	}, std::move(done));
}

void Channels::updateChannel(
		const ChatId &chatId,
		const QString &name,
		const QString &description,
		bool isPublic,
		bool isReadOnly,
		Done done) {
	simpleCall("Chat.UpdateChannel", QJsonObject{
		{ "chatId", chatId },
		{ "name", name },
		{ "description", description },
		{ "isPublic", isPublic },
		{ "isReadOnly", isReadOnly },
	}, std::move(done));
}

void Channels::updateGroupChat(
		const ChatId &chatId,
		const QString &name,
		Done done) {
	simpleCall("Chat.UpdateGroupChat", QJsonObject{
		{ "chatId", chatId },
		{ "name", name },
	}, std::move(done));
}

void Channels::deleteGroupChat(const ChatId &chatId, Done done) {
	simpleCall("Chat.DeleteGroupChat", QJsonObject{
		{ "chatId", chatId },
	}, std::move(done));
}

void Channels::searchChannels(
		const QString &query,
		const OrganizationId &organizationId,
		int from,
		int size,
		std::function<void(QList<ChannelData>)> done) {
	_rpc->call(
		"Chat.SearchChannelsV3",
		QJsonObject{
			{ "query", query },
			{ "organizationId", organizationId },
			{ "from", from },
			{ "size", size },
			{ "previewCharsSize", 255 },
		},
		[=](const QJsonObject &result) {
			QList<ChannelData> list;
			const auto items = result.value("value").toObject()
				.value("items").toArray();
			for (const auto &item : items) {
				const auto obj = item.toObject();
				auto channel = ChannelData();
				channel.id = obj.value("id").toString();
				channel.name = obj.value("name").toString();
				channel.description = obj.value("description").toString();
				channel.type = ChatType::Channel;
				channel.organizationId =
					obj.value("organizationId").toString();
				channel.isPublic = obj.value("isPublic").toBool();
				channel.isReadOnly = obj.value("isReadOnly").toBool();
				if (!channel.id.isEmpty()) {
					list.push_back(std::move(channel));
				}
			}
			done(std::move(list));
		},
		[=](const QString &) { done({}); });
}

void Channels::searchNonMembers(
		const ChatId &chatId,
		bool groupChat,
		const QString &query,
		int offset,
		int limit,
		MembersDone done) {
	auto param = QJsonObject{
		{ "chatId", chatId },
		{ "limit", limit },
		{ "offset", offset },
	};
	if (!query.isEmpty()) {
		param.insert("query", query);
	}
	_rpc->call(
		groupChat
			? "Chat.SearchNonChatMembersV2"
			: "Chat.SearchNonChatMembersAndTeamsV2",
		param,
		[=](const QJsonObject &result) {
			auto list = QList<MemberProfile>();
			const auto value = result.value("value").toObject();
			// Group chats: {members: [{profile}]}, no teams.
			for (const auto &entry : value.value("members").toArray()) {
				auto profile = ParseMemberProfile(
					entry.toObject().value("profile").toObject());
				profile.presence = MemberPresence::Unknown;
				profile.role = MemberRole::Member;
				if (!profile.userId.isEmpty()) {
					list.push_back(std::move(profile));
				}
			}
			const auto items = value.value("items").toArray();
			for (const auto &entry : items) {
				const auto obj = entry.toObject();
				if (obj.value("type").toString()
					!= QStringLiteral("SearchNonChatItemOrganizationMemberProfile")) {
					continue; // Teams are not supported yet.
				}
				const auto prof = obj.value("value").toObject()
					.value("item").toObject();
				auto profile = MemberProfile{
					.userId = prof.value("userId").toString(),
					.organizationId =
						prof.value("organizationId").toString(),
					.email = prof.value("email").toString(),
					.firstName = prof.value("firstName").toString(),
					.lastName = prof.value("lastName").toString(),
					.displayName = prof.value("displayName").toString(),
					.presence = MemberPresence::Unknown,
					.avatarFileId = prof.value("avatarFileId").toString(),
					.role = MemberRole::Member,
				};
				if (!profile.userId.isEmpty()) {
					list.push_back(std::move(profile));
				}
			}
			done(std::move(list));
		},
		[=](const QString &) { done({}); });
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

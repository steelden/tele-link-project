/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#include "mtslink/api/api_users.h"
#include "mtslink/rpc.h"

namespace MtsLink::Api {

Users::Users(Rpc *rpc, QObject *parent)
: QObject(parent)
, _rpc(rpc) {
}

void Users::loadMember(const UserId &userId, const QString &organizationId) {
	_rpc->call(
		"Organization.GetMemberV2",
		QJsonObject{{"userId", userId}, {"organizationId", organizationId}},
		[this](const QJsonObject &result) {
			const auto value = result.value("value").toObject();
			const auto prof = value.value("profile").toObject();
			const auto roleStr = value.value("role").toString();
			const auto presenceStr = value.value("presence").toString();
			MemberProfile profile{
				.userId = value.value("userId").toString(),
				.organizationId =
					value.value("organizationId").toString(),
				.email = prof.value("email").toString(),
				.phone = prof.value("phone").toString(),
				.position = prof.value("position").toString(),
				.department = prof.value("department").toString(),
				.firstName = prof.value("firstName").toString(),
				.lastName = prof.value("lastName").toString(),
				.displayName = prof.value("displayName").toString(),
				.presence = (presenceStr == "Online")
					? MemberPresence::Online
					: (presenceStr == "Away")
						? MemberPresence::Away
						: MemberPresence::Offline,
				.avatarFileId =
					prof.value("avatarFileId").toString(),
				.role = (roleStr == "Owner")
					? MemberRole::Owner
					: (roleStr == "Admin")
						? MemberRole::Admin
						: (roleStr == "Guest")
							? MemberRole::Guest
							: MemberRole::Member,
			};
			if (value.contains("inCall")) {
				profile.inCall = int(value.value("inCall").toBool());
			}
			_cache.insert(profile.userId, profile);
			Q_EMIT memberLoaded(profile);
		});
}

void Users::loadOrganizationMembers(int offset, int limit) {
	_rpc->call(
		"Organization.GetMembersV2",
		QJsonObject{{"offset", offset}, {"limit", limit}},
		[this](const QJsonObject &result) {
			const auto value = result.value("value").toObject();
			const auto list = value.value("members").toArray();
			QList<MemberProfile> members;
			members.reserve(list.size());
			for (const auto &item : list) {
				const auto obj = item.toObject();
				const auto roleStr = obj.value("role").toString();
				const auto presenceStr =
					obj.value("presence").toString();
				MemberProfile profile{
					.userId = obj.value("userId").toString(),
					.organizationId =
						obj.value("organizationId").toString(),
					.email = obj.value("email").toString(),
					.phone = obj.value("phone").toString(),
					.position = obj.value("position").toString(),
					.department = obj.value("department").toString(),
					.firstName = obj.value("firstName").toString(),
					.lastName = obj.value("lastName").toString(),
					.displayName =
						obj.value("displayName").toString(),
					.presence = (presenceStr == "Online")
						? MemberPresence::Online
						: (presenceStr == "Away")
							? MemberPresence::Away
							: MemberPresence::Offline,
					.avatarFileId =
						obj.value("avatarFileId").toString(),
					.role = (roleStr == "Owner")
						? MemberRole::Owner
						: (roleStr == "Admin")
							? MemberRole::Admin
							: (roleStr == "Guest")
								? MemberRole::Guest
								: MemberRole::Member,
				};
				if (obj.contains("inCall")) {
					profile.inCall = int(obj.value("inCall").toBool());
				}
				_cache.insert(profile.userId, profile);
				members.push_back(std::move(profile));
			}
			Q_EMIT membersLoaded(members);
		});
}

void Users::searchMembers(
		const QString &query,
		int offset,
		int limit,
		std::function<void(QList<MemberProfile>)> done) {
	_rpc->call(
		"Member.SearchMembers",
		QJsonObject{
			{ "query", query },
			{ "offset", offset },
			{ "limit", limit },
			{ "highlightSize", 255 },
		},
		[=](const QJsonObject &result) {
			QList<MemberProfile> list;
			const auto items = result.value("value").toObject()
				.value("items").toArray();
			for (const auto &item : items) {
				const auto obj = item.toObject();
				const auto prof = obj.value("profile").toObject();
				auto profile = MemberProfile{
					.userId = obj.value("userId").toString(),
					.organizationId =
						obj.value("organizationId").toString(),
					.email = prof.value("email").toString(),
					.phone = prof.value("phone").toString(),
					.position = prof.value("position").toString(),
					.firstName = prof.value("firstName").toString(),
					.lastName = prof.value("lastName").toString(),
					.displayName = prof.value("displayName").toString(),
					.presence = MemberPresence::Unknown,
					.avatarFileId = prof.value("avatarFileId").toString(),
					.role = MemberRole::Member,
				};
				if (!profile.userId.isEmpty()
					&& !obj.value("isBot").toBool()) {
					list.push_back(std::move(profile));
				}
			}
			done(std::move(list));
		},
		[=](const QString &) { done({}); });
}

void Users::loadChatMembers(const ChatId &chatId) {
	_rpc->call(
		"Chat.SearchChatMembersV2",
		QJsonObject{
			{"chatId", chatId},
			{"query", QString()},
			{"offset", 0},
			{"limit", 200},
		},
		[this, chatId](const QJsonObject &result) {
			const auto value = result.value("value").toObject();
			const auto membersList = value.value("members").toArray();
			const auto adminsList = value.value("admins").toArray();

			const auto parseEntry = [](const QJsonValue &item,
					MemberRole defaultRole) -> MemberProfile {
				const auto wrapper = item.toObject();
				const auto obj = wrapper.value("profile").toObject();
				const auto chatRole = wrapper.value("role").toString();
				// "presence" / "inCall" in chat member profiles are not
				// reliable (almost everyone is "Online"), statuses come from
				// Member.GetMember and organization events instead.
				const auto presence = MemberPresence::Unknown;
				auto role = defaultRole;
				if (chatRole.contains("Owner")) {
					role = MemberRole::Owner;
				} else if (chatRole.contains("Admin")) {
					role = MemberRole::Admin;
				}
				return MemberProfile{
					.userId = obj.value("userId").toString(),
					.organizationId =
						obj.value("organizationId").toString(),
					.email = obj.value("email").toString(),
					.firstName = obj.value("firstName").toString(),
					.lastName = obj.value("lastName").toString(),
					.displayName =
						obj.value("displayName").toString(),
					.presence = presence,
					.avatarFileId =
						obj.value("avatarFileId").toString(),
					.role = role,
				};
			};

			QList<MemberProfile> members;
			members.reserve(adminsList.size() + membersList.size());
			for (const auto &item : adminsList) {
				auto profile = parseEntry(item, MemberRole::Admin);
				if (profile.userId.isEmpty()) {
					continue;
				}
				_cache.insert(profile.userId, profile);
				members.push_back(std::move(profile));
			}
			for (const auto &item : membersList) {
				auto profile = parseEntry(item, MemberRole::Member);
				if (profile.userId.isEmpty()) {
					continue;
				}
				_cache.insert(profile.userId, profile);
				members.push_back(std::move(profile));
			}
			Q_EMIT chatMembersLoaded(chatId, members);
		});
}

void Users::loadPresence(
		const UserId &userId,
		const QString &organizationId) {
	_rpc->call(
		"Member.GetMember",
		QJsonObject{
			{ "organizationId", organizationId },
			{ "userId", userId },
		},
		[=](const QJsonObject &result) {
			const auto value = result.value("value").toObject();
			const auto presenceStr = value.value("presence").toString();
			if (presenceStr.isEmpty()) {
				return;
			}
			Q_EMIT presenceLoaded(
				userId,
				(presenceStr == u"Online"_q)
					? MemberPresence::Online
					: (presenceStr == u"Away"_q)
					? MemberPresence::Away
					: MemberPresence::Offline,
				value.value("inCall").toBool());
		});
}

MemberProfile Users::cachedProfile(const UserId &userId) const {
	return _cache.value(userId);
}

void Users::cacheProfiles(const QList<MemberProfile> &profiles) {
	for (const auto &p : profiles) {
		_cache.insert(p.userId, p);
	}
}

} // namespace MtsLink::Api

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
				_cache.insert(profile.userId, profile);
				members.push_back(std::move(profile));
			}
			Q_EMIT membersLoaded(members);
		});
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
			const auto list = value.value("members").toArray();
			QList<MemberProfile> members;
			members.reserve(list.size());
			for (const auto &item : list) {
				const auto obj = item.toObject();
				MemberProfile profile{
					.userId = obj.value("userId").toString(),
					.firstName = obj.value("firstName").toString(),
					.lastName = obj.value("lastName").toString(),
					.displayName =
						obj.value("displayName").toString(),
					.avatarFileId =
						obj.value("avatarFileId").toString(),
				};
				if (profile.userId.isEmpty()) {
					continue;
				}
				_cache.insert(profile.userId, profile);
				members.push_back(std::move(profile));
			}
			Q_EMIT chatMembersLoaded(chatId, members);
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

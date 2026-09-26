/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#pragma once

#include "mtslink/api/api_messages.h"

#include <QObject>
#include <QHash>

namespace MtsLink {
class Rpc;
} // namespace MtsLink

namespace MtsLink::Api {

class Users final : public QObject {
	Q_OBJECT

public:
	explicit Users(Rpc *rpc, QObject *parent = nullptr);

	void loadMember(const UserId &userId);
	void loadOrganizationMembers(int offset = 0, int limit = 100);
	void loadChatMembers(const ChatId &chatId);

	[[nodiscard]] MemberProfile cachedProfile(const UserId &userId) const;
	void cacheProfiles(const QList<MemberProfile> &profiles);

Q_SIGNALS:
	void memberLoaded(const MemberProfile &profile);
	void membersLoaded(const QList<MemberProfile> &members);
	void chatMembersLoaded(
		const ChatId &chatId,
		const QList<MemberProfile> &members);
	void presenceChanged(
		const UserId &userId,
		MemberPresence presence);

private:
	Rpc *_rpc = nullptr;
	QHash<UserId, MemberProfile> _cache;
};

} // namespace MtsLink::Api

/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#pragma once

#include "mtslink/types.h"
#include "mtslink/api/api_messages.h"

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <functional>

namespace MtsLink {
class Rpc;
} // namespace MtsLink

namespace MtsLink::Api {

struct ChannelData {
	ChatId id;
	QString name;
	QString description;
	ChatType type;
	OrganizationId organizationId;
	int unreadCount = 0;
	QString lastMessageId;
	QString lastMessageText;
	qint64 lastMessageTimestamp = 0;
	QString avatarFileId;
	bool isMuted = false;
	bool isPinned = false;
	bool isReadOnly = false;
	bool isPublic = false;
	bool isPublicKnown = false; // Not in the GetMyChannelsV3 list items.
	int pinnedMessageCount = 0;
	QString memberRole;
	QString interlocutorId;
	int memberCount = 0;
	// Chat.GetChatV3: the profiles of the members (of the dialog).
	QList<QJsonObject> memberProfiles;
};

class Channels final : public QObject {
	Q_OBJECT

public:
	explicit Channels(Rpc *rpc, QObject *parent = nullptr);

	void loadMyChannels();
	void loadMyDialogsAndGroupChats();
	void loadChatInfo(const ChatId &chatId);
	void pinChat(const ChatId &chatId);
	void unpinChat(const ChatId &chatId);
	void createCall(
		const ChatId &chatId,
		const QString &name,
		std::function<void(QString joinLink)> done,
		std::function<void()> fail);

	// Channel membership and roles, done(ok) is called on the response.
	using Done = std::function<void(bool ok)>;
	void addAdministrators(
		const ChatId &chatId,
		const QStringList &userIds,
		Done done);
	void removeAdministrators(
		const ChatId &chatId,
		const QStringList &userIds,
		Done done);
	void giveOwnership(
		const ChatId &chatId,
		const QString &userId,
		Done done);
	void addUsers(
		const ChatId &chatId,
		const QStringList &userIds,
		const OrganizationId &organizationId,
		Done done);
	void removeUsers(
		const ChatId &chatId,
		const QStringList &userIds,
		const OrganizationId &organizationId,
		Done done);

	// done(nullopt) on failure.
	void createChannel(
		const QString &name,
		const QString &description,
		bool isPublic,
		bool isReadOnly,
		const OrganizationId &organizationId,
		std::function<void(std::optional<ChannelData>)> done);
	void addChannelCover(
		const ChatId &chatId,
		const FileId &fileId,
		Done done);
	void leaveChat(const ChatId &chatId, Done done);
	void deleteChannel(const ChatId &chatId, Done done);
	// Public channels.
	void joinChat(const ChatId &chatId, Done done);

	// Group chats: only an owner, no administrators.
	void createGroupChat(
		const QString &name,
		const OrganizationId &organizationId,
		const QStringList &userIds,
		std::function<void(std::optional<ChannelData>)> done);
	void addGroupChatCover(
		const ChatId &chatId,
		const FileId &fileId,
		Done done);
	void addGroupUsers(
		const ChatId &chatId,
		const QStringList &userIds,
		const OrganizationId &organizationId,
		Done done);
	void removeGroupUsers(
		const ChatId &chatId,
		const QStringList &userIds,
		const OrganizationId &organizationId,
		Done done);
	void giveGroupOwnership(
		const ChatId &chatId,
		const QString &userId,
		Done done);
	void deleteGroupChat(const ChatId &chatId, Done done);

	// Editing: channels need all the fields, group chats only the name.
	void updateChannel(
		const ChatId &chatId,
		const QString &name,
		const QString &description,
		bool isPublic,
		bool isReadOnly,
		Done done);
	void updateGroupChat(
		const ChatId &chatId,
		const QString &name,
		Done done);

	// Channels of the organization by name / description.
	void searchChannels(
		const QString &query,
		const OrganizationId &organizationId,
		int from,
		int size,
		std::function<void(QList<ChannelData>)> done);

	// Organization members that are not in the chat yet.
	using MembersDone = std::function<void(QList<MemberProfile>)>;
	void searchNonMembers(
		const ChatId &chatId,
		bool groupChat,
		const QString &query,
		int offset,
		int limit,
		MembersDone done);

Q_SIGNALS:
	void channelsLoaded(const QList<ChannelData> &channels);
	void dialogsLoaded(const QList<ChannelData> &dialogs);
	void chatInfoLoaded(const ChannelData &chat);
	void chatPinned(const ChatId &chatId, int pinPosition);
	void chatUnpinned(const ChatId &chatId);

private:
	void simpleCall(
		const QString &method,
		const QJsonObject &param,
		Done done);
	[[nodiscard]] ChannelData parseChat(const QJsonObject &obj) const;
	[[nodiscard]] ChatType parseChatType(const QString &type) const;

	Rpc *_rpc = nullptr;
};

} // namespace MtsLink::Api

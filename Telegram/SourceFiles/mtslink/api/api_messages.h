/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#pragma once

#include "mtslink/types.h"

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>

namespace MtsLink {
class Rpc;
} // namespace MtsLink

namespace MtsLink::Api {

struct MemberProfile {
	UserId userId;
	OrganizationId organizationId;
	QString email;
	QString phone;
	QString position;
	QString department;
	QString firstName;
	QString lastName;
	QString displayName;
	MemberPresence presence;
	QString avatarFileId;
	MemberRole role;
};

struct FileData {
	FileId id;
	QString name;
	QString url;
	qint64 size = 0;
	QString mime;
	int width = 0;
	int height = 0;
};

struct MentionInfo {
	UserId userId;
	QString name;
};

struct MessageData {
	MessageId id;
	ChatId chatId;
	UserId authorId;
	QString text;
	QString markdown;
	MessageType type;
	QJsonArray blocks;
	QList<FileData> files;
	QList<MentionInfo> mentions;
	qint64 createdAt = 0;
	qint64 updatedAt = 0;
	bool isDeleted = false;
	MessageId repliedMessageId;
	MessageId parentId;
	int threadChildrenCount = 0;
	int threadUnreadCount = 0;
};

class Messages final : public QObject {
	Q_OBJECT

public:
	explicit Messages(Rpc *rpc, QObject *parent = nullptr);

	void load(
		const ChatId &chatId,
		const MessageId &fromMessageId = {},
		int limit = 50);

	[[nodiscard]] bool isLoading(const ChatId &chatId) const;

	void search(
		const ChatId &chatId,
		const QString &query,
		int limit = 50);

	void loadPinned(const ChatId &chatId, int limit = 50);

	void loadThread(
		const ChatId &chatId,
		const MessageId &parentId,
		const MessageId &fromMessageId = {},
		int limit = 50);

Q_SIGNALS:
	void messagesLoaded(
		const ChatId &chatId,
		const QList<MessageData> &messages,
		const QList<MemberProfile> &profiles,
		const QString &rawLastId,
		int rawCount);
	void olderMessagesLoaded(
		const ChatId &chatId,
		const QList<MessageData> &messages,
		const QList<MemberProfile> &profiles,
		const QString &rawLastId,
		int rawCount);
	void searchCompleted(
		const ChatId &chatId,
		const QList<MessageData> &messages,
		const QList<MemberProfile> &profiles,
		int total);
	void pinnedMessagesLoaded(
		const ChatId &chatId,
		const QList<MessageData> &messages,
		const QList<MemberProfile> &profiles,
		int total);
	void threadMessagesLoaded(
		const ChatId &chatId,
		const MessageId &parentId,
		const QList<MessageData> &messages,
		const QList<MemberProfile> &profiles);

private:
	[[nodiscard]] MessageData parseMessage(const QJsonObject &obj) const;
	[[nodiscard]] FileData parseFile(const QJsonObject &obj) const;
	[[nodiscard]] MemberProfile parseProfile(const QJsonObject &obj) const;

	Rpc *_rpc = nullptr;
	QSet<ChatId> _loadingChats;
};

} // namespace MtsLink::Api

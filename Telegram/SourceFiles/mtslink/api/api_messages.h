/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#pragma once

#include "mtslink/types.h"

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QHash>
#include <optional>

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
	MemberPresence presence = MemberPresence::Unknown;
	QString avatarFileId;
	MemberRole role;
	int inCall = -1; // -1 unknown, 0 / 1 when reported by the server.
	// "customStatus" {emoji, status, setting, expiresAt}, if it was sent.
	QJsonObject customStatus;
	bool customStatusKnown = false;
	// The full organization profile was sent: position, department and
	// the organization fields ({profileFieldId, value}) are known.
	QList<QPair<QString, QString>> additionalFields;
	bool detailsKnown = false;
};

struct FileData {
	FileId id;
	QString name;
	QString url;
	qint64 size = 0;
	QString mime;
	int width = 0;
	int height = 0;
	// A voice message ("voiceMeta"): duration in seconds, amplitudes 0-255.
	bool voice = false;
	int duration = 0;
	QVector<int> waveform;
};

struct MentionInfo {
	UserId userId;
	QString name;
};

struct ReactionData {
	QString emojiId;
	QString emoji;
	int count = 0;
	bool selected = false;
};

struct ForwardInfo {
	UserId authorId;
	MessageId messageId;
	ChatId chatId;
	qint64 createdAt = 0;
};

struct CallMetadata {
	QString status;
	QString joinLink;
	QString webinarEventId; // For the personal link of the joining user.
	int duration = 0;
	QString statusReason; // "None", "Declined", ...
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
	QList<ReactionData> reactions;
	qint64 createdAt = 0;
	qint64 updatedAt = 0;
	bool isDeleted = false;
	// Read by the others (for the author's own messages), when known.
	std::optional<bool> isRead;
	MessageId repliedMessageId;
	MessageId parentId;
	int threadChildrenCount = 0;
	int threadUnreadCount = 0;
	std::optional<ForwardInfo> forward;
	std::optional<CallMetadata> callMeta;
};

[[nodiscard]] FileData ParseFileData(const QJsonObject &obj);
[[nodiscard]] MemberProfile ParseMemberProfile(const QJsonObject &obj);
// From the "profile" object of an organization member.
void ParseProfileDetails(const QJsonObject &profile, MemberProfile &result);

class Messages final : public QObject {
	Q_OBJECT

public:
	explicit Messages(Rpc *rpc, QObject *parent = nullptr);

	void load(
		const ChatId &chatId,
		const MessageId &fromMessageId = {},
		int limit = 50);

	void loadPreview(const ChatId &chatId, int limit);

	// The newest messages are loading (not the older ones: the refresh
	// on opening a chat was skipped while its history was loading).
	[[nodiscard]] bool isLoading(const ChatId &chatId) const;
	[[nodiscard]] bool isLoadingOlder(const ChatId &chatId) const;
	// The newest messages were loaded at least once in this session (a
	// failed or timed out request is repeated when the chat is opened).
	[[nodiscard]] bool loadedOnce(const ChatId &chatId) const;
	void retryFailedLoads();

	void search(
		const ChatId &chatId,
		const QString &query,
		int limit = 50);

	void loadPinned(const ChatId &chatId, int limit = 50);
	void reloadPinned(const ChatId &chatId);

	void loadThread(
		const ChatId &chatId,
		const MessageId &parentId,
		const MessageId &fromMessageId = {},
		int limit = 50);

	void loadAround(
		const ChatId &chatId,
		const MessageId &messageId,
		int limit = 50);

	// A page of a thread, "Before" or "After" the given message (the
	// first replies: after the parent itself), done(messages, profiles).
	using ThreadPageDone = std::function<void(
		QList<MessageData>,
		QList<MemberProfile>)>;
	void loadThreadPage(
		const ChatId &chatId,
		const MessageId &parentId,
		const MessageId &fromMessageId,
		bool after,
		int limit,
		ThreadPageDone done);

	// Search in all my chats, done(messages, profiles, total, rawCount).
	using GlobalSearchDone = std::function<void(
		QList<MessageData>,
		QList<MemberProfile>,
		int,
		int)>;
	void searchGlobal(
		const QString &query,
		const OrganizationId &organizationId,
		int from,
		int size,
		GlobalSearchDone done);

	[[nodiscard]] MessageId newestRawId(const ChatId &chatId) const {
		return _newestRawIds.value(chatId);
	}

Q_SIGNALS:
	void deletedMessageSeen(const ChatId &chatId, const MessageId &messageId);
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
	void previewLoaded(
		const ChatId &chatId,
		const QList<MessageData> &messages,
		const QList<MemberProfile> &profiles,
		int rawCount,
		int limit);
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
	void aroundMessagesLoaded(
		const ChatId &chatId,
		const MessageId &targetId,
		const QList<MessageData> &messages,
		const QList<MemberProfile> &profiles);
private:
	[[nodiscard]] MessageData parseMessage(const QJsonObject &obj) const;
	[[nodiscard]] FileData parseFile(const QJsonObject &obj) const;
	[[nodiscard]] MemberProfile parseProfile(const QJsonObject &obj) const;

	Rpc *_rpc = nullptr;
	QSet<ChatId> _loadingChats;
	QSet<ChatId> _loadingOlderChats;
	// The newest message of the chat including deleted ones: the server
	// counts a deleted newest message as unread until it is read.
	QHash<ChatId, MessageId> _newestRawIds;
	QSet<ChatId> _failedChats;
	QSet<ChatId> _loadedOnceChats;
	QSet<ChatId> _loadingPinnedChats;
};

} // namespace MtsLink::Api

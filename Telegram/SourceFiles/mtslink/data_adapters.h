/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.

Bridges MTS Link API data into tdesktop's data model
(PeerData, History, HistoryItem).
*/
#pragma once

#include "data/data_peer_id.h"
#include "mtslink/api/api_channels.h"
#include "mtslink/api/api_messages.h"
#include "mtslink/api/api_threads.h"
#include "ui/text/text_entity.h"
#include "ui/style/style_core_types.h"

#include <QtNetwork/QNetworkCookie>
#include <QtGui/QImage>

namespace Ui {
struct GroupCallBarContent;
} // namespace Ui

namespace Main {
class Session;
} // namespace Main

class PeerData;
class ChannelData;
class UserData;
class History;
class HistoryItem;

namespace Data {
class RepliesList;
} // namespace Data

namespace MtsLink {

class Session;

void connectToSession(
	not_null<Main::Session*> mainSession,
	not_null<Session*> mtsSession);

[[nodiscard]] BareId uuidToBareId(const QString &uuid);
[[nodiscard]] PeerId chatIdToPeerId(const QString &chatId, ChatType type);
[[nodiscard]] PeerId chatIdToPeerId(const QString &chatId);
[[nodiscard]] QString peerIdToChatId(PeerId peerId);
[[nodiscard]] bool hasChatId(PeerId peerId);
void markReadRequestSent(const QString &chatId);
[[nodiscard]] bool consumeReadRequestSent(const QString &chatId);
[[nodiscard]] ChatType chatTypeForPeer(PeerId peerId);
[[nodiscard]] PeerId favoritesPeerId();
[[nodiscard]] bool isThreadPeer(PeerId peerId);
[[nodiscard]] QPair<PeerId, MsgId> threadParentInfo(PeerId threadPeerId);
[[nodiscard]] QPair<ChatId, MessageId> threadTopicInfo(MsgId rootId);
[[nodiscard]] const QHash<PeerId, QPair<PeerId, MsgId>> &threadPeerMap();
[[nodiscard]] PeerId threadAuthorPeerId(PeerId threadPeerId);
[[nodiscard]] PeerId currentOpenThreadAuthor(PeerId parentPeerId);

void updateThreadParticipants(
	not_null<Main::Session*> session,
	PeerId parentPeerId,
	MsgId rootId,
	const std::vector<MsgId> &messageIds);

void applyPendingThreadUnreads(
	not_null<Main::Session*> session,
	PeerId chatPeerId);
[[nodiscard]] int pendingThreadUnreadCount(PeerId chatPeerId);
[[nodiscard]] MsgId firstPendingThreadUnreadParent(PeerId chatPeerId);
[[nodiscard]] QString firstPendingThreadUnreadUuid(PeerId chatPeerId);

[[nodiscard]] MsgId currentOpenThreadRoot(PeerId parentPeerId);
[[nodiscard]] PeerId threadPeerFor(PeerId parentPeerId, MsgId rootId);

// Pinned messages ordered by date (MsgIds of MTS Link are not chronological).
// Returns the pinned message to show: the newest one not newer than
// visibleBottomDate, or, after a click on clickedId, the previous one.
[[nodiscard]] MsgId pinnedToShow(
	PeerId peerId,
	TimeId visibleBottomDate,
	MsgId clickedId);
// Index of the pinned message in date order (0 = oldest), -1 if unknown.
[[nodiscard]] int pinnedDateIndex(PeerId peerId, MsgId msgId);
[[nodiscard]] TimeId pinnedDate(PeerId peerId, MsgId msgId);

// Loads a single message of the chat from the server, done() is always called.
void requestMessageData(
	not_null<Main::Session*> session,
	PeerId peerId,
	MsgId msgId,
	Fn<void()> done);
[[nodiscard]] bool isThreadSubscribed(
	not_null<Main::Session*> session,
	PeerId parentPeerId,
	MsgId rootId);
void setThreadSubscribed(
	not_null<Main::Session*> session,
	PeerId parentPeerId,
	MsgId rootId,
	bool subscribed);

[[nodiscard]] bool isThreadNotifiable(PeerId parentPeerId, MsgId rootId);

// Adds "subscribe / unsubscribe" and, when subscribed, "mute / unmute"
// thread actions through the given menu callback.
void fillThreadSubscriptionActions(
	not_null<Main::Session*> session,
	PeerId parentPeerId,
	MsgId rootId,
	Fn<void(const QString&, Fn<void()>, const style::icon*)> addAction,
	bool withSubscribe = true);
void setThreadNotifiable(
	not_null<Main::Session*> session,
	PeerId parentPeerId,
	MsgId rootId,
	bool notifiable);

// Unread counter of the thread entry in the "Threads" folder.
void addThreadEntryUnread(
	not_null<Main::Session*> session,
	PeerId parentPeerId,
	MsgId rootId,
	int delta,
	TimeId date);
void resetThreadEntryUnread(
	not_null<Main::Session*> session,
	PeerId parentPeerId,
	MsgId rootId);

void applyThreadsList(
	not_null<Main::Session*> session,
	const QList<Api::ThreadData> &threads);

void applyDialogData(
	not_null<Main::Session*> session,
	const Api::ChannelData &src);

void applyChannelData(
	not_null<Main::Session*> session,
	const Api::ChannelData &src);

void applyUserData(
	not_null<Main::Session*> session,
	const Api::MemberProfile &src);

HistoryItem *addMessage(
	not_null<Main::Session*> session,
	const Api::MessageData &src,
	bool threadOnly = false,
	std::vector<not_null<HistoryItem*>> *batchItems = nullptr);

[[nodiscard]] bool addOlderMessages(
	not_null<Main::Session*> session,
	const ChatId &chatId,
	const QList<Api::MessageData> &messages,
	const QList<Api::MemberProfile> &profiles,
	const MessageId &rawLastId,
	int rawCount);

void applyChatList(
	not_null<Main::Session*> session,
	const QList<Api::ChannelData> &channels);

void handleChatEvent(
	not_null<Main::Session*> session,
	const QString &dst,
	const QJsonObject &param);

void deleteMessage(
	not_null<Main::Session*> session,
	const ChatId &chatId,
	const MessageId &messageId);

void updateMessage(
	not_null<Main::Session*> session,
	const Api::MessageData &src);

[[nodiscard]] QString msgIdToMtsLinkId(PeerId peerId, MsgId msgId);
void registerMessageId(PeerId peerId, MsgId msgId, const QString &mtsLinkId);

[[nodiscard]] QString buildChatLink(PeerId peerId);
[[nodiscard]] QString buildMessageLink(
	not_null<HistoryItem*> item,
	bool inRepliesContext);
void shortenAndCopy(
	not_null<Main::Session*> session,
	const QString &fullUrl);

void registerThreadRoot(PeerId peerId, MsgId msgId, MsgId rootId);
[[nodiscard]] MsgId threadRootFor(PeerId peerId, MsgId msgId);

struct ThreadScrollState {
	FullMsgId itemId;
	TimeId date = 0;
	int shift = 0;
};
void saveThreadScroll(PeerId peerId, MsgId rootId, const ThreadScrollState &state);
[[nodiscard]] std::optional<ThreadScrollState> threadScroll(PeerId peerId, MsgId rootId);

void setCurrentOpenThread(PeerId peerId, MsgId rootId);
// Clears only if rootId is still the open one: a new thread view of the same
// chat is created before the previous one is destroyed.
void clearCurrentOpenThread(PeerId peerId, MsgId rootId);
[[nodiscard]] rpl::producer<> openThreadChanges();
[[nodiscard]] bool isThreadOpen(PeerId peerId, MsgId rootId);

void cacheRepliesList(PeerId peerId, MsgId rootId, std::shared_ptr<Data::RepliesList> replies);
[[nodiscard]] std::shared_ptr<Data::RepliesList> cachedRepliesList(PeerId peerId, MsgId rootId);

void fetchThreadLastRead(
	not_null<Main::Session*> session,
	PeerId peerId,
	MsgId rootId);

void setPendingTempMessage(PeerId peerId, MsgId msgId);
void addPendingThreadSend(const QString &clientId);
[[nodiscard]] bool takePendingThreadSend(const QString &clientId);
void clearPendingTempMessage(
	not_null<Main::Session*> session,
	PeerId peerId);
bool replacePendingWithReal(
	not_null<Main::Session*> session,
	PeerId peerId,
	const Api::MessageData &realMsg);

void setOldestLoadedMessageId(PeerId peerId, const QString &mtsLinkId);
[[nodiscard]] QString oldestLoadedMessageId(PeerId peerId);

void saveMessagesToCache(
	not_null<Main::Session*> session,
	const QString &chatId,
	const QList<Api::MessageData> &messages,
	const QList<Api::MemberProfile> &profiles);
void loadMessagesFromCache(
	not_null<Main::Session*> session,
	const QString &chatId);

void saveChatListToCache(
	not_null<Main::Session*> session,
	const QList<Api::ChannelData> &channels);
void loadChatListFromCache(
	not_null<Main::Session*> session);

void saveFiltersToCache(not_null<Main::Session*> session);
void loadFiltersFromCache(
	not_null<Main::Session*> session,
	Fn<void(bool loaded)> done);

void setFileAuthToken(const QString &token);
[[nodiscard]] QString fileAuthToken();

void setFileRefreshToken(const QString &token);
[[nodiscard]] QString fileRefreshToken();

void setFileAuthCookies(const QList<QNetworkCookie> &cookies);
[[nodiscard]] QList<QNetworkCookie> fileAuthCookies();

void requestTokenRefresh();
void setTokenRefreshCallback(std::function<void()> callback);

struct MtsLinkMessageContent {
	QString text;
	QJsonArray blocks;
	QJsonArray mentionsMeta;
};

[[nodiscard]] MtsLinkMessageContent convertMentionsForSending(
	const TextWithTags &textWithTags,
	not_null<Main::Session*> session);

[[nodiscard]] TextWithEntities parseMentionedText(
	const QString &text,
	const QString &markdown,
	const QList<Api::MentionInfo> &mentions,
	not_null<Main::Session*> session);

[[nodiscard]] QString userBareIdToUuid(uint64 bareId);

void setEmojiMapping(const QHash<QString, QString> &emojiToId);
void setEmojiIdMapping(const QString &emojiId, const QString &emoji);
[[nodiscard]] QString emojiToId(const QString &emoji);
[[nodiscard]] QString idToEmoji(const QString &emojiId);

[[nodiscard]] std::vector<not_null<UserData*>> chatMtsLinkUsers(
	not_null<Main::Session*> session,
	PeerId chatPeerId);

[[nodiscard]] bool isMtsLinkUrl(const QString &url);
void handleMtsLinkUrl(
	const QString &url,
	const QVariant &context);

[[nodiscard]] bool isUserInCall(PeerId userPeerId);

// Typing: subscribe when a chat / thread is opened, send while typing.
void subscribeTyping(
	not_null<Main::Session*> session,
	PeerId peerId,
	MsgId rootId = 0);
void sendTyping(
	not_null<Main::Session*> session,
	PeerId peerId,
	MsgId rootId = 0);

// Channel roles: "Owner", "Admin", "Member" or empty when unknown.
[[nodiscard]] QString myChannelRole(PeerId channelPeerId);
[[nodiscard]] PeerId channelOwner(PeerId channelPeerId);
[[nodiscard]] bool isChannelAdmin(PeerId channelPeerId, PeerId userPeerId);

// Channel management, the result arrives as server events.
void setChannelAdmin(
	not_null<Main::Session*> session,
	not_null<PeerData*> channel,
	not_null<UserData*> user,
	bool admin);
void giveChannelOwnership(
	not_null<Main::Session*> session,
	not_null<PeerData*> channel,
	not_null<UserData*> user);
void removeChannelMember(
	not_null<Main::Session*> session,
	not_null<PeerData*> channel,
	not_null<UserData*> user);
void inviteChannelMembers(
	not_null<Main::Session*> session,
	not_null<PeerData*> channel,
	const std::vector<not_null<UserData*>> &users);
// Creates an MTS Link channel, done(nullptr) on failure.
void createChannel(
	not_null<Main::Session*> session,
	const QString &title,
	const QString &description,
	bool isPublic,
	bool isReadOnly,
	QImage cover,
	Fn<void(ChannelData*)> done);
void leaveChannel(
	not_null<Main::Session*> session,
	not_null<ChannelData*> channel);
// Public channels: Chat.JoinToChat.
void joinChannel(
	not_null<Main::Session*> session,
	not_null<ChannelData*> channel);
// Group chat with the members, done(nullptr) on failure.
void createGroupChat(
	not_null<Main::Session*> session,
	const QString &title,
	const std::vector<not_null<UserData*>> &users,
	QImage cover,
	Fn<void(ChannelData*)> done);
// Organization members for a new group, without me.
void searchOrganizationMembers(
	not_null<Main::Session*> session,
	const QString &query,
	Fn<void(std::vector<not_null<UserData*>>)> done);
[[nodiscard]] bool isGroupChat(PeerId peerId);
// The server rejects emoji in channel and group chat names.
[[nodiscard]] bool containsEmoji(const QString &text);
// Public channels can be found and joined by everyone in the organization.
[[nodiscard]] rpl::producer<bool> channelPublicValue(PeerId channelPeerId);
void requestChatInfo(not_null<Main::Session*> session, PeerId peerId);
// Saves the edited info, done(ok) after the info request, the cover is
// uploaded after that.
void updateChatInfo(
	not_null<Main::Session*> session,
	not_null<ChannelData*> channel,
	const QString &title,
	const QString &description,
	bool isPublic,
	bool isReadOnly,
	QImage cover,
	Fn<void(bool ok)> done);
// Members with roles, the result comes as PeerUpdate::Flag::Members.
void reloadChannelMembers(
	not_null<Main::Session*> session,
	PeerId channelPeerId);
void deleteChannel(
	not_null<Main::Session*> session,
	not_null<ChannelData*> channel);

// Global search: my channels and organization members.
void searchPeersGlobal(
	not_null<Main::Session*> session,
	const QString &query,
	Fn<void(
		std::vector<not_null<PeerData*>> my,
		std::vector<not_null<PeerData*>> peers)> done);
// Global messages search, done(items, total, full).
void searchMessagesGlobal(
	not_null<Main::Session*> session,
	const QString &query,
	int offset,
	int limit,
	Fn<void(std::vector<not_null<HistoryItem*>>, int, bool)> done);

// Organization members not in the channel, empty query lists everyone.
void searchChannelNonMembers(
	not_null<Main::Session*> session,
	not_null<PeerData*> channel,
	const QString &query,
	Fn<void(std::vector<not_null<UserData*>>)> done);

// Creates an MTS Link video conference for the chat and opens its link.
void startCall(not_null<Main::Session*> session, not_null<PeerData*> peer);

void setActiveCall(
	not_null<Main::Session*> session,
	PeerId peerId,
	const QString &joinLink);
[[nodiscard]] QString activeCallJoinLink(PeerId peerId);
[[nodiscard]] rpl::producer<Ui::GroupCallBarContent> activeCallBarContent(
	PeerId peerId);

} // namespace MtsLink

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
class PeerNotifySettings;
enum class DefaultNotify : uint8_t;
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
// The own messages not read by the others yet ("isRead" of the server).
[[nodiscard]] bool isOutboxUnread(PeerId peerId, MsgId msgId);
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

// The organization profile of a user, shown in the profile section.
struct UserDetails {
	QString email;
	QString position;
	QString department;
	// Organization profile fields: {profileFieldId, value}.
	std::vector<std::pair<QString, QString>> additional;
	bool full = false; // Loaded from the full organization profile.
};
struct ProfileFieldInfo {
	QString id;
	QString title;
};
[[nodiscard]] rpl::producer<UserDetails> userDetailsValue(
	not_null<UserData*> user);
[[nodiscard]] const std::vector<ProfileFieldInfo> &profileFields();
[[nodiscard]] rpl::producer<QString> profileFieldTitle(const QString &title);
// Loads the full profile and the organization fields once.
void requestUserDetails(not_null<UserData*> user);

// The materials of a finished call (summary, record) in the cache.
void saveCallMaterialsToCache(
	not_null<Main::Session*> session,
	const QString &eventId,
	bool done,
	const QList<Api::MessageData> &messages);
void loadCallMaterialsFromCache(
	not_null<Main::Session*> session,
	const QString &eventId,
	Fn<void(bool done, QList<Api::MessageData> messages)> done);

// Scheduled messages (MessageScheduler): the remote ids of
// Data::ScheduledMessages are made from the scheduled message uuids.
void requestScheduledMessages(
	not_null<Main::Session*> session,
	PeerId peerId,
	Fn<void(QVector<MTPMessage>)> done);
void createScheduledMessage(
	not_null<Main::Session*> session,
	PeerId peerId,
	const QString &markdown,
	const QStringList &fileIds,
	TimeId date);
void deleteScheduledMessages(
	not_null<Main::Session*> session,
	const QVector<MsgId> &remoteIds);
void sendScheduledMessagesNow(
	not_null<Main::Session*> session,
	const QVector<MsgId> &remoteIds);
// No editing in MessageScheduler: deleted and created again.
void rescheduleMessage(
	not_null<Main::Session*> session,
	MsgId remoteId,
	TimeId date,
	std::optional<QString> markdown = std::nullopt);
void handleSchedulerEvent(
	not_null<Main::Session*> session,
	const QJsonObject &param);

// The unread count of the thread row ("Threads" folder), from the server
// thread counter, or -1 if there is no row for that thread.
[[nodiscard]] int threadEntryUnreadCount(PeerId parentPeerId, MsgId rootId);

// The local messages being sent: the clock till the server sends them
// back (by the client id), sent again after a reconnect, failed on an error.
void markLocalSending(
	not_null<Main::Session*> session,
	PeerId peerId,
	MsgId localId);
void markLocalFailed(
	not_null<Main::Session*> session,
	PeerId peerId,
	MsgId localId);
QString trackSend(
	not_null<Main::Session*> session,
	PeerId peerId,
	MsgId localId,
	const QString &chatId,
	const QString &text,
	const QJsonArray &blocks,
	const QJsonArray &mentionsMeta,
	const QString &replyToId,
	const QStringList &fileIds,
	const QString &parentId,
	QString clientId = QString());

// The content of an uploaded file in the file cache of its document: the
// sent message shows it, not downloads it back from the server.
void rememberUploadedContent(
	not_null<Main::Session*> session,
	DocumentId localDocumentId,
	const QString &fileId,
	const QByteArray &content);

// The local stickers (the favorites of the stickers panel): images of the
// messages, kept with their content in the session cache.
[[nodiscard]] bool canAddPhotoToStickers(not_null<PhotoData*> photo);
// An image sent as a file: shown as a file (kept in the session cache).
void rememberImageAsFile(
	not_null<Main::Session*> session,
	const QString &fileId);
void restoreImagesAsFiles(not_null<Main::Session*> session);

// The image content in the cache by the CDN urls of a file (the preview of
// a photo or of a document): shown without loading from the server.
void putToImageCache(
	not_null<Main::Session*> session,
	const QString &fileId,
	const QByteArray &bytes);
// A local sticker, or a sticker of a message (kept as a local one).
bool toggleFavedSticker(not_null<DocumentData*> document, bool faved);
void addPhotoToStickers(not_null<PhotoData*> photo);
void restoreLocalStickers(not_null<Main::Session*> session);
// A local sticker pack: a zip with images, or the images of a folder.
void importStickerPack(
	not_null<Main::Session*> session,
	QPointer<QWidget> parent,
	Fn<void(QString)> showToast);

// The saved GIFs (the GIFs panel): local only, kept in the session cache.
void restoreSavedGifs(not_null<Main::Session*> session);
// Sends a GIF of the panel by its MTS Link file, false if it is not known.
bool sendSavedGif(
	not_null<Main::Session*> session,
	not_null<History*> history,
	not_null<DocumentData*> document,
	MsgId replyToId,
	MsgId topicRootId);


// The local messages of the call materials in the call thread.
[[nodiscard]] std::vector<MsgId> callMaterialMessages(
	PeerId peerId,
	MsgId rootId);

// The default notification settings (private chats / groups / channels)
// are not on the MTS Link server, they are kept locally.
void saveDefaultNotify(
	Data::DefaultNotify type,
	const Data::PeerNotifySettings &value);
void restoreDefaultNotify(not_null<Main::Session*> session);

// The catalogue emoji by their names (ru and en), best matches first.
[[nodiscard]] std::vector<QString> searchEmojiByName(
	const QString &query,
	bool exact);
[[nodiscard]] QString emojiName(const QString &emoji);
// The full size photo of the current userpic, for the media viewer.
void ensureUserpicFor(not_null<PeerData*> peer, const QString &fileId);

// profileChanged: from MemberProfileChanged, the userpic is replaced.
void applyUserData(
	not_null<Main::Session*> session,
	const Api::MemberProfile &src,
	bool profileChanged = false);

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
	const Api::MessageData &realMsg,
	const QString &clientId = QString());
// The local messages lost with the connection: sent again.
void resendPendingSends(not_null<Main::Session*> session);

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

// The emoji of the user's custom status, empty if none or expired.
[[nodiscard]] QString userStatusEmoji(PeerId userPeerId);
// {emoji, status, setting, expiresAt} from a profile or an event.
void applyUserStatus(
	not_null<Main::Session*> session,
	const QString &userId,
	const QJsonObject &status);

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
// Compares the UUIDv6 message ids by time.
[[nodiscard]] bool isNewerMessageId(const QString &a, const QString &b);
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
// Without a confirmation, in the system browser or as set in the settings.
// The notification of an ongoing incoming call opens the call window.
[[nodiscard]] bool openIncomingCall(not_null<HistoryItem*> item);
void startCallNow(
	not_null<Main::Session*> session,
	not_null<PeerData*> peer,
	bool systemBrowser);

void setActiveCall(
	not_null<Main::Session*> session,
	PeerId peerId,
	const Api::CallMetadata &meta);
void setActiveCall(
	not_null<Main::Session*> session,
	PeerId peerId,
	const QString &joinLink);
[[nodiscard]] QString activeCallJoinLink(PeerId peerId);
[[nodiscard]] rpl::producer<Ui::GroupCallBarContent> activeCallBarContent(
	PeerId peerId);

} // namespace MtsLink

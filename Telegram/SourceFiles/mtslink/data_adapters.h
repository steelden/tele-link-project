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
#include "ui/text/text_entity.h"

#include <QtNetwork/QNetworkCookie>

namespace Main {
class Session;
} // namespace Main

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
[[nodiscard]] ChatType chatTypeForPeer(PeerId peerId);
[[nodiscard]] PeerId favoritesPeerId();

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
	bool threadOnly = false);

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

void registerThreadRoot(PeerId peerId, MsgId msgId, MsgId rootId);
[[nodiscard]] MsgId threadRootFor(PeerId peerId, MsgId msgId);

struct ThreadScrollState {
	FullMsgId itemId;
	TimeId date = 0;
	int shift = 0;
};
void saveThreadScroll(PeerId peerId, MsgId rootId, const ThreadScrollState &state);
[[nodiscard]] std::optional<ThreadScrollState> threadScroll(PeerId peerId, MsgId rootId);

void cacheRepliesList(PeerId peerId, MsgId rootId, std::shared_ptr<Data::RepliesList> replies);
[[nodiscard]] std::shared_ptr<Data::RepliesList> cachedRepliesList(PeerId peerId, MsgId rootId);

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

void setFileAuthToken(const QString &token);
[[nodiscard]] QString fileAuthToken();

void setFileAuthCookies(const QList<QNetworkCookie> &cookies);
[[nodiscard]] QList<QNetworkCookie> fileAuthCookies();

struct MtsLinkMessageContent {
	QString text;
	QJsonArray blocks;
	QJsonArray mentionsMeta;
};

[[nodiscard]] MtsLinkMessageContent convertMentionsForSending(
	const TextWithTags &textWithTags,
	not_null<Main::Session*> session);

[[nodiscard]] QString userBareIdToUuid(uint64 bareId);

void setEmojiMapping(const QHash<QString, QString> &emojiToId);
[[nodiscard]] QString emojiToId(const QString &emoji);
[[nodiscard]] QString idToEmoji(const QString &emojiId);

[[nodiscard]] std::vector<not_null<UserData*>> chatMtsLinkUsers(
	not_null<Main::Session*> session,
	PeerId chatPeerId);

} // namespace MtsLink

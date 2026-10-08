/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#include "mtslink/data_adapters.h"
#include "mtslink/session.h"
#include "mtslink/env_config.h"
#include "mtslink/my_profile.h"
#include "mtslink/call_window.h"

#include "main/main_session.h"
#include "data/components/scheduled_messages.h"
#include "api/api_text_entities.h"
#include "main/main_account.h"
#include "data/stickers/data_stickers.h"
#include "data/data_document_media.h"
#include "main/main_domain.h"
#include "storage/storage_account.h"
#include "storage/file_download.h"
#include "core/core_settings.h"
#include "dialogs/ui/dialogs_quick_action.h"
#include "history/view/history_view_element.h"
#include "data/stickers/data_stickers_set.h"
#include <QtGui/QImageReader>
#include <QtCore/QCollator>
#include "base/zlib_help.h"
#include "data/data_session.h"
#include "data/data_peer_values.h"
#include "data/data_channel.h"
#include "data/data_chat_participant_status.h"
#include "data/data_chat_filters.h"
#include "data/data_user.h"
#include "data/data_document.h"
#include "data/data_photo.h"
#include "data/data_changes.h"
#include "data/data_send_action.h"
#include "data/data_replies_list.h"
#include "data/data_message_reaction_id.h"
#include "data/data_message_reactions.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/history_item_helpers.h"
#include "history/history_item_reply_markup.h"
#include "lang/lang_keys.h"
#include "history/view/history_view_send_action.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_indexed_list.h"
#include "history/history_unread_things.h"
#include "base/timer.h"
#include "dialogs/dialogs_pinned_list.h"
#include "ui/text/format_values.h"
#include "ui/boxes/confirm_box.h"
#include "styles/style_menu_icons.h"
#include "styles/style_layers.h"
#include "styles/style_boxes.h"
#include "ui/widgets/labels.h"
#include "ui/layers/generic_box.h"
#include "dialogs/dialogs_key.h"
#include "data/data_lastseen_status.h"
#include "data/data_types.h"
#include "data/notify/data_notify_settings.h"
#include "data/notify/data_peer_notify_settings.h"
#include "core/application.h"
#include "lang/lang_instance.h"
#include "calls/calls_instance.h"
#include "calls/calls_call.h"
#include "window/notifications_manager.h"
#include "base/unixtime.h"
#include "settings.h"
#include "base/random.h"
#include "base/call_delayed.h"
#include "ui/image/image_location.h"
#include "ui/text/text_entity.h"
#include "ui/emoji_config.h"
#include "ui/chat/group_call_bar.h"
#include "ui/chat/group_call_userpics.h"
#include "storage/cache/storage_cache_database.h"
#include "storage/storage_shared_media.h"

#include "core/click_handler_types.h"
#include "core/file_utilities.h"
#include "window/window_session_controller.h"
#include "window/window_controller.h"
#include "mainwindow.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDataStream>
#include <QtCore/QBuffer>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtGui/QGuiApplication>
#include <QtGui/QClipboard>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtCore/QFile>
#include <QtCore/QUuid>
#include <QtCore/QDateTime>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkCookie>

namespace MtsLink {

constexpr auto kMtsLinkMsgCacheTag = uint64(0xBC01'0000'0000'0000ULL);
constexpr auto kMtsLinkThreadCacheTag = uint64(0xBC0D'0000'0000'0000ULL);

namespace {

QHash<PeerId, QString> PeerToChatMap;
QHash<QString, PeerId> ChatToPeerMap;
QHash<PeerId, ChatType> PeerToChatTypeMap;
QHash<quint64, QString> MsgIdToMtsLinkIdMap;
QHash<PeerId, QString> OldestLoadedMsgMap;
QHash<uint64, QString> UserBareIdToUuidMap;
QHash<PeerId, std::vector<uint64>> ChatMembersMap;
QHash<PeerId, QVector<MsgId>> PendingTempMessages;
QHash<QString, QPair<PeerId, MsgId>> MtsLinkIdToMsgMap;

// The local id of a message by its MTS Link id: a sent message keeps the
// id of its local message (a thread to it had no root otherwise).
[[nodiscard]] MsgId localMsgId(const QString &mtsLinkId) {
	const auto i = MtsLinkIdToMsgMap.constFind(mtsLinkId);
	return (i != MtsLinkIdToMsgMap.constEnd())
		? i->second
		: MsgId(uuidToBareId(mtsLinkId) & 0x7FFFFFFFLL);
}
QSet<QString> PinnedMessagesLoadedChats;
QSet<QString> ChatInfoLoadedChats;
QSet<QString> PendingThreadClientIds;
QHash<quint64, MsgId> ThreadRootMap;
QString FileAuthTokenValue;
QString FileRefreshTokenValue;
QList<QNetworkCookie> FileAuthCookies;
// The members of a chat loaded last time, they are not reloaded too often.
QHash<QString, crl::time> MembersLoadedAt;

void requestProfileFields(not_null<Main::Session*> session);
// Incoming call messages: their notifications open the incoming call.
base::flat_set<FullMsgId> CallMessages;
std::function<void()> TokenRefreshCallback;
bool TokenRefreshInProgress = false;
QHash<QString, QString> EmojiToIdMap;
std::vector<QString> EmojiCatalogueOrder; // The MTS Link catalogue order.
QHash<QString, QString> IdToEmojiMap;
bool EmojiMapsInitialized = false;

QHash<PeerId, QString> ActiveCallLinks;
rpl::event_stream<PeerId> ActiveCallChanges;
QString CachedMyUserId;
QString ProfileDataPath;

QHash<MsgId, QPair<ChatId, MessageId>> ThreadTopicMap;
QHash<PeerId, QPair<PeerId, MsgId>> ThreadPeerInfoMap;
QHash<PeerId, PeerId> ThreadAuthorMap;

struct PeerMsgKey {
	PeerId peer;
	MsgId msg;
	bool operator==(const PeerMsgKey &o) const {
		return peer == o.peer && msg == o.msg;
	}
};

[[nodiscard]] inline size_t qHash(const PeerMsgKey &k, size_t seed = 0) {
	return ::qHash(k.peer.value, seed) ^ ::qHash(k.msg.bare, seed);
}

QHash<PeerMsgKey, PeerId> ThreadReverseMap;
QHash<PeerId, MsgId> CurrentOpenThreads; // parent peer -> open root


QString myUserIdFilePath() {
	return ProfileDataPath + u"mtslink_userid"_q;
}

void saveCachedUserId(const QString &userId) {
	CachedMyUserId = userId;
	QFile f(myUserIdFilePath());
	if (f.open(QIODevice::WriteOnly)) {
		f.write(userId.toUtf8());
	}
}

void loadCachedUserId() {
	QFile f(myUserIdFilePath());
	if (f.open(QIODevice::ReadOnly)) {
		CachedMyUserId = QString::fromUtf8(f.readAll()).trimmed();
	}
}

QString emojiMapFilePath() {
	return ProfileDataPath + u"mtslink_emoji_map.json"_q;
}

void saveEmojiMaps() {
	QJsonObject obj;
	for (auto it = IdToEmojiMap.constBegin(); it != IdToEmojiMap.constEnd(); ++it) {
		obj[it.key()] = it.value();
	}
	QFile f(emojiMapFilePath());
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
	}
}

void loadEmojiMaps() {
	QFile f(emojiMapFilePath());
	if (!f.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto doc = QJsonDocument::fromJson(f.readAll());
	if (!doc.isObject()) {
		return;
	}
	const auto obj = doc.object();
	for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
		const auto &id = it.key();
		const auto emoji = it.value().toString();
		if (!id.isEmpty() && !emoji.isEmpty()) {
			IdToEmojiMap[id] = emoji;
			EmojiToIdMap[emoji] = id;
		}
	}
}

void ensureEmojiMapsInitialized() {
	if (EmojiMapsInitialized) {
		return;
	}
	EmojiMapsInitialized = true;
	loadEmojiMaps();
	static const struct { const char *id; const char *emoji; } kKnown[] = {
		{"1ee90646-44c1-6729-b7ec-e379ab852b3a", "\xF0\x9F\x98\x80"},     // 😀
		{"1ee90646-44c1-699f-b7ec-1c0820ce7d44", "\xF0\x9F\x98\x81"},     // 😁
		{"1ee90646-44c1-6a64-b7ec-98d17e93ec9a", "\xF0\x9F\x98\x86"},     // 😆
		{"1ee90646-44c1-6af6-b7ec-4344e5f5119d", "\xF0\x9F\x98\x85"},     // 😅
		{"1ee90646-44c2-6748-b7ec-12ecf6e3bd10", "\xF0\x9F\xA5\xB2"},     // 🥲
		{"1ee90646-44c2-6fd3-b7ec-9ff1a0c145a2", "\xF0\x9F\xA4\x94"},     // 🤔
		{"1ee90646-44c3-640d-b7ec-3f519cd5e68f", "\xF0\x9F\xAB\xA5"},     // 🫥
		{"1ee90646-44c3-6686-b7ec-ecbb4e188720", "\xF0\x9F\x98\x92"},     // 😒
		{"1ee90646-44c4-6908-b7ec-ecf3406eac09", "\xF0\x9F\xA4\x95"},     // 🤕
		{"1ee90646-44c5-6e54-b7ec-cf6de5283b5d", "\xF0\x9F\x98\xB0"},     // 😰
		{"1ee90646-44c5-6ede-b7ec-4ce6d5975ecf", "\xF0\x9F\x98\xA5"},     // 😥
		{"1ee90646-44c6-605a-b7ec-00979f9b910b", "\xF0\x9F\x98\xB1"},     // 😱
		{"1ee90646-44ca-6ff6-b7ec-52a330d3955f",
			"\xE2\x9D\xA4\xEF\xB8\x8F"},                                  // ❤️
		{"1ee90646-44cf-6b25-b7ec-0291b8cfb5c7", "\xF0\x9F\x91\x8C"},     // 👌
		{"1ee90646-44d4-6e10-b7ec-19333962b453", "\xF0\x9F\x91\x8D"},     // 👍
		{"1ee90646-44d7-6a1e-b7ec-6f78b6dd5ff4", "\xF0\x9F\xA4\x9D"},     // 🤝
		{"1ee90646-44db-62b5-b7ec-3423e195c7b0", "\xF0\x9F\x91\x80"},     // 👀
		{"1ee90646-4a50-657c-b7ec-28855313e42c", "\xF0\x9F\x8C\xB4"},     // 🌴
		{"1ee90646-4a5b-64fc-b7ec-64020d8be8e1", "\xF0\x9F\x94\xA5"},     // 🔥
		{"1ee90646-4a6e-6ecb-b7ec-d586504d174b", "\xE2\x9E\x95"},         // ➕
		{"1ee90646-4a6f-6ae5-b7ec-0ac07ed0e115", "\xE2\x9C\x85"},         // ✅
	};
	for (const auto &e : kKnown) {
		const auto id = QString::fromLatin1(e.id);
		const auto emoji = QString::fromUtf8(e.emoji);
		if (!IdToEmojiMap.contains(id)) {
			IdToEmojiMap[id] = emoji;
			EmojiToIdMap[emoji] = id;
		}
	}
	// Map ❤ without VS16 to the same emojiId as ❤️.
	const auto heartNoVs16 = QString::fromUtf8("\xE2\x9D\xA4");
	const auto heartId = QStringLiteral("1ee90646-44ca-6ff6-b7ec-52a330d3955f");
	if (!EmojiToIdMap.contains(heartNoVs16)) {
		EmojiToIdMap[heartNoVs16] = heartId;
	}
	// The full MTS Link catalogue (mf-host-orchestrator emojiUtils), the
	// emoji there are without U+FE0F as the client strips it.
	QFile catalogue(u":/mtslink/emoji_ids.txt"_q);
	if (catalogue.open(QIODevice::ReadOnly)) {
		auto count = 0;
		for (const auto &line : QString::fromUtf8(
				catalogue.readAll()).split(QChar(10))) {
			const auto parts = line.split(QChar(9));
			if (parts.size() != 2 || parts[1].trimmed().isEmpty()) {
				continue;
			}
			const auto emoji = parts[0];
			const auto id = parts[1].trimmed();
			if (!IdToEmojiMap.contains(id)) {
				IdToEmojiMap[id] = emoji;
			}
			if (!EmojiToIdMap.contains(emoji)) {
				EmojiToIdMap[emoji] = id;
			}
			EmojiCatalogueOrder.push_back(emoji);
			++count;
		}
		LOG(("MtsLink Emoji: catalogue of %1 emoji").arg(count));
	}
}
PeerId FavoritesPeerIdValue = PeerId(0);

// The names of the catalogue emoji (Unicode CLDR annotations, ru and en):
// the emoji keywords of Telegram come by MTProto, there are none here.
struct EmojiNames {
	QString emoji;
	QStringList keywords; // The name first, normalized.
	QString name; // The name in the current language.
};

[[nodiscard]] QString NormalizeEmojiName(QString text) {
	return text.trimmed().toLower().replace(QChar(0x0451), QChar(0x0435));
}

[[nodiscard]] const std::vector<EmojiNames> &EmojiNamesList() {
	static auto result = [] {
		auto list = std::vector<EmojiNames>();
		auto file = QFile(u":/mtslink/emoji_names.txt"_q);
		if (!file.open(QIODevice::ReadOnly)) {
			return list;
		}
		const auto ru = Lang::GetInstance().id().startsWith(u"ru"_q);
		for (const auto &line : QString::fromUtf8(
				file.readAll()).split(QChar(10))) {
			const auto parts = line.split(QChar(9));
			if (parts.size() < 3 || parts[0].isEmpty()) {
				continue;
			}
			auto entry = EmojiNames{ .emoji = parts[0] };
			const auto names = std::array{
				parts[ru ? 1 : 2].split(QChar('|'), Qt::SkipEmptyParts),
				parts[ru ? 2 : 1].split(QChar('|'), Qt::SkipEmptyParts),
			};
			entry.name = names[0].isEmpty()
				? (names[1].isEmpty() ? QString() : names[1].front())
				: names[0].front();
			for (const auto &list : names) {
				for (const auto &word : list) {
					entry.keywords.push_back(NormalizeEmojiName(word));
				}
			}
			list.push_back(std::move(entry));
		}
		LOG(("MtsLink Emoji: %1 emoji names").arg(list.size()));
		return list;
	}();
	return result;
}

struct PendingChatEvent {
	QString dst;
	QJsonObject param;
};
QHash<QString, QList<PendingChatEvent>> PendingChatEvents;
QHash<QString, QList<PendingChatEvent>> PendingUserEvents;
QSet<QString> ChatInfoRequested;
// The chats the user was just added to: placed in the chat list by the
// time of joining (as Telegram does), not by their (unknown) last message.
QSet<QString> JustJoinedChats;
QSet<QString> UserProfileRequested;
// Authors whose messages are shown without waiting for the profile.
QSet<QString> AuthorWaitExpired;
QSet<QString> ReadRequestSentChats;
struct PendingThreadUnreadInfo {
	int count = 0;
	QString parentUuid;
};
QMap<QPair<PeerId, MsgId>, PendingThreadUnreadInfo> PendingThreadUnread;
constexpr auto kPreviewRetryLimit = 20;
QHash<PeerId, MsgId> PendingChatThreadScroll;
bool FolderPinsSyncScheduled = false;
QSet<PeerId> InCallUsers;
QHash<PeerId, QString> MyChannelRoles; // channel -> "Owner"/"Admin"/"Member"
QHash<PeerId, PeerId> ChannelOwners; // channel -> owner user
QSet<QString> MembersReloadScheduled;
QHash<PeerId, QHash<PeerId, MemberRole>> ChatMembersRoles;
QSet<QString> SelfLeavingChats; // Our own leave, no "removed" toast.
QHash<PeerId, bool> ChannelPublic;
rpl::event_stream<PeerId> ChannelPublicChanges;

void setChannelPublic(PeerId peerId, bool isPublic) {
	const auto i = ChannelPublic.constFind(peerId);
	if (i == ChannelPublic.constEnd() || *i != isPublic) {
		ChannelPublic.insert(peerId, isPublic);
		ChannelPublicChanges.fire_copy(peerId);
	}
}
// Pinned messages of a chat ordered by date, MsgIds are not chronological.
QHash<PeerId, std::vector<std::pair<TimeId, MsgId>>> PinnedByDate;
QSet<PeerId> PresenceKnownUsers;
QSet<PeerId> PresenceRequestedUsers;
QHash<PeerId, bool> ThreadNotifiable; // thread peer -> isNotifiable
QSet<QString> ThreadLoadRequested;

// MTS Link reports presence explicitly: a user stays online until
// MemberOffline, so the online status must not expire on its own.
constexpr auto kMtsLinkOnlineHorizon = TimeId(365 * 86400);

// MTS Link pins are global, so every folder pins the globally pinned
// chats it contains, in the main list order.
void syncFolderPins(not_null<Main::Session*> session) {
	using Flag = Data::ChatFilter::Flag;
	auto &filters = session->data().chatsFilters();
	const auto &mainOrder = session->data().chatsList()->pinned()->order();
	auto updates = std::vector<Data::ChatFilter>();
	for (const auto &filter : filters.list()) {
		if (!filter.id() || (filter.flags() & Flag::Threads)) {
			continue;
		}
		auto pinned = std::vector<not_null<History*>>();
		for (const auto &key : mainOrder) {
			if (const auto history = key.history()) {
				if (filter.contains(history)) {
					pinned.push_back(history);
				}
			}
		}
		if (pinned != filter.pinned()) {
			updates.push_back(Data::ChatFilter(
				filter.id(),
				filter.title(),
				filter.iconEmoji(),
				filter.colorIndex(),
				filter.flags(),
				filter.always(),
				std::move(pinned),
				filter.never()));
		}
	}
	for (auto &filter : updates) {
		filters.set(std::move(filter));
	}
}

void scheduleFolderPinsSync(not_null<Main::Session*> session) {
	if (FolderPinsSyncScheduled) {
		return;
	}
	FolderPinsSyncScheduled = true;
	crl::on_main(session, [=] {
		FolderPinsSyncScheduled = false;
		syncFolderPins(session);
	});
}

[[nodiscard]] QString privateCdnThumbBase() {
	return EnvConfig::instance().privateCdnMediaUrl() + u"/thumb_"_q;
}
[[nodiscard]] QString avatarCdnBase() {
	return EnvConfig::instance().publicCdnMediaUrl() + u"/thumb_"_q;
}
[[nodiscard]] QString fileDownloadBase() {
	return EnvConfig::instance().baseMediaUrl() + u"/file/"_q;
}

quint64 makeMsgKey(PeerId peerId, MsgId msgId) {
	return (quint64(peerId.value) ^ (quint64(msgId.bare) << 32));
}

bool isImageMime(const QString &mime) {
	return mime.startsWith(u"image/"_q);
}

std::optional<MTPMessageReactions> buildMtpReactions(
		const QList<Api::ReactionData> &reactions) {
	if (reactions.isEmpty()) {
		return std::nullopt;
	}
	ensureEmojiMapsInitialized();

	auto results = QVector<MTPReactionCount>();
	results.reserve(reactions.size());
	int chosenIdx = 0;

	for (const auto &r : reactions) {
		auto emoji = r.emoji;
		if (emoji.isEmpty()) {
			emoji = IdToEmojiMap.value(r.emojiId);
		}
		const auto knownEmoji = !emoji.isEmpty();
		if (emoji.isEmpty() && !r.emojiId.isEmpty()) {
			emoji = r.emojiId;
		}
		if (emoji.isEmpty()) {
			continue;
		}
		if (knownEmoji && !r.emojiId.isEmpty()
			&& !IdToEmojiMap.contains(r.emojiId)) {
			IdToEmojiMap[r.emojiId] = emoji;
			EmojiToIdMap[emoji] = r.emojiId;
			saveEmojiMaps();
		}
		auto rcFlags = MTPDreactionCount::Flags(0);
		int order = 0;
		if (r.selected) {
			rcFlags |= MTPDreactionCount::Flag::f_chosen_order;
			order = chosenIdx++;
		}
		results.push_back(MTP_reactionCount(
			MTP_flags(rcFlags),
			MTP_int(order),
			MTP_reactionEmoji(MTP_string(emoji)),
			MTP_int(r.count)));
	}

	if (results.isEmpty()) {
		return std::nullopt;
	}

	const auto mrFlags = MTPDmessageReactions::Flags(
		MTPDmessageReactions::Flag::f_can_see_list);
	return MTP_messageReactions(
		MTP_flags(mrFlags),
		MTP_vector<MTPReactionCount>(results),
		MTP_vector<MTPMessagePeerReaction>(),
		MTP_vector<MTPMessageReactor>());
}

void reapplyPhotoUrls(
		not_null<HistoryItem*> item,
		const Api::FileData &file) {
	if (!isImageMime(file.mime) || file.width <= 0 || file.height <= 0) {
		return;
	}
	const auto media = item->media();
	if (!media) {
		return;
	}
	const auto photo = media->photo();
	if (!photo) {
		return;
	}

	const auto thumbUrl = privateCdnThumbBase() + file.id + u"_s.jpg"_q;
	const auto fullUrl = privateCdnThumbBase() + file.id + u".jpg"_q;

	const auto thumbLocation = ImageLocation(
		DownloadLocation{ PlainUrlLocation{ thumbUrl } },
		file.width, file.height);
	const auto fullLocation = ImageLocation(
		DownloadLocation{ PlainUrlLocation{ fullUrl } },
		file.width, file.height);

	photo->updateImages(
		QByteArray(),
		ImageWithLocation{},
		ImageWithLocation{ .location = thumbLocation },
		ImageWithLocation{ .location = fullLocation },
		ImageWithLocation{},
		ImageWithLocation{},
		crl::time(0));
	photo->clearFailed(Data::PhotoSize::Small);
	photo->clearFailed(Data::PhotoSize::Thumbnail);
	photo->clearFailed(Data::PhotoSize::Large);
}

// The GIF files by their documents: the saved GIFs are kept and sent by
// the MTS Link file ids.
QHash<DocumentId, Api::FileData> GifFiles;
// The image files by their photos: added to the local stickers.
QHash<PhotoId, Api::FileData> PhotoFiles;
// The images sent by TeleLink as files: MTS Link has no "as a file" (the
// server marks any image with its size), they are shown as files here.
QSet<QString> ImageAsFileIds;
constexpr auto kMtsLinkImageFilesTag = uint64(0xBC0B'0000'0000'0001ULL);
// The animated stickers of the messages (sent by TeleLink): by documents.
QHash<DocumentId, Api::FileData> StickerFiles;

constexpr auto kTgsMime = "application/x-tgsticker";
constexpr auto kWebmStickerSuffix = ".sticker.webm";
constexpr auto kAnimatedStickerSide = 512;

// TeleLink sends the animated stickers as files: "*.tgs" (Lottie) and
// "*.sticker.webm" (video), shown as stickers by these names.
[[nodiscard]] bool isTgsStickerFile(const QString &name, const QString &mime) {
	return (mime == QLatin1String(kTgsMime))
		|| name.endsWith(u".tgs"_q, Qt::CaseInsensitive);
}

[[nodiscard]] bool isWebmStickerFile(const QString &name) {
	return name.endsWith(
		QLatin1String(kWebmStickerSuffix),
		Qt::CaseInsensitive);
}

// A static sticker sent by TeleLink: "<name>.sticker.png" (.jpg, .webp),
// shown as a sticker (other clients show the image).
[[nodiscard]] bool isImageStickerFile(const QString &name) {
	const auto lower = name.toLower();
	return lower.endsWith(u".sticker.png"_q)
		|| lower.endsWith(u".sticker.jpg"_q)
		|| lower.endsWith(u".sticker.jpeg"_q)
		|| lower.endsWith(u".sticker.webp"_q);
}

// The name of a sticker image to send: with ".sticker" before the ext.
[[nodiscard]] QString imageStickerFileName(const QString &name) {
	if (isImageStickerFile(name)) {
		return name;
	}
	const auto info = QFileInfo(name);
	const auto suffix = info.suffix().isEmpty()
		? u"png"_q
		: info.suffix();
	return info.completeBaseName() + u".sticker."_q + suffix;
}

[[nodiscard]] QVector<MTPDocumentAttribute> stickerAttributes(
		const QString &name,
		int width,
		int height) {
	return {
		MTP_documentAttributeFilename(MTP_string(name)),
		MTP_documentAttributeImageSize(
			MTP_int(width > 0 ? width : kAnimatedStickerSide),
			MTP_int(height > 0 ? height : kAnimatedStickerSide)),
		MTP_documentAttributeSticker(
			MTP_flags(0),
			MTP_string(),
			MTP_inputStickerSetEmpty(),
			MTPMaskCoords()),
	};
}

MTPMessageMedia buildPhotoMedia(
		not_null<Main::Session*> session,
		const Api::FileData &file,
		TimeId date) {
	const auto thumbUrl = privateCdnThumbBase() + file.id + u"_s.jpg"_q;
	const auto fullUrl = privateCdnThumbBase() + file.id + u".jpg"_q;

	const auto photoId = PhotoId(uuidToBareId(file.id));
	PhotoFiles.insert(photoId, file);

	const auto thumbLocation = ImageLocation(
		DownloadLocation{ PlainUrlLocation{ thumbUrl } },
		file.width, file.height);
	const auto fullLocation = ImageLocation(
		DownloadLocation{ PlainUrlLocation{ fullUrl } },
		file.width, file.height);

	const auto photo = session->data().photo(photoId);
	photo->updateImages(
		QByteArray(),
		ImageWithLocation{},
		ImageWithLocation{ .location = thumbLocation },
		ImageWithLocation{ .location = fullLocation },
		ImageWithLocation{},
		ImageWithLocation{},
		crl::time(0));


	using Flag = MTPDmessageMediaPhoto::Flag;
	return MTP_messageMediaPhoto(
		MTP_flags(Flag::f_photo),
		MTP_photo(
			MTP_flags(0),
			MTP_long(photoId),
			MTP_long(0),
			MTP_bytes(),
			MTP_int(date),
			MTP_vector<MTPPhotoSize>(1,
				MTP_photoSize(
					MTP_string("m"),
					MTP_int(file.width),
					MTP_int(file.height),
					MTP_int(0))),
			MTPVector<MTPVideoSize>(),
			MTP_int(session->mainDcId())),
		MTP_int(0),
		MTPDocument());
}


MTPMessageMedia buildFileMedia(
		not_null<Main::Session*> session,
		const Api::FileData &file,
		TimeId date) {
	if (file.url.isEmpty() && file.id.isEmpty()) {
		return MTP_messageMediaEmpty();
	}

	const auto isGif = (file.mime == u"image/gif"_q);
	if (!isGif
		&& isImageMime(file.mime)
		&& file.width > 0
		&& file.height > 0
		&& !ImageAsFileIds.contains(file.id)
		&& !isImageStickerFile(file.name)) {
		return buildPhotoMedia(session, file, date);
	}

	const auto fileUrl = fileDownloadBase() + file.id + u"/download"_q;

	const auto isTgs = isTgsStickerFile(file.name, file.mime);
	const auto isWebmSticker = !isTgs && isWebmStickerFile(file.name);
	const auto isImageSticker = !isTgs
		&& !isWebmSticker
		&& isImageStickerFile(file.name);
	if (isTgs || isWebmSticker || isImageSticker) {
		// An animated sticker: the mime type of Telegram, not of the server.
		const auto mime = isTgs
			? QString(kTgsMime)
			: isWebmSticker
			? u"video/webm"_q
			: file.mime;
		const auto attrs = stickerAttributes(
			file.name,
			isTgs ? 0 : file.width,
			isTgs ? 0 : file.height);
		const auto docId = DocumentId(uuidToBareId(file.id));
		auto kept = file;
		kept.mime = mime;
		StickerFiles.insert(docId, kept);
		const auto doc = session->data().document(
			docId,
			uint64(0),
			QByteArray(),
			date,
			attrs,
			mime,
			InlineImageLocation(),
			ImageWithLocation(),
			ImageWithLocation(),
			false,
			session->mainDcId(),
			file.size);
		doc->setContentUrl(fileUrl);
		using Flag = MTPDmessageMediaDocument::Flag;
		return MTP_messageMediaDocument(
			MTP_flags(Flag::f_document),
			MTP_document(
				MTP_flags(0),
				MTP_long(docId),
				MTP_long(0),
				MTP_bytes(),
				MTP_int(date),
				MTP_string(mime),
				MTP_long(file.size),
				MTP_vector<MTPPhotoSize>(),
				MTPVector<MTPVideoSize>(),
				MTP_int(session->mainDcId()),
				MTP_vector<MTPDocumentAttribute>(attrs)),
			MTPVector<MTPDocument>(),
			MTPPhoto(),
			MTPint(),
			MTP_int(0));
	}

	const auto isVideo = file.mime.startsWith(u"video/"_q);

	QVector<MTPDocumentAttribute> attrs;
	attrs.push_back(
		MTP_documentAttributeFilename(MTP_string(file.name)));
	// Any ogg file is shown as a voice message, with the player.
	if (file.voice || file.mime == u"audio/ogg"_q) {
		// The waveform is counted from the sound when loaded, the one of
		// MTS Link does not match the sound in the Telegram player.
		using Flag = MTPDdocumentAttributeAudio::Flag;
		attrs.push_back(MTP_documentAttributeAudio(
			MTP_flags(Flag::f_voice),
			MTP_int(file.duration),
			MTPstring(),
			MTPstring(),
			MTPbytes()));
	} else if (isVideo) {
		using Flag = MTPDdocumentAttributeVideo::Flag;
		attrs.push_back(MTP_documentAttributeVideo(
			MTP_flags(Flag::f_supports_streaming),
			MTP_double(0),
			MTP_int(file.width),
			MTP_int(file.height),
			MTPint(),
			MTPdouble(),
			MTPstring()));
	} else if (file.width > 0 && file.height > 0) {
		attrs.push_back(MTP_documentAttributeImageSize(
			MTP_int(file.width), MTP_int(file.height)));
	}
	if (isGif) {
		// An animation: the GIF player and "Save GIF".
		attrs.push_back(MTP_documentAttributeAnimated());
	}

	auto thumbnail = ImageWithLocation{};
	const auto hasVisualThumb = !file.id.isEmpty()
		&& (file.mime.startsWith(u"image/"_q)
			|| file.mime.startsWith(u"video/"_q));
	if (hasVisualThumb) {
		const auto thumbUrl = privateCdnThumbBase() + file.id + u"_s.jpg"_q;
		thumbnail = ImageWithLocation{
			.location = ImageLocation(
				DownloadLocation{ PlainUrlLocation{ thumbUrl } },
				320, 320),
		};
	}

	const auto docId = DocumentId(uuidToBareId(file.id));
	if (isGif) {
		GifFiles.insert(docId, file);
	}
	const auto doc = session->data().document(
		docId,
		uint64(0),
		QByteArray(),
		date,
		attrs,
		file.mime,
		InlineImageLocation(),
		thumbnail,
		ImageWithLocation{},
		false,
		session->mainDcId(),
		file.size);
	doc->setContentUrl(fileUrl);

	const auto mtpDoc = MTP_document(
		MTP_flags(0),
		MTP_long(docId),
		MTP_long(0),
		MTP_bytes(),
		MTP_int(date),
		MTP_string(file.mime),
		MTP_long(file.size),
		MTP_vector<MTPPhotoSize>(),
		MTPVector<MTPVideoSize>(),
		MTP_int(session->mainDcId()),
		MTP_vector<MTPDocumentAttribute>(attrs));

	using Flag = MTPDmessageMediaDocument::Flag;
	return MTP_messageMediaDocument(
		MTP_flags(Flag::f_document),
		mtpDoc,
		MTPVector<MTPDocument>(),
		MTPPhoto(),
		MTPint(),
		MTP_int(0));
}

// The userpic photo for the media viewer: a click on a userpic opens it
// in the full size ("thumb_<id>.jpg", "_xl" redirects there).
void ensureUserpicPhoto(
		not_null<PeerData*> peer,
		PhotoId photoId,
		const QString &fileId) {
	static auto created = QSet<PhotoId>();
	if (created.contains(photoId)) {
		return;
	}
	created.insert(photoId);
	const auto base = avatarCdnBase() + fileId;
	const auto image = [&](const QString &suffix, int size) {
		return ImageWithLocation{
			.location = ImageLocation(
				DownloadLocation{ PlainUrlLocation{ base + suffix } },
				size,
				size),
		};
	};
	peer->owner().photo(
		photoId,
		uint64(0),
		QByteArray(),
		base::unixtime::now(),
		peer->session().mainDcId(),
		false,
		QByteArray(),
		image(u"_s.jpg"_q, 160),
		image(u"_m.jpg"_q, 256),
		image(u".jpg"_q, 1024),
		ImageWithLocation(),
		ImageWithLocation(),
		crl::time(0));
}

// The server gives an avatarFileId to users without an avatar too, the
// file is just not on the CDN (404): such users have no userpic.
QSet<QString> MissingAvatarFiles;

void ClearUserpic(not_null<PeerData*> peer) {
	peer->setUserpic(PhotoId(), ImageLocation(), false);
	peer->session().changes().peerUpdated(
		peer,
		Data::PeerUpdate::Flag::Photo);
}
QHash<PeerId, QString> UserpicFileIds;

void applyUserpic(
		not_null<PeerData*> peer,
		const QString &fileId,
		int line);

// A thread in the chats list shows the userpic of its root message author.
void applyUserpicToThreads(
		not_null<PeerData*> user,
		const QString &fileId) {
	for (auto i = ThreadAuthorMap.cbegin(); i != ThreadAuthorMap.cend(); ++i) {
		if (i.value() != user->id) {
			continue;
		} else if (const auto thread = user->owner().peerLoaded(i.key())) {
			applyUserpic(thread, fileId, __LINE__);
		}
	}
}

void applyUserpic(
		not_null<PeerData*> peer,
		const QString &fileId,
		int line) {
	if (fileId.isEmpty()) {
		return;
	} else if (MissingAvatarFiles.contains(fileId)) {
		if (peer->userpicPhotoId() || peer->userpicPhotoUnknown()) {
			ClearUserpic(peer);
		}
		if (peer->isUser()) {
			applyUserpicToThreads(peer, fileId);
		}
		return;
	}
	if (peer->isUser()) {
		applyUserpicToThreads(peer, fileId);
	}
	UserpicFileIds.insert(peer->id, fileId);
	const auto photoId = PhotoId(uuidToBareId(fileId));
	ensureUserpicPhoto(peer, photoId, fileId);
	if (peer->userpicPhotoId() == photoId) {
		return;
	}
	const auto url = avatarCdnBase() + fileId + u"_s.jpg"_q;
	const auto location = ImageLocation(
		DownloadLocation{ PlainUrlLocation{ url } }, 160, 160);
	peer->setUserpic(photoId, location, false);
	peer->session().changes().peerUpdated(
		peer,
		Data::PeerUpdate::Flag::Photo);
}

[[nodiscard]] bool HasKnownUserpic(not_null<PeerData*> peer) {
	return !peer->userpicPhotoUnknown() && peer->userpicPhotoId();
}

} // namespace

// A userpic restored from the local storage (the self user) or set before:
// the full size photo for the media viewer is created for it as well.
void ensureUserpicFor(not_null<PeerData*> peer, const QString &fileId) {
	if (fileId.isEmpty()) {
		return;
	}
	const auto photoId = PhotoId(uuidToBareId(fileId));
	if (peer->userpicPhotoId() == photoId) {
		UserpicFileIds.insert(peer->id, fileId);
		ensureUserpicPhoto(peer, photoId, fileId);
	} else if (!HasKnownUserpic(peer)) {
		applyUserpic(peer, fileId, __LINE__);
	}
}

namespace {

ChatId extractChatIdFromDst(const QString &dst) {
	const auto chatPrefix = QStringLiteral("chat-");
	if (!dst.startsWith(chatPrefix)) {
		return {};
	}
	const auto orgPos = dst.indexOf("-org-");
	if (orgPos < 0) {
		return dst.mid(chatPrefix.size());
	}
	return dst.mid(chatPrefix.size(), orgPos - chatPrefix.size());
}

qint64 parseTimestamp(const QJsonObject &obj, const char *msKey, const char *key) {
	const auto ms = obj.value(QLatin1String(msKey));
	if (!ms.isUndefined()) {
		return qint64(ms.toDouble());
	}
	const auto v = qint64(obj.value(QLatin1String(key)).toDouble());
	return (v > 0 && v < 10000000000LL) ? v * 1000 : v;
}

} // namespace

TextWithEntities parseMentionedText(
		const QString &text,
		const QString &markdown,
		const QList<Api::MentionInfo> &mentions,
		not_null<Main::Session*> session) {
	const auto &source = markdown.isEmpty() ? text : markdown;
	if (source.isEmpty()) {
		return TextWithEntities{};
	}

	QHash<QString, QString> nameMap;
	for (const auto &m : mentions) {
		if (!m.userId.isEmpty() && !m.name.isEmpty()) {
			nameMap[m.userId] = m.name;
		}
	}
	if (source.contains(u"<@u:"_q)) {
		auto names = QStringList();
		for (const auto &m : mentions) {
			names.push_back(m.userId.left(8) + '=' + m.name);
		}
		LOG(("MtsLink Mention: parse '%1' mentions=[%2]"
			).arg(source.left(120)
			).arg(names.join(u", "_q)));
	}

	const bool hasMentions = source.contains(u"<@u:"_q);
	const bool hasMarkdownChars = source.contains('*')
		|| source.contains('~')
		|| source.contains('`')
		|| source.contains('_')
		|| source.contains('|')
		|| source.contains('[')
		|| source.startsWith('>')
		|| source.contains(u"\n>"_q);
	const bool needsMarkdownParse = !markdown.isEmpty()
		|| hasMarkdownChars;

	if (!needsMarkdownParse && !hasMentions) {
		auto result = TextWithEntities{ source };
		TextUtilities::ParseEntities(result, TextParseLinks);
		return result;
	}

	QString result;
	EntitiesInText entities;

	if (!needsMarkdownParse) {
		static const auto re = QRegularExpression(
			QStringLiteral("<@u:([0-9a-f\\-]{36})>"));
		int pos = 0;
		auto it = re.globalMatch(source);
		while (it.hasNext()) {
			const auto match = it.next();
			result += source.mid(pos, match.capturedStart() - pos);
			const auto userId = match.captured(1);
			auto displayName = nameMap.value(userId);
			if (displayName.isEmpty()) {
				const auto bareId = uuidToBareId(userId);
				if (const auto user = session->data().userLoaded(
						::UserId(bareId))) {
					displayName = user->name();
				}
			}
			if (!displayName.isEmpty()) {
				const auto mention = u"@"_q + displayName;
				const auto bareId = uuidToBareId(userId);
				const auto selfBareId = session->userId().bare;
				const auto data = TextUtilities::MentionNameDataFromFields({
					.selfId = selfBareId,
					.userId = bareId,
					.accessHash = 0,
				});
				entities.push_back(EntityInText(
					EntityType::MentionName,
					result.size(),
					mention.size(),
					data));
				result += mention;
			} else {
				result += match.captured(0);
			}
			pos = match.capturedEnd();
		}
		result += source.mid(pos);
		auto parsed = TextWithEntities{ result, entities };
		TextUtilities::ParseEntities(parsed, TextParseLinks);
		return parsed;
	}

	int boldStart = -1;
	int italicStart = -1;
	int strikeStart = -1;
	int underlineStart = -1;
	int spoilerStart = -1;
	int codeStart = -1;
	bool inCode = false;
	int preStart = -1;
	QString preLang;
	bool inPre = false;
	int quoteStart = -1;
	bool atLineStart = true;

	QString unescaped;
	unescaped.reserve(source.size());
	for (int i = 0, sz = source.size(); i < sz; ++i) {
		if (source[i] == '\\' && i + 1 < sz) {
			const auto next = source[i + 1];
			if (next == '*'
				|| next == '~'
				|| next == '`'
				|| next == '\\'
				|| next == '_'
				|| next == '|') {
				unescaped += next;
				++i;
				continue;
			}
		}
		unescaped += source[i];
	}

	int pos = 0;
	const int len = unescaped.size();

	const auto isTripleBacktick = [&](int p) {
		return p + 2 < len
			&& unescaped[p] == '`'
			&& unescaped[p + 1] == '`'
			&& unescaped[p + 2] == '`';
	};
	while (pos < len) {
		if (isTripleBacktick(pos)) {
			if (inPre) {
				// The newline before the closing ``` is not in the block
				// (as in Telegram): a block at the very end of the message
				// lost its entity with the trimmed last newline.
				auto preLength = result.size() - preStart;
				if (preLength > 0 && result.endsWith('\n')) {
					--preLength;
				}
				entities.push_back(EntityInText(
					EntityType::Pre,
					preStart,
					preLength,
					preLang));
				preStart = -1;
				preLang.clear();
				inPre = false;
				pos += 3;
				if (pos < len && unescaped[pos] == '\n') {
					++pos;
					atLineStart = true;
				}
				continue;
			} else {
				pos += 3;
				const auto nlPos = unescaped.indexOf('\n', pos);
				const auto searchEnd = (nlPos != -1) ? nlPos : len;
				int closeTriple = -1;
				for (int i = pos; i + 2 < searchEnd; ++i) {
					if (unescaped[i] == '`'
						&& unescaped[i + 1] == '`'
						&& unescaped[i + 2] == '`') {
						closeTriple = i;
						break;
					}
				}
				if (closeTriple >= 0) {
					const auto content = unescaped.mid(
						pos, closeTriple - pos);
					const auto entityStart = result.size();
					result += content;
					if (!content.isEmpty()) {
						entities.push_back(EntityInText(
							EntityType::Pre,
							entityStart,
							content.size()));
					}
					pos = closeTriple + 3;
				} else {
					if (nlPos != -1) {
						preLang = unescaped.mid(
							pos, nlPos - pos).trimmed();
						pos = nlPos + 1;
					}
					preStart = result.size();
					inPre = true;
				}
				continue;
			}
		}

		if (inPre) {
			result += unescaped[pos];
			++pos;
			continue;
		}

		if (atLineStart) {
			if (quoteStart >= 0 && unescaped[pos] != '>') {
				auto quoteLen = result.size() - quoteStart;
				if (quoteLen > 0
					&& result[quoteStart + quoteLen - 1] == '\n') {
					--quoteLen;
				}
				if (quoteLen > 0) {
					entities.push_back(EntityInText(
						EntityType::Blockquote,
						quoteStart,
						quoteLen));
				}
				quoteStart = -1;
			}
			if (unescaped[pos] == '>') {
				if (quoteStart < 0) {
					quoteStart = result.size();
				}
				++pos;
				if (pos < len && unescaped[pos] == ' ') {
					++pos;
				}
				atLineStart = false;
				continue;
			}
			atLineStart = false;
		}

		if (pos + 3 < len
			&& unescaped[pos] == '<'
			&& unescaped[pos + 1] == '@'
			&& unescaped[pos + 2] == 'u'
			&& unescaped[pos + 3] == ':') {
			const auto end = unescaped.indexOf('>', pos + 4);
			if (end != -1) {
				const auto userId = unescaped.mid(pos + 4, end - pos - 4);
				auto displayName = nameMap.value(userId);
				if (displayName.isEmpty()) {
					const auto bareId = uuidToBareId(userId);
					if (const auto user = session->data().userLoaded(
							::UserId(bareId))) {
						displayName = user->name();
					}
				}
				if (!displayName.isEmpty()) {
					const auto mention = u"@"_q + displayName;
					const auto bareId = uuidToBareId(userId);
					const auto selfBareId = session->userId().bare;
					const auto data = TextUtilities::MentionNameDataFromFields({
						.selfId = selfBareId,
						.userId = bareId,
						.accessHash = 0,
					});
					entities.push_back(EntityInText(
						EntityType::MentionName,
						result.size(),
						mention.size(),
						data));
					result += mention;
				} else {
					result += unescaped.mid(pos, end - pos + 1);
				}
				pos = end + 1;
				continue;
			}
		}

		if (unescaped[pos] == '[') {
			const auto closeB = unescaped.indexOf(']', pos + 1);
			if (closeB != -1
				&& closeB + 1 < len
				&& unescaped[closeB + 1] == '(') {
				const auto closeP = unescaped.indexOf(')', closeB + 2);
				if (closeP != -1) {
					const auto linkText = unescaped.mid(pos + 1, closeB - pos - 1);
					const auto linkUrl = unescaped.mid(closeB + 2, closeP - closeB - 2);
					if (!linkText.isEmpty() && !linkUrl.isEmpty()) {
						entities.push_back(EntityInText(
							EntityType::CustomUrl,
							result.size(),
							linkText.size(),
							linkUrl));
						result += linkText;
						pos = closeP + 1;
						continue;
					}
				}
			}
		}

		if (unescaped[pos] == '`') {
			if (codeStart >= 0) {
				entities.push_back(EntityInText(
					EntityType::Code,
					codeStart,
					result.size() - codeStart));
				codeStart = -1;
				inCode = false;
			} else {
				codeStart = result.size();
				inCode = true;
			}
			++pos;
			continue;
		}

		if (inCode) {
			result += unescaped[pos];
			++pos;
			continue;
		}

		if (pos + 1 < len
			&& unescaped[pos] == '~'
			&& unescaped[pos + 1] == '~') {
			if (strikeStart >= 0) {
				entities.push_back(EntityInText(
					EntityType::StrikeOut,
					strikeStart,
					result.size() - strikeStart));
				strikeStart = -1;
			} else {
				strikeStart = result.size();
			}
			pos += 2;
			continue;
		}

		if (pos + 1 < len
			&& unescaped[pos] == '|'
			&& unescaped[pos + 1] == '|') {
			if (spoilerStart >= 0) {
				entities.push_back(EntityInText(
					EntityType::Spoiler,
					spoilerStart,
					result.size() - spoilerStart));
				spoilerStart = -1;
			} else {
				spoilerStart = result.size();
			}
			pos += 2;
			continue;
		}

		if (pos + 1 < len
			&& unescaped[pos] == '*'
			&& unescaped[pos + 1] == '*') {
			if (boldStart >= 0) {
				entities.push_back(EntityInText(
					EntityType::Bold,
					boldStart,
					result.size() - boldStart));
				boldStart = -1;
			} else {
				boldStart = result.size();
			}
			pos += 2;
			continue;
		}

		if (pos + 1 < len
			&& unescaped[pos] == '_'
			&& unescaped[pos + 1] == '_') {
			if (underlineStart >= 0) {
				entities.push_back(EntityInText(
					EntityType::Underline,
					underlineStart,
					result.size() - underlineStart));
				underlineStart = -1;
			} else {
				underlineStart = result.size();
			}
			pos += 2;
			continue;
		}

		if (unescaped[pos] == '*') {
			if (italicStart >= 0) {
				entities.push_back(EntityInText(
					EntityType::Italic,
					italicStart,
					result.size() - italicStart));
				italicStart = -1;
			} else {
				italicStart = result.size();
			}
			++pos;
			continue;
		}

		if (unescaped[pos] == '\n') {
			atLineStart = true;
		}
		result += unescaped[pos];
		++pos;
	}

	if (quoteStart >= 0) {
		auto quoteLen = result.size() - quoteStart;
		if (quoteLen > 0
			&& result[quoteStart + quoteLen - 1] == '\n') {
			--quoteLen;
		}
		if (quoteLen > 0) {
			entities.push_back(EntityInText(
				EntityType::Blockquote,
				quoteStart,
				quoteLen));
		}
	}

	struct UnmatchedMarker {
		int *start;
		const char *marker;
	};
	for (const auto &u : {
		UnmatchedMarker{ &boldStart, "**" },
		UnmatchedMarker{ &italicStart, "*" },
		UnmatchedMarker{ &strikeStart, "~~" },
		UnmatchedMarker{ &underlineStart, "__" },
		UnmatchedMarker{ &spoilerStart, "||" },
		UnmatchedMarker{ &codeStart, "`" },
	}) {
		if (*u.start >= 0) {
			const auto marker = QString::fromLatin1(u.marker);
			result.insert(*u.start, marker);
			const auto shift = marker.size();
			for (auto &e : entities) {
				if (e.offset() >= *u.start) {
					e = EntityInText(
						e.type(),
						e.offset() + shift,
						e.length(),
						e.data());
				}
			}
		}
	}
	if (inPre) {
		const auto marker = u"```"_q
			+ (preLang.isEmpty() ? QString() : preLang)
			+ u"\n"_q;
		result.insert(preStart, marker);
		const auto shift = marker.size();
		for (auto &e : entities) {
			if (e.offset() >= preStart) {
				e = EntityInText(
					e.type(),
					e.offset() + shift,
					e.length(),
					e.data());
			}
		}
	}

	auto parsed = TextWithEntities{ result, entities };
	TextUtilities::ParseEntities(parsed, TextParseLinks);
	return parsed;
}

namespace {

QString markdownFromBlocks(const QJsonArray &blocks) {
	QString md;
	for (const auto &b : blocks) {
		const auto obj = b.toObject();
		const auto type = obj.value(u"type"_q).toString();
		const auto value = obj.value(u"value"_q).toObject();

		if (type == u"LineBreak"_q) {
			md += '\n';
		} else if (type == u"TextElement"_q) {
			const auto text = value.value(u"text"_q).toString();
			const auto style = value.value(u"style"_q).toObject();
			const auto bold = style.value(u"bold"_q).toBool();
			const auto italic = style.value(u"italic"_q).toBool();
			const auto strike = style.value(u"strike"_q).toBool();
			QString wrapped = text;
			if (bold) wrapped = u"**"_q + wrapped + u"**"_q;
			if (italic) wrapped = u"*"_q + wrapped + u"*"_q;
			if (strike) wrapped = u"~~"_q + wrapped + u"~~"_q;
			md += wrapped;
		} else if (type == u"MentionElement"_q) {
			const auto id = value.value(u"id"_q).toString();
			md += u"<@u:"_q + id + u">"_q;
		} else if (type == u"LinkElement"_q) {
			const auto url = value.value(u"url"_q).toString();
			const auto elements = value.value(u"elements"_q).toArray();
			QString linkText;
			for (const auto &el : elements) {
				const auto eo = el.toObject();
				if (eo.value(u"type"_q).toString() == u"TextElement"_q) {
					linkText += eo.value(u"value"_q).toObject()
						.value(u"text"_q).toString();
				}
			}
			if (linkText == url || linkText.isEmpty()) {
				md += url;
			} else {
				md += u"["_q + linkText + u"]("_q + url + u")"_q;
			}
		}
	}
	return md;
}

// Mirrors MTS Link: "Outgoing / Incoming / Missed call" in personal chats,
// "Group call" elsewhere, with the duration once the call has ended.
PreparedServiceText buildCallServiceText(
		const Api::CallMetadata &meta,
		bool outgoing,
		bool personal) {
	const auto ended = (meta.status == u"Ended"_q);
	const auto reason = meta.statusReason;
	const auto withDuration = [&](const QString &type) {
		return (ended && meta.duration > 0)
			? tr::lng_call_type_and_duration(
				tr::now,
				lt_type,
				type,
				lt_duration,
				Ui::FormatDurationWords(meta.duration))
			: type;
	};
	auto text = QString();
	if (!personal) {
		text = withDuration(tr::lng_mtslink_group_call(tr::now));
	} else if (ended && reason == u"Declined"_q) {
		text = tr::lng_call_declined(tr::now);
	} else {
		if (ended && !reason.isEmpty() && reason != u"None"_q) {
			LOG(("MtsLink Call: unknown statusReason '%1'").arg(reason));
		}
		text = withDuration(outgoing
			? tr::lng_call_outgoing(tr::now)
			: tr::lng_call_incoming(tr::now));
	}
	auto result = PreparedServiceText();
	result.text = TextWithEntities{ text };
	return result;
}

[[nodiscard]] bool isPersonalChat(PeerId peerId) {
	const auto type = chatTypeForPeer(peerId);
	return (type == ChatType::Dialog) || (type == ChatType::Favorites);
}

// The call message is a regular message (its thread is the chat inside the
// call): the call text and a "Join" button while the call is going on.
QHash<FullMsgId, Api::CallMetadata> CallMetaByItem;

// The materials of a finished call (WebinarApp.GetMaterials): the summary
// and the record are added to the call thread as local messages. They are
// kept in the cache of the messages and are not requested again.
struct CallMaterialsState {
	FullMsgId callItem;
	QString callUuid;
	TimeId date = 0;
	int attempt = 0;
	bool summary = false;
	bool record = false;
	bool done = false; // Nothing more to request.
	bool cacheLoading = false;
	bool cacheLoaded = false;
	bool requested = false;
	bool polling = false; // Finished in this launch.
	bool pollWhenLoaded = false;
	QList<Api::MessageData> messages; // As kept in the cache.
};
QHash<QString, CallMaterialsState> CallMaterials; // By webinarEventId.
QHash<FullMsgId, std::vector<MsgId>> CallMaterialItems;
QHash<FullMsgId, int> CallServerChildren;

// The record message: a card opening the record, not a text.
const auto kCallRecordStatus = u"Record"_q;

void updateCallReplies(not_null<HistoryItem*> item) {
	const auto id = item->fullId();
	const auto server = CallServerChildren.value(id);
	const auto local = int(CallMaterialItems.value(id).size());
	const auto total = server + local;
	if (total <= 0) {
		return;
	}
	const auto views = item->Get<HistoryMessageViews>();
	const auto unread = views
		? std::max(int(views->commentsMaxId.bare
			- views->commentsInboxReadTillId.bare), 0)
		: 0;
	auto repliesData = HistoryMessageRepliesData();
	repliesData.isNull = false;
	repliesData.repliesCount = total;
	repliesData.maxId = MsgId(total);
	repliesData.readMaxId = MsgId(std::max(total - unread, 1));
	item->setReplies(std::move(repliesData));
	item->history()->owner().requestItemViewRefresh(item);
}

// Adds a material message (a new or a cached one) to the call thread.
void showCallMaterial(
		not_null<Main::Session*> session,
		const QString &eventId,
		Api::MessageData msg) {
	const auto &state = CallMaterials[eventId];
	const auto history = session->data().historyLoaded(state.callItem.peer);
	const auto callItem = history
		? session->data().message(state.callItem)
		: nullptr;
	if (!callItem || state.callUuid.isEmpty()) {
		return;
	}
	// The call message may be reloaded with another local id.
	msg.parentId = state.callUuid;
	msg.chatId = peerIdToChatId(state.callItem.peer);
	const auto item = addMessage(session, msg, true);
	if (!item) {
		return;
	}
	if (msg.callMeta && msg.callMeta->status == kCallRecordStatus) {
		// A card as of a file: opens the record by a click.
		item->setMtsLinkMedia(std::make_unique<Data::MediaMtsLinkCall>(
			item,
			Data::MtsLinkCall{
				.title = tr::lng_mtslink_materials_record_text(tr::now),
				.duration = msg.callMeta->duration,
				.recordLink = msg.callMeta->joinLink,
			}));
		if (!item->originalText().empty()) {
			item->setText(TextWithEntities());
		}
	}
	auto &list = CallMaterialItems[state.callItem];
	if (!ranges::contains(list, item->id)) {
		list.push_back(item->id);
	}
	updateCallReplies(callItem);
}

void saveCallMaterials(
		not_null<Main::Session*> session,
		const QString &eventId) {
	const auto &state = CallMaterials[eventId];
	saveCallMaterialsToCache(session, eventId, state.done, state.messages);
}

// A new material: shown and kept in the cache.
void addCallMaterial(
		not_null<Main::Session*> session,
		const QString &eventId,
		const QString &suffix,
		const QString &text,
		std::optional<Api::FileData> file,
		std::optional<Api::CallMetadata> meta) {
	auto &state = CallMaterials[eventId];
	const auto history = session->data().historyLoaded(state.callItem.peer);
	const auto callItem = history
		? session->data().message(state.callItem)
		: nullptr;
	if (!callItem) {
		return;
	}
	auto msg = Api::MessageData();
	msg.id = eventId + u"-"_q + suffix;
	// From the initiator of the call, or from the user if there is none.
	if (const auto user = callItem->from()->asUser()) {
		msg.authorId = userBareIdToUuid(peerToUser(user->id).bare);
	}
	if (msg.authorId.isEmpty()) {
		if (const auto mts = session->account().mtsLinkSession()) {
			msg.authorId = mts->userId();
		}
	}
	msg.text = text;
	msg.createdAt = qint64(base::unixtime::now()) * 1000;
	msg.type = MessageType::Text;
	if (file) {
		msg.files.push_back(*file);
	}
	msg.callMeta = meta;
	state.messages.push_back(msg);
	saveCallMaterials(session, eventId);
	LOG(("MtsLink Call: material '%1' added to the thread of %2"
		).arg(suffix, state.callUuid));
	showCallMaterial(session, eventId, std::move(msg));
}

// The summary file name, size and type: a HEAD request with the session.
void addCallSummary(
		not_null<Main::Session*> session,
		const QString &eventId,
		const QString &text,
		const QString &fileId) {
	const auto weak = base::make_weak(session);
	if (fileId.isEmpty()) {
		addCallMaterial(session, eventId, u"summary"_q, text, {}, {});
		return;
	}
	static const auto manager = new QNetworkAccessManager();
	auto request = QNetworkRequest(
		QUrl(fileDownloadBase() + fileId + u"/download"_q));
	auto cookies = QStringList();
	for (const auto &cookie : fileAuthCookies()) {
		cookies.push_back(QString::fromLatin1(cookie.name())
			+ '='
			+ QString::fromLatin1(cookie.value()));
	}
	request.setRawHeader("Cookie", cookies.join(u"; "_q).toLatin1());
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	const auto reply = manager->head(request);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		auto name = QString();
		const auto disposition = QString::fromUtf8(
			reply->rawHeader("Content-Disposition"));
		static const auto utf8 = QRegularExpression(
			u"filename\\*=UTF-8''([^;]+)"_q,
			QRegularExpression::CaseInsensitiveOption);
		static const auto plain = QRegularExpression(
			u"filename=\"?([^\";]+)\"?"_q,
			QRegularExpression::CaseInsensitiveOption);
		if (const auto m = utf8.match(disposition); m.hasMatch()) {
			name = QUrl::fromPercentEncoding(m.captured(1).toUtf8());
		} else if (const auto m = plain.match(disposition); m.hasMatch()) {
			name = m.captured(1);
		}
		const auto mime = QString::fromLatin1(
			reply->rawHeader("Content-Type")).section(';', 0, 0).trimmed();
		const auto size = reply->header(
			QNetworkRequest::ContentLengthHeader).toLongLong();
		LOG(("MtsLink Call: summary file status=%1 name='%2' mime=%3 size=%4"
			).arg(reply->attribute(
				QNetworkRequest::HttpStatusCodeAttribute).toInt()
			).arg(name, mime
			).arg(size));
		if (name.isEmpty()) {
			name = tr::lng_mtslink_materials_summary(tr::now);
		}
		addCallMaterial(strong, eventId, u"summary"_q, text, Api::FileData{
			.id = fileId,
			.name = name,
			.size = size,
			.mime = mime.isEmpty() ? u"application/octet-stream"_q : mime,
		}, {});
	});
}

void requestCallMaterials(
		not_null<Main::Session*> session,
		const QString &eventId);

void scheduleCallMaterials(
		not_null<Main::Session*> session,
		const QString &eventId) {
	// The summary and the record are ready some minutes after the call.
	constexpr auto kDelays = std::array<crl::time, 6>{
		30 * 1000,
		2 * 60 * 1000,
		5 * 60 * 1000,
		10 * 60 * 1000,
		20 * 60 * 1000,
		40 * 60 * 1000,
	};
	auto &state = CallMaterials[eventId];
	if (state.done || state.attempt >= int(kDelays.size())) {
		return;
	}
	const auto weak = base::make_weak(session);
	base::call_delayed(kDelays[state.attempt++], [=] {
		if (const auto strong = weak.get()) {
			requestCallMaterials(strong, eventId);
		}
	});
}

void requestCallMaterials(
		not_null<Main::Session*> session,
		const QString &eventId) {
	const auto mts = session->account().mtsLinkSession();
	if (!mts || !mts->rpc()) {
		return;
	}
	const auto weak = base::make_weak(session);
	mts->rpc()->call(
		u"WebinarApp.GetMaterials"_q,
		QJsonObject{ { u"callId"_q, eventId } },
		[=](const QJsonObject &result) {
			const auto strong = weak.get();
			if (!strong
				|| result.value(u"type"_q).toString() != u"Materials"_q) {
				return;
			}
			const auto value = result.value(u"value"_q).toObject();
			auto &state = CallMaterials[eventId];
			const auto summary = value.value(u"summary"_q).toObject();
			if (!state.summary
				&& summary.value(u"type"_q).toString() == u"Material"_q) {
				state.summary = true;
				const auto data = summary.value(u"value"_q).toObject();
				addCallSummary(
					strong,
					eventId,
					data.value(u"text"_q).toString(),
					data.value(u"fileId"_q).toString());
			}
			const auto record = value.value(u"record"_q).toObject();
			if (!state.record
				&& record.value(u"type"_q).toString() == u"Material"_q) {
				const auto url = record.value(u"value"_q).toObject().value(
					u"fileUrl"_q).toString();
				if (!url.isEmpty()) {
					state.record = true;
					addCallMaterial(
						strong,
						eventId,
						u"record"_q,
						tr::lng_mtslink_materials_record_text(tr::now)
							+ '\n'
							+ url,
						{},
						Api::CallMetadata{
							.status = kCallRecordStatus,
							.joinLink = url,
							.duration = int(value.value(
								u"callDuration"_q).toDouble() / 1000),
						});
				}
			}
			const auto old = state.date
				&& (base::unixtime::now() - state.date > 3600);
			if ((state.summary && state.record) || (old && !state.polling)) {
				// An old call: what is not ready now will not be.
				if (!state.done) {
					state.done = true;
					saveCallMaterials(strong, eventId);
				}
			} else if (state.polling) {
				scheduleCallMaterials(strong, eventId);
			}
		});
}

void requestCallMaterialsIfNeeded(
		not_null<Main::Session*> session,
		const QString &eventId) {
	auto &state = CallMaterials[eventId];
	if (state.done || !state.cacheLoaded) {
		return;
	}
	if (state.pollWhenLoaded) {
		state.requested = true;
		if (!state.polling) {
			state.polling = true;
			scheduleCallMaterials(session, eventId);
		}
	} else if (!state.requested) {
		state.requested = true;
		// Spread the requests of the calls in the loaded history.
		static auto queued = 0;
		const auto weak = base::make_weak(session);
		base::call_delayed(crl::time(500) * (++queued), [=] {
			--queued;
			if (const auto strong = weak.get()) {
				requestCallMaterials(strong, eventId);
			}
		});
	}
}

// A finished call: the materials from the cache, then requested if they
// are not all there. A call finished just now is polled until ready.
void ensureCallMaterials(
		not_null<Main::Session*> session,
		not_null<HistoryItem*> item,
		const Api::CallMetadata &meta,
		bool justEnded) {
	if (meta.status != u"Ended"_q || meta.webinarEventId.isEmpty()) {
		return;
	}
	const auto eventId = meta.webinarEventId;
	auto &state = CallMaterials[eventId];
	const auto reloaded = (state.callItem != item->fullId());
	state.callItem = item->fullId();
	state.callUuid = msgIdToMtsLinkId(item->history()->peer->id, item->id);
	state.date = item->date();
	if (justEnded) {
		state.pollWhenLoaded = true;
	}
	if (state.cacheLoaded) {
		if (reloaded && !state.messages.isEmpty()) {
			const auto weak = base::make_weak(session);
			crl::on_main(session, [=] {
				if (const auto strong = weak.get()) {
					for (const auto &msg : CallMaterials[eventId].messages) {
						showCallMaterial(strong, eventId, msg);
					}
				}
			});
		}
		requestCallMaterialsIfNeeded(session, eventId);
		return;
	} else if (state.cacheLoading) {
		return;
	}
	state.cacheLoading = true;
	const auto weak = base::make_weak(session);
	loadCallMaterialsFromCache(session, eventId, [=](
			bool done,
			QList<Api::MessageData> messages) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		auto &state = CallMaterials[eventId];
		state.cacheLoading = false;
		state.cacheLoaded = true;
		state.done = state.done || done;
		for (auto &msg : messages) {
			const auto record = msg.callMeta
				&& (msg.callMeta->status == kCallRecordStatus);
			const auto exists = ranges::any_of(state.messages, [&](
					const Api::MessageData &m) {
				return m.id == msg.id;
			});
			if (exists) {
				continue;
			}
			(record ? state.record : state.summary) = true;
			state.messages.push_back(msg);
			showCallMaterial(strong, eventId, std::move(msg));
		}
		requestCallMaterialsIfNeeded(strong, eventId);
	});
}

void applyCallItem(
		not_null<Main::Session*> session,
		not_null<HistoryItem*> item,
		const Api::CallMetadata &meta,
		bool justEnded = false) {
	const auto id = item->fullId();
	CallMetaByItem.insert(id, meta);
	// The title without the duration, the media shows it separately.
	auto titleMeta = meta;
	titleMeta.duration = 0;
	const auto ongoing = (meta.status == u"Started"_q)
		&& !meta.joinLink.isEmpty();
	item->setMtsLinkMedia(std::make_unique<Data::MediaMtsLinkCall>(
		item,
		Data::MtsLinkCall{
			.title = buildCallServiceText(
				titleMeta,
				item->out(),
				isPersonalChat(id.peer)).text.text,
			.joinLink = meta.joinLink,
			.duration = (meta.status == u"Ended"_q) ? meta.duration : 0,
			.ongoing = ongoing,
		}));
	if (!item->originalText().empty()) {
		item->setText(TextWithEntities());
	}
	item->updateReplyMarkup(HistoryMessageMarkupData());
	if (ongoing) {
		setActiveCall(session, id.peer, meta);
	}
	ensureCallMaterials(session, item, meta, justEnded);
}

} // namespace

void handleNotificationEvent(
	not_null<Main::Session*> session,
	const QJsonObject &param);

// MTS Link ids are UUIDv6: the leading 60 bits are a Gregorian timestamp
// in 100ns units, "1f1becd2-d550-652e-..." -> 1f1becd2 d550 52e.
[[nodiscard]] TimeId uuidV6Time(const QString &uuid) {
	const auto parts = uuid.split('-');
	if (parts.size() != 5
		|| parts[0].size() != 8
		|| parts[1].size() != 4
		|| parts[2].size() != 4
		|| parts[2][0] != QChar('6')) {
		return 0;
	}
	auto ok1 = false, ok2 = false, ok3 = false;
	const auto high = parts[0].toULongLong(&ok1, 16);
	const auto mid = parts[1].toULongLong(&ok2, 16);
	const auto low = parts[2].mid(1).toULongLong(&ok3, 16);
	if (!ok1 || !ok2 || !ok3) {
		return 0;
	}
	const auto ticks = (high << 28) | (mid << 12) | low;
	constexpr auto kGregorianToUnix = 0x01B21DD213814000ULL;
	return (ticks > kGregorianToUnix)
		? TimeId((ticks - kGregorianToUnix) / 10000000ULL)
		: 0;
}

MTPPeerNotifySettings makeMuteSettings(bool muted);
void applyMyChannelRole(not_null<ChannelData*> channel, const QString &role);
void removeStaleChannels(
	not_null<Main::Session*> session,
	const QList<Api::ChannelData> &channels,
	ChatType type = ChatType::Channel);
void attachChatCover(
	not_null<Main::Session*> session,
	const QString &chatId,
	bool group,
	const QString &fileId,
	int attempt = 0);
void applyThreadNotifiable(
	not_null<Main::Session*> session,
	const QString &chatId,
	const QString &threadId,
	bool notifiable);

void applyThreadsList(
		not_null<Main::Session*> session,
		const QList<Api::ThreadData> &threads) {
	auto previousActivity = TimeId(0);
	for (const auto &thread : threads) {
		const auto peerId = chatIdToPeerId(
			thread.id, ChatType::Thread);
		const auto channelId = peerToChannel(peerId);
		const auto channel = session->data().channel(channelId);

		using Flag = ChannelDataFlag;
		auto flags = channel->flags();
		flags |= Flag::Megagroup;
		flags &= ~Flag::Broadcast;
		flags &= ~Flag::Left;
		flags &= ~Flag::Forbidden;
		flags &= ~Flag::Forum;
		channel->setFlags(flags);

		channel->setName(thread.chatName, {});
		channel->setLoadedStatus(PeerData::LoadedStatus::Normal);

		const auto history = session->data().history(channel->id);
		if (!history->folderKnown()) {
			history->clearFolder();
		}
		ThreadNotifiable.insert(peerId, thread.isNotifiable);
		session->data().notifySettings().apply(
			channel,
			makeMuteSettings(!thread.isNotifiable));

		const auto createdAt = thread.message.value("createdAt");
		const auto dateMs = createdAt.isDouble()
			? qint64(createdAt.toDouble())
			: createdAt.toString().toLongLong();
		const auto date = dateMs > 0
			? TimeId(dateMs / 1000)
			: base::unixtime::now();
		history->setChatListTimeId(date);

		if (thread.unreadChildrenCount > 0) {
			history->setUnreadCount(thread.unreadChildrenCount);
		}

		const auto rootId = localMsgId(thread.id);

		const auto msgText = thread.message.value("text").toString();
		// A sticker, a GIF or a file without a text: the media in the
		// preview (it was empty).
		const auto rootFiles = thread.message.value("files").toArray();
		const auto rootMedia = rootFiles.isEmpty()
			? MTP_messageMediaEmpty()
			: buildFileMedia(
				session,
				Api::ParseFileData(rootFiles.first().toObject()),
				date);
		// No "type" in the thread message, a call has the call metadata.
		if (thread.message.value("metadata").toObject().value(
				"type").toString() == u"CallMetadata"_q) {
			// The thread of a call: the call media in the preview.
			const auto v = thread.message.value("metadata").toObject().value(
				"value").toObject();
			const auto meta = Api::CallMetadata{
				.status = v.value("status").toString(),
				.joinLink = v.value("joinLink").toString(),
				.webinarEventId = v.value("webinarEventId").toString(),
				.duration = int(v.value("duration").toDouble() / 1000),
				.statusReason = v.value("statusReasonV2").toString(
					v.value("statusReason").toString()),
			};
			const auto authorUuid
				= thread.message.value("authorId").toString();
			const auto mts = session->account().mtsLinkSession();
			const auto out = mts
				&& !authorUuid.isEmpty()
				&& (authorUuid == mts->userId());
			auto titleMeta = meta;
			titleMeta.duration = 0;
			const auto call = Data::MtsLinkCall{
				.title = buildCallServiceText(
					titleMeta,
					out,
					isPersonalChat(chatIdToPeerId(thread.chatId))).text.text,
				.duration = (meta.status == u"Ended"_q) ? meta.duration : 0,
			};
			auto item = session->data().message(channel->id, rootId);
			if (!item) {
				auto flags = MessageFlags();
				auto fromId = PeerId();
				if (!authorUuid.isEmpty()) {
					fromId = PeerId(::UserId(uuidToBareId(authorUuid)));
					flags |= MessageFlag::HasFromId;
				}
				if (out) {
					flags |= MessageFlag::Outgoing;
				}
				item = history->addNewLocalMessage(
					HistoryItemCommonFields{
						.id = rootId,
						.flags = flags,
						.from = fromId,
						.date = date,
					},
					TextWithEntities(),
					MTP_messageMediaEmpty());
			}
			if (item) {
				item->setMtsLinkMedia(std::make_unique<Data::MediaMtsLinkCall>(
					item,
					call));
				item->invalidateChatListEntry();
			}
		} else if (!msgText.isEmpty() || !rootFiles.isEmpty()) {
			const auto authorUuid =
				thread.message.value("authorId").toString();
			PeerId fromId;
			MessageFlags msgFlags;
			if (!authorUuid.isEmpty()) {
				const auto authorBareId = uuidToBareId(authorUuid);
				fromId = PeerId(::UserId(authorBareId));
				msgFlags |= MessageFlag::HasFromId;
			}
			const auto markdown =
				thread.message.value("markdown").toString();
			QList<Api::MentionInfo> mentions;
			auto mentionsArray = thread.message.value("metadata")
				.toObject().value("value").toObject()
				.value("mentions").toArray();
			if (mentionsArray.isEmpty()) {
				mentionsArray = thread.message.value("mentions")
					.toArray();
			}
			for (const auto &m : mentionsArray) {
				const auto mo = m.toObject();
				if (mo.value("type").toString() == "User") {
					mentions.push_back({
						.userId = mo.value("id").toString(),
						.name = mo.value("name").toString(),
					});
				}
			}
			auto text = parseMentionedText(
				msgText, markdown, mentions, session);
			// The list is applied again on every reload: the preview item
			// is updated, a new one with the same id destroyed the old one.
			if (const auto existing = session->data().message(
					channel->id,
					rootId)) {
				if (existing->originalText() != text) {
					existing->setText(std::move(text));
					session->data().requestItemTextRefresh(existing);
				}
			} else {
				history->addNewLocalMessage(
					HistoryItemCommonFields{
						.id = rootId,
						.flags = msgFlags,
						.from = fromId,
						.date = date,
					},
					std::move(text),
					rootMedia);
			}
		}
		const auto parentPeerId = chatIdToPeerId(thread.chatId);
		// The parent message may never be loaded in its chat, but the thread
		// view resolves the root uuid through this map to load replies.
		if (msgIdToMtsLinkId(parentPeerId, rootId).isEmpty()) {
			registerMessageId(parentPeerId, rootId, thread.id);
		}
		ThreadTopicMap.insert(rootId, { thread.chatId, thread.id });
		ThreadPeerInfoMap.insert(peerId, { parentPeerId, rootId });
		ThreadReverseMap.insert({ parentPeerId, rootId }, peerId);
		refreshThreadsMark(session, parentPeerId);

		const auto authorUuidForMap =
			thread.message.value("authorId").toString();
		if (!authorUuidForMap.isEmpty()) {
			const auto authorBare = uuidToBareId(authorUuidForMap);
			const auto authorPeerId = PeerId(::UserId(authorBare));
			ThreadAuthorMap.insert(peerId, authorPeerId);
			const auto authorFileId = UserpicFileIds.value(authorPeerId);
			if (!authorFileId.isEmpty()) {
				applyUserpic(channel, authorFileId, __LINE__);
			}
		}

		// Keep exactly the server order: the list comes newest first, so
		// each next thread gets an earlier time than the previous one.
		auto activity = std::max(date, uuidV6Time(thread.lastChildId));
		if (previousActivity && activity >= previousActivity) {
			activity = previousActivity - 1;
		}
		previousActivity = activity;
		history->setChatListTimeId(activity);
		history->updateChatListExistence();
		session->data().refreshChatListEntry(
			Dialogs::Key(history));
	}

}

QList<Api::ChannelData> PendingChannelsList;
QList<Api::ChannelData> LoadedDialogsList;
bool DialogsApplied = false;
bool ActiveChatRestored = false;

void applyThreadLeft(
	not_null<Main::Session*> session,
	const QString &chatId,
	const QString &threadId);

// ChatTypingEvent {chatId, textMembers:[{name, userId}]},
// ThreadTypingEvent {threadId, textMembers}. Telegram expires the action
// by itself, the server repeats the event while the user types.
void handleTypingEvent(
		not_null<Main::Session*> session,
		not_null<Session*> mts,
		const QJsonObject &param) {
	const auto type = param.value("type").toString();
	const auto value = param.value("value").toObject();
	auto targets = std::vector<std::pair<not_null<History*>, MsgId>>();
	if (type == u"ChatTypingEvent"_q) {
		const auto chatId = value.value("chatId").toString();
		if (!ChatToPeerMap.contains(chatId)) {
			return;
		}
		if (const auto history = session->data().historyLoaded(
				chatIdToPeerId(chatId))) {
			targets.emplace_back(history, MsgId(0));
		}
	} else if (type == u"ThreadTypingEvent"_q) {
		const auto threadId = value.value("threadId").toString();
		const auto rootId = localMsgId(threadId);
		const auto topic = ThreadTopicMap.constFind(rootId);
		const auto known = MtsLinkIdToMsgMap.constFind(threadId);
		// Subscribed threads or any loaded root message.
		const auto parentPeerId = (topic != ThreadTopicMap.constEnd())
			? chatIdToPeerId(topic->first)
			: (known != MtsLinkIdToMsgMap.constEnd())
			? known->first
			: PeerId();
		if (!parentPeerId) {
			return;
		}
		if (const auto history = session->data().historyLoaded(
				parentPeerId)) {
			// The thread view of the chat.
			targets.emplace_back(history, rootId);
		}
		if (const auto threadPeerId = threadPeerFor(parentPeerId, rootId)) {
			// The thread row in the "Threads" folder.
			if (const auto history = session->data().historyLoaded(
					threadPeerId)) {
				targets.emplace_back(history, MsgId(0));
			}
		}
	} else {
		return;
	}
	const auto now = base::unixtime::now();
	for (const auto &member : value.value("textMembers").toArray()) {
		const auto obj = member.toObject();
		const auto userId = obj.value("userId").toString();
		if (userId.isEmpty() || userId == mts->userId()) {
			continue;
		}
		const auto bareId = uuidToBareId(userId);
		UserBareIdToUuidMap.insert(bareId, userId);
		const auto user = session->data().user(::UserId(bareId));
		if (user->name().isEmpty()) {
			const auto name = obj.value("name").toString();
			if (!name.isEmpty()) {
				user->setName(name, QString(), QString(), QString());
			}
		}
		for (const auto &[history, rootId] : targets) {
			session->data().sendActionManager().registerFor(
				history,
				rootId,
				user,
				MTP_sendMessageTypingAction(),
				now);
		}
	}
}

// The popular reactions first, then the whole MTS Link emoji catalogue for
// the expanded reactions selector.
[[nodiscard]] QStringList withMtsLinkCatalogue(QStringList popular) {
	ensureEmojiMapsInitialized();
	auto result = QStringList();
	auto seen = QSet<QString>();
	const auto add = [&](const QString &emoji) {
		auto key = emoji;
		key.remove(QChar(0xFE0F));
		if (!seen.contains(key) && Ui::Emoji::Find(emoji)) {
			seen.insert(key);
			result.push_back(emoji);
		}
	};
	for (const auto &emoji : popular) {
		add(emoji);
	}
	for (const auto &emoji : EmojiCatalogueOrder) {
		add(emoji);
	}
	LOG(("MtsLink Emoji: %1 reactions").arg(result.size()));
	return result;
}

void connectToSession(
		not_null<Main::Session*> mainSession,
		not_null<Session*> mtsSession) {
	ProfileDataPath = mainSession->account().local().basePath();
	loadCachedUserId();
	PendingChannelsList.clear();
	LoadedDialogsList.clear();
	DialogsApplied = false;
	ActiveChatRestored = false;
	// Before the messages: they are built from the cache after the list.
	restoreImagesAsFiles(mainSession);
	loadChatListFromCache(mainSession);

	rpl::merge(
		mainSession->data().chatsFilters().changed(),
		mainSession->data().pinnedDialogsOrderUpdated()
	) | rpl::on_next([=] {
		scheduleFolderPinsSync(mainSession);
	}, mainSession->lifetime());

	setTokenRefreshCallback([mtsSession] {
		mtsSession->auth()->refreshTokens();
	});
	QObject::connect(
		mtsSession->auth(),
		&Api::Auth::tokenRefreshed,
		mtsSession,
		[mainSession, mtsSession](const QString &newToken) {
			MtsLink::setFileAuthToken(newToken);
			TokenRefreshInProgress = false;
			const auto userId = mainSession->userId().bare;
			const auto deviceId = mtsSession->auth()->deviceId();
			mainSession->account().local().writeMtsLinkToken(
				newToken, userId, MtsLink::fileRefreshToken(), deviceId);
			LOG(("MtsLink: token refreshed, CDN auth updated"));
		});
	QObject::connect(
		mtsSession->auth(),
		&Api::Auth::authFailed,
		mtsSession,
		[](const QString &error) {
			TokenRefreshInProgress = false;
			LOG(("MtsLink: token refresh failed: %1").arg(error));
		});

	QObject::connect(
		mtsSession,
		&Session::initialized,
		mtsSession->messages(),
		[mainSession, mtsSession] {
			mtsSession->messages()->retryFailedLoads();
			saveCachedUserId(mtsSession->userId());
			// The messages lost with the connection are sent again.
			resendPendingSends(mainSession);
		});
	const auto refreshLastMessages = [mainSession, mtsSession](
			const QList<Api::ChannelData> &list) {
		for (const auto &ch : list) {
			if (ch.lastMessageTimestamp <= 0) {
				continue;
			}
			if (mtsSession->messages()->isLoading(ch.id)) {
				continue;
			}
			const auto peerId = chatIdToPeerId(ch.id);
			const auto history = mainSession->data().historyLoaded(peerId);
			const auto currentLast = history
				? history->lastMessage()
				: nullptr;
			const auto apiDate = TimeId(ch.lastMessageTimestamp / 1000);
			if (!currentLast) {
				mtsSession->messages()->loadPreview(ch.id, 1);
			} else if (apiDate > currentLast->date()) {
				// New messages while TeleLink was closed or disconnected:
				// the newest ones together, not only the last one (the
				// missed ones came later below it, out of order).
				LOG(("MtsLink NewMsg: %1 has newer messages, loading"
					).arg(ch.id));
				mtsSession->messages()->load(ch.id, {}, 50);
			}
		}
	};
	QObject::connect(
		mtsSession->channels(),
		&Api::Channels::channelsLoaded,
		[mainSession, refreshLastMessages](const QList<Api::ChannelData> &list) {
			if (DialogsApplied) {
				applyChatList(mainSession, list);
				removeStaleChannels(mainSession, list);
				refreshLastMessages(list);
				auto combined = LoadedDialogsList + list;
				saveChatListToCache(mainSession, combined);
			} else {
				PendingChannelsList = list;
			}
		});
	QObject::connect(
		mtsSession->channels(),
		&Api::Channels::dialogsLoaded,
		[mainSession, refreshLastMessages](const QList<Api::ChannelData> &list) {
			DialogsApplied = true;
			LoadedDialogsList = list;
			applyChatList(mainSession, list);
			removeStaleChannels(mainSession, list, ChatType::GroupChat);
			refreshLastMessages(list);
			if (!PendingChannelsList.isEmpty()) {
				applyChatList(mainSession, PendingChannelsList);
				removeStaleChannels(mainSession, PendingChannelsList);
				refreshLastMessages(PendingChannelsList);
				auto combined = list + PendingChannelsList;
				saveChatListToCache(mainSession, combined);
				PendingChannelsList.clear();
			}
		});
	QObject::connect(
		mtsSession->channels(),
		&Api::Channels::chatInfoLoaded,
		[mainSession, mtsSession](const Api::ChannelData &info) {
			auto ch = info;
			LOG(("MtsLink: chat info loaded %1 type=%2 role=%3"
				).arg(ch.id).arg(int(ch.type)).arg(ch.memberRole));
			// The members of a new chat: the full profiles (the dialog of
			// a new person had only the name and the userpic).
			for (const auto &profile : ch.memberProfiles) {
				const auto parsed = Api::ParseMemberProfile(profile);
				if (parsed.userId.isEmpty()) {
					continue;
				}
				applyUserData(mainSession, parsed);
				if (ch.type == ChatType::Dialog
					&& ch.interlocutorId.isEmpty()
					&& parsed.userId != mtsSession->userId()) {
					LOG(("MtsLink ChatInfo: dialog %1 interlocutor from "
						"the profiles: %2").arg(ch.id, parsed.userId));
					ch.interlocutorId = parsed.userId;
				}
			}
			if (ch.type == ChatType::Dialog
				|| ch.type == ChatType::Favorites) {
				applyDialogData(mainSession, ch);
			} else {
				applyChannelData(mainSession, ch);
				if (!ch.isReadOnly) {
					mtsSession->users()->loadChatMembers(ch.id);
				}
			}
			if (JustJoinedChats.remove(ch.id)) {
				const auto history = mainSession->data().history(
					chatIdToPeerId(ch.id));
				const auto now = base::unixtime::now();
				LOG(("MtsLink Channel: joined %1, last message time %2, "
					"list time %3 -> %4"
					).arg(ch.id
					).arg(ch.lastMessageTimestamp
					).arg(history->chatListTimeId()
					).arg(now));
				if (history->chatListTimeId() < now) {
					history->setChatListTimeId(now);
				}
				// The last message for the preview in the list.
				if (!history->lastMessage()) {
					mtsSession->messages()->loadPreview(ch.id, 1);
				}
			}
			const auto pending = PendingChatEvents.take(ch.id);
			ChatInfoRequested.remove(ch.id);
			for (const auto &ev : pending) {
				handleChatEvent(mainSession, ev.dst, ev.param);
			}
		});
	QObject::connect(
		mtsSession->messages(),
		&Api::Messages::previewLoaded,
		[mainSession, mtsSession](
				const ChatId &chatId,
				const QList<Api::MessageData> &messages,
				const QList<Api::MemberProfile> &profiles,
				int rawCount,
				int limit) {
			const auto peerId = chatIdToPeerId(chatId);
			const auto history = mainSession->data().history(peerId);
			if (!history->isEmpty() || history->lastMessage()) {
				return;
			}
			if (messages.isEmpty()) {
				// The newest messages may all be deleted, look deeper.
				if (rawCount == limit && limit < kPreviewRetryLimit) {
					mtsSession->messages()->loadPreview(
						chatId,
						kPreviewRetryLimit);
				}
				return;
			}
			for (const auto &p : profiles) {
				applyUserData(mainSession, p);
			}
			// Chat list preview only: create the item outside of blocks,
			// the full history loads when the chat opens.
			const auto &newest = messages.front();
			std::vector<not_null<HistoryItem*>> created;
			addMessage(mainSession, newest, false, &created);
			if (!created.empty()) {
				history->applyDialogTopMessage(created.back()->id);
			}
			saveMessagesToCache(mainSession, chatId, { newest }, profiles);
		});
	QObject::connect(
		mtsSession->messages(),
		&Api::Messages::deletedMessageSeen,
		[mainSession](const ChatId &chatId, const MessageId &messageId) {
			// Deleted while TeleLink was closed: still in the local cache.
			const auto peerId = chatIdToPeerId(chatId);
			const auto known = MtsLinkIdToMsgMap.constFind(messageId);
			const auto msgId = (known != MtsLinkIdToMsgMap.constEnd())
				? known->second
				: MsgId(uuidToBareId(messageId) & 0x7FFFFFFFLL);
			if (mainSession->data().message(peerId, msgId)) {
				LOG(("MtsLink Messages: %1 in %2 deleted on the server"
					).arg(messageId, chatId));
				deleteMessage(mainSession, chatId, messageId);
			}
		});
	QObject::connect(
		mtsSession->messages(),
		&Api::Messages::messagesLoaded,
		[mainSession, mtsSession](
				const ChatId &chatId,
				const QList<Api::MessageData> &messages,
				const QList<Api::MemberProfile> &profiles,
				const QString &rawLastId,
				int rawCount) {
			LOG(("MtsLink Messages: loaded %1 of %2 for %3"
				).arg(messages.size()).arg(rawCount).arg(chatId));
			for (const auto &p : profiles) {
				applyUserData(mainSession, p);
			}
			const auto peerId = chatIdToPeerId(chatId);
			TimeId newestExistingDate = 0;
			{
				const auto hist =
					mainSession->data().historyLoaded(peerId);
				if (hist) {
					if (const auto last = hist->lastMessage()) {
						newestExistingDate = last->date();
					}
				}
			}
			const bool hasCachedMessages = (newestExistingDate > 0);
			if (hasCachedMessages
				&& !messages.isEmpty()
				&& rawCount >= 50
				&& TimeId(messages.back().createdAt / 1000)
					> newestExistingDate) {
				// Diagnostics: more new messages than loaded, a gap.
				LOG(("MtsLink NewMsg: GAP in %1, the oldest loaded %2 is "
					"newer than the newest known %3"
					).arg(chatId
					).arg(messages.back().createdAt / 1000
					).arg(newestExistingDate));
			}
			std::vector<not_null<HistoryItem*>> newerItems;
			for (int i = messages.size() - 1; i >= 0; --i) {
				addMessage(
					mainSession,
					messages[i],
					false,
					hasCachedMessages ? &newerItems : nullptr);
			}
			// Set active call state from the newest Call message only.
			// messages[0] is the newest (server returns newest-first).
			for (int ci = 0; ci < messages.size(); ++ci) {
				const auto &cm = messages[ci];
				if (cm.type == MessageType::Call && cm.callMeta) {
					if (cm.callMeta->status == "Started"
						&& !cm.callMeta->joinLink.isEmpty()) {
						setActiveCall(mainSession, peerId, *cm.callMeta);
					} else {
						setActiveCall(mainSession, peerId, QString());
					}
					break;
				}
			}
			if (!newerItems.empty()) {
				const auto history =
					mainSession->data().history(peerId);
				history->addCreatedNewerSlice(newerItems);
				// Messages already shown are not in newerItems, so its last
				// item may be older than the current chat list preview.
				const auto newest = *ranges::max_element(
					newerItems,
					ranges::less(),
					[](not_null<HistoryItem*> item) { return item->date(); });
				const auto currentLast = history->lastMessage();
				if (!currentLast || newest->date() >= currentLast->date()) {
					history->applyDialogTopMessage(newest->id);
				}
				mainSession->data().notifyHistoryChangeDelayed(
					history);
			}
			{
				const auto history =
					mainSession->data().historyLoaded(peerId);
				if (history && !messages.isEmpty()) {
					const auto unread = history->unreadCount();
					if (unread > 0
						&& unread < int(messages.size())) {
						const auto &lastRead = messages[unread];
						const auto readDate = TimeId(
							lastRead.createdAt / 1000);
						history->setMtsLinkInboxReadDate(readDate);
					} else if (unread == 0
						&& !history->mtsLinkInboxReadDate()) {
						const auto &newest = messages[0];
						const auto readDate = TimeId(
							newest.createdAt / 1000);
						history->setMtsLinkInboxReadDate(readDate);
					}
				}
			}
			mainSession->data().sendHistoryChangeNotifications();
			saveMessagesToCache(mainSession, chatId, messages, profiles);
			refreshThreadsMark(mainSession, peerId);
			if (!rawLastId.isEmpty()
				&& oldestLoadedMessageId(peerId).isEmpty()) {
				setOldestLoadedMessageId(peerId, rawLastId);
			}
			if (!PinnedMessagesLoadedChats.contains(chatId)) {
				PinnedMessagesLoadedChats.insert(chatId);
				mtsSession->messages()->loadPinned(chatId, 5);
			}
			if (!ChatInfoLoadedChats.contains(chatId)) {
				ChatInfoLoadedChats.insert(chatId);
				mtsSession->channels()->loadChatInfo(chatId);
			}
			if (messages.size() < 20 && rawCount > 0
				&& !rawLastId.isEmpty()) {
				const auto conn = std::make_shared<QMetaObject::Connection>();
				const auto retryChatId = chatId;
				*conn = QObject::connect(
					mtsSession->messages(),
					&Api::Messages::olderMessagesLoaded,
					[mainSession, mtsSession, conn, retryChatId](
							const ChatId &cid,
							const QList<Api::MessageData> &msgs,
							const QList<Api::MemberProfile> &profs,
							const QString &rLastId,
							int rCount) {
						if (cid != retryChatId) {
							return;
						}
						const auto done = addOlderMessages(
							mainSession,
							cid,
							msgs,
							profs,
							rLastId,
							rCount);
						if (done) {
							QObject::disconnect(*conn);
						} else if (!rLastId.isEmpty()) {
							mtsSession->messages()->load(
								cid, rLastId, 50);
						} else {
							QObject::disconnect(*conn);
						}
					});
				mtsSession->messages()->load(chatId, rawLastId, 50);
			}
		});

	QObject::connect(
		mtsSession->rpc(),
		&Rpc::eventReceived,
		[mainSession, mtsSession](
				const QString &name,
				const QString &dst,
				const QJsonObject &param) {
			if (name == "ChatEvent") {
				handleChatEvent(mainSession, dst, param);
			} else if (name == "MessageSchedulerEvent") {
				handleSchedulerEvent(mainSession, param);
			} else if (name == "TypingEvent") {
				handleTypingEvent(mainSession, mtsSession, param);
			} else if (name == "OrganizationEvent") {
				const auto type = param.value("type").toString();
				const auto value = param.value("value").toObject();
				if (type == "MemberOnline"
					|| type == "MemberOffline") {
					const auto userId = value.value("userId").toString();
					if (!userId.isEmpty()) {
						const auto bareId = uuidToBareId(userId);
						const auto user = mainSession->data().user(::UserId(bareId));
						const auto online = (type == "MemberOnline");
						const auto status = online
							? Data::LastseenStatus::OnlineTill(
								base::unixtime::now() + kMtsLinkOnlineHorizon)
							: Data::LastseenStatus::Recently();
						PresenceKnownUsers.insert(user->id);
						if (user->updateLastseen(status)) {
							mainSession->data().session().changes().peerUpdated(
								user,
								Data::PeerUpdate::Flag::OnlineStatus);
						}
					}
				} else if (type == "CustomStatusChanged") {
					applyUserStatus(
						mainSession,
						value.value("userId").toString(),
						value);
					if (value.value("userId").toString()
							== mtsSession->userId()) {
						LOG(("MtsLink Status: changed to '%1'"
							).arg(value.value("status").toString()));
						applyMyStatusChanged(value);
					}
				} else if (type == "MemberProfileChanged") {
					const auto userId = value.value("userId").toString();
					const auto prof = value.value("profile").toObject();
					if (!userId.isEmpty() && !prof.isEmpty()) {
						applyUserData(mainSession, Api::MemberProfile{
							.userId = userId,
							.organizationId = value.value(
								"organizationId").toString(),
							.email = prof.value("email").toString(),
							.phone = prof.value("phone").toString(),
							.position = prof.value("position").toString(),
							.department = prof.value("department").toString(),
							.firstName = prof.value("firstName").toString(),
							.lastName = prof.value("lastName").toString(),
							.displayName = prof.value("displayName").toString(),
							.avatarFileId = prof.value("avatarFileId").toString(),
							.role = MemberRole::Member,
							.additionalFields = [&] {
								auto parsed = Api::MemberProfile();
								Api::ParseProfileDetails(prof, parsed);
								return parsed.additionalFields;
							}(),
							.detailsKnown = true,
						}, true);
						if (userId == mtsSession->userId()) {
							applyMyProfileChanged(mainSession, prof);
						}
					}
				} else if (type == "MemberInCallChanged") {
					const auto userId = value.value("userId").toString();
					if (!userId.isEmpty()) {
						const auto user = mainSession->data().user(
							::UserId(uuidToBareId(userId)));
						const auto inCall = value.value("inCall").toBool();
						// Joined the call on another device (the MTS Link
						// app or the browser): no more ringing here.
						if (inCall && userId == mtsSession->userId()) {
							LOG(("MtsLink Call: I am in a call elsewhere"));
							Core::App().calls().mtsLinkAnsweredElsewhere();
						}
						const auto changed = inCall
							? !InCallUsers.contains(user->id)
							: InCallUsers.contains(user->id);
						if (changed) {
							if (inCall) {
								InCallUsers.insert(user->id);
							} else {
								InCallUsers.remove(user->id);
							}
							mainSession->changes().peerUpdated(
								user,
								Data::PeerUpdate::Flag::OnlineStatus);
						}
					}
				}
			} else if (name == "NotificationEvent") {
				handleNotificationEvent(mainSession, param);
			}
		});

	QObject::connect(
		mtsSession->users(),
		&Api::Users::memberLoaded,
		[mainSession](const Api::MemberProfile &profile) {
			applyUserData(mainSession, profile);
			const auto authorPeerId = PeerId(
				::UserId(uuidToBareId(profile.userId)));
			for (auto it = ThreadAuthorMap.constBegin();
				it != ThreadAuthorMap.constEnd(); ++it) {
				if (it.value() != authorPeerId) {
					continue;
				}
				const auto history = mainSession->data().historyLoaded(
					it.key());
				if (const auto last = history
						? history->lastMessage()
						: nullptr) {
					last->invalidateChatListEntry();
				}
				if (history) {
					// The letters of the author in the thread userpic.
					mainSession->changes().peerUpdated(
						history->peer,
						Data::PeerUpdate::Flag::Photo);
				}
			}
			const auto pending = PendingUserEvents.take(profile.userId);
			for (const auto &ev : pending) {
				handleChatEvent(mainSession, ev.dst, ev.param);
			}
		});
	QObject::connect(
		mtsSession->users(),
		&Api::Users::membersLoaded,
		[mainSession](const QList<Api::MemberProfile> &members) {
			for (const auto &m : members) {
				applyUserData(mainSession, m);
			}
			for (auto it = ThreadPeerInfoMap.constBegin();
				it != ThreadPeerInfoMap.constEnd(); ++it) {
				const auto history = mainSession->data().historyLoaded(
					it.key());
				if (history) {
					if (const auto last = history->lastMessage()) {
						last->invalidateChatListEntry();
					}
				}
			}
		});
	QObject::connect(
		mtsSession->users(),
		&Api::Users::presenceLoaded,
		[mainSession](
				const UserId &userId,
				MemberPresence presence,
				bool inCall) {
			const auto user = mainSession->data().user(
				::UserId(uuidToBareId(userId)));
			PresenceKnownUsers.insert(user->id);
			const auto status = (presence == MemberPresence::Online)
				? Data::LastseenStatus::OnlineTill(
					base::unixtime::now() + kMtsLinkOnlineHorizon)
				: Data::LastseenStatus::Recently();
			auto changed = user->updateLastseen(status);
			if (inCall != InCallUsers.contains(user->id)) {
				if (inCall) {
					InCallUsers.insert(user->id);
				} else {
					InCallUsers.remove(user->id);
				}
				changed = true;
			}
			if (changed) {
				mainSession->changes().peerUpdated(
					user,
					Data::PeerUpdate::Flag::OnlineStatus);
			}
		});
	QObject::connect(
		mtsSession->users(),
		&Api::Users::chatMembersLoaded,
		[mainSession, mtsSession](
				const ChatId &chatId,
				const QList<Api::MemberProfile> &members) {
			MembersLoadedAt.insert(chatId, crl::now());
			const auto peerId = chatIdToPeerId(chatId);
			// The presence of each member was requested (Member.GetMember):
			// 200 requests for a big group, the server rate limit. Only for
			// the dialogs, the others come with MemberOnline/MemberOffline
			// and with an opened profile.
			const auto requestPresence = (chatTypeForPeer(peerId)
				== ChatType::Dialog);
			auto &stored = ChatMembersMap[peerId];
			stored.clear();
			stored.reserve(members.size());
			for (const auto &m : members) {
				applyUserData(mainSession, m);
				const auto bareId = uuidToBareId(m.userId);
				stored.push_back(bareId);
				const auto userPeerId = PeerId(::UserId(bareId));
				if (requestPresence
					&& !PresenceKnownUsers.contains(userPeerId)
					&& !PresenceRequestedUsers.contains(userPeerId)) {
					PresenceRequestedUsers.insert(userPeerId);
					mtsSession->users()->loadPresence(
						m.userId,
						mtsSession->organizationId());
				}
			}
			const auto chatType = chatTypeForPeer(peerId);
			if (chatType == ChatType::Dialog) {
				const auto selfId = mtsSession->userId();
				for (const auto &m : members) {
					if (m.userId != selfId
							&& !m.avatarFileId.isEmpty()) {
						const auto peer = mainSession->data()
							.peerLoaded(peerId);
						if (peer && !peer->userpicPhotoId()) {
							applyUserpic(peer, m.avatarFileId, __LINE__);
						}
						break;
					}
				}
			}
			if (const auto channel = mainSession->data().channelLoaded(
					peerToChannel(peerId))) {
				const auto selfId = mtsSession->userId();
				auto rolesLog = QStringList();
				for (const auto &m : members) {
					if (m.role == MemberRole::Owner) {
						rolesLog.push_back(m.userId + u"=Owner"_q);
					} else if (m.role == MemberRole::Admin) {
						rolesLog.push_back(m.userId + u"=Admin"_q);
					}
				}
				ChannelOwners.remove(channel->id);
				auto &roles = ChatMembersRoles[channel->id];
				roles.clear();
				for (const auto &m : members) {
					roles.insert(
						PeerId(::UserId(uuidToBareId(m.userId))),
						m.role);
					if (m.role == MemberRole::Owner) {
						ChannelOwners.insert(
							channel->id,
							PeerId(::UserId(uuidToBareId(m.userId))));
					}
					if (m.userId == selfId) {
						applyMyChannelRole(
							channel,
							(m.role == MemberRole::Owner)
								? u"Owner"_q
								: (m.role == MemberRole::Admin)
								? u"Admin"_q
								: u"Member"_q);
					}
				}
				if (const auto mega = channel->asMegagroup()) {
					if (mega->mgInfo) {
						const auto owner = ChannelOwners.value(channel->id);
						mega->mgInfo->creator = owner
							? mainSession->data().userLoaded(peerToUser(owner))
							: nullptr;
						mega->mgInfo->lastParticipants.clear();
						mega->mgInfo->lastAdmins.clear();
						for (const auto &m : members) {
							const auto bareId = uuidToBareId(m.userId);
							const auto user = mainSession->data()
								.userLoaded(::UserId(bareId));
							if (!user) {
								continue;
							}
							mega->mgInfo->lastParticipants.push_back(
								user);
							if (m.role == MemberRole::Admin
								|| m.role == MemberRole::Owner) {
								mega->mgInfo->lastAdmins.emplace(
									user,
									MegagroupInfo::Admin(
										ChatAdminRightsInfo()));
							}
						}
						mega->mgInfo->lastParticipantsStatus
							= MegagroupInfo::LastParticipantsUpToDate
							| MegagroupInfo::LastParticipantsOnceReceived;
						mega->mgInfo->lastParticipantsCount
							= mega->membersCount();
						mainSession->changes().peerUpdated(
							mega,
							Data::PeerUpdate::Flag::Members);
					}
				} else {
					// Broadcast channels: the members box rebuilds roles.
					mainSession->changes().peerUpdated(
						channel,
						Data::PeerUpdate::Flag::Members);
				}
			}
		});
	QObject::connect(
		mtsSession->messages(),
		&Api::Messages::pinnedMessagesLoaded,
		[mainSession](
				const ChatId &chatId,
				const QList<Api::MessageData> &messages,
				const QList<Api::MemberProfile> &profiles,
				int total) {
			for (const auto &p : profiles) {
				applyUserData(mainSession, p);
			}
			const auto peerId = chatIdToPeerId(chatId);
			std::vector<MsgId> pinnedIds;
			pinnedIds.reserve(messages.size());
			std::vector<not_null<HistoryItem*>> batchItems;
			for (const auto &src : messages) {
				const auto item = addMessage(
					mainSession, src, false, &batchItems);
				if (item) {
					item->setIsPinned(true);
					pinnedIds.push_back(item->id);
				}
			}
			{
				auto &byDate = PinnedByDate[peerId];
				byDate.clear();
				for (const auto id : pinnedIds) {
					if (const auto item = mainSession->data().message(
							peerId,
							id)) {
						byDate.push_back({ item->date(), id });
					}
				}
				std::sort(byDate.begin(), byDate.end());
			}
			if (!pinnedIds.empty()) {
				const auto history = mainSession->data()
					.historyLoaded(peerId);
				if (history) {
					history->setHasPinnedMessages(true);
				}
				std::sort(pinnedIds.begin(), pinnedIds.end());
				mainSession->storage().add(
					Storage::SharedMediaAddSlice(
						peerId,
						MsgId(0),
						PeerId(0),
						Storage::SharedMediaType::Pinned,
						std::move(pinnedIds),
						{ 0, ServerMaxMsgId },
						total));
			}
		});

	QObject::connect(
		mtsSession->threads(),
		&Api::Threads::threadsLoaded,
		[mainSession, mtsSession](
				const QList<Api::ThreadData> &threads,
				const QList<Api::MemberProfile> &profiles) {
			for (const auto &profile : profiles) {
				if (!profile.userId.isEmpty()) {
					applyUserData(mainSession, profile);
				}
			}
			// Authors missing in the profiles are loaded separately,
			// the preview shows the name once the profile arrives.
			auto missing = QSet<QString>();
			for (const auto &thread : threads) {
				const auto authorId = thread.message.value("authorId")
					.toString();
				if (authorId.isEmpty() || missing.contains(authorId)) {
					continue;
				}
				const auto user = mainSession->data().userLoaded(
					::UserId(uuidToBareId(authorId)));
				if (!user || user->name().isEmpty()) {
					missing.insert(authorId);
				}
			}
			applyThreadsList(mainSession, threads);
			for (const auto &authorId : missing) {
				if (!UserProfileRequested.contains(authorId)) {
					UserProfileRequested.insert(authorId);
					mtsSession->users()->loadMember(
						authorId,
						mtsSession->organizationId());
				}
			}
		});
	QObject::connect(
		mtsSession->threads(),
		&Api::Threads::threadJoined,
		[mtsSession](const ChatId &, const MessageId &threadId) {
			LOG(("MtsLink Thread: joined %1").arg(threadId));
			ThreadLoadRequested.insert(threadId);
			mtsSession->threads()->loadThread(threadId);
		});
	QObject::connect(
		mtsSession->threads(),
		&Api::Threads::threadNotificationsChanged,
		[mainSession](
				const ChatId &chatId,
				const MessageId &threadId,
				bool isNotifiable) {
			applyThreadNotifiable(mainSession, chatId, threadId, isNotifiable);
		});
	QObject::connect(
		mtsSession->threads(),
		&Api::Threads::threadLeft,
		[mainSession](const ChatId &chatId, const MessageId &threadId) {
			LOG(("MtsLink Thread: left %1").arg(threadId));
			applyThreadLeft(mainSession, chatId, threadId);
		});

	{
		using Flag = Data::ChatFilter::Flag;
		const auto filterId = FilterId(5);
		bool hasThreadsFolder = false;
		for (const auto &f : mainSession->data().chatsFilters().list()) {
			if (f.flags() & Flag::Threads) {
				hasThreadsFolder = true;
				break;
			}
		}
		if (!hasThreadsFolder) {
			mainSession->data().chatsFilters().set(Data::ChatFilter(
				filterId,
				Data::ChatFilterTitle{
					{ tr::lng_threads_folder(tr::now) } },
				QString(),
				std::nullopt,
				Flag::Threads,
				{},
				{},
				{}));
		}
	}

	mainSession->data().reactions().populateMtsLinkReactions(withMtsLinkCatalogue({
		QString::fromUtf8("\xF0\x9F\x91\x8D"),     // 👍
		QString::fromUtf8("\xF0\x9F\x91\x8E"),     // 👎
		QString::fromUtf8("\xE2\x9D\xA4"),          // ❤
		QString::fromUtf8("\xF0\x9F\x94\xA5"),     // 🔥
		QString::fromUtf8("\xF0\x9F\xA5\xB0"),     // 🥰
		QString::fromUtf8("\xF0\x9F\x91\x8F"),     // 👏
		QString::fromUtf8("\xF0\x9F\x98\x81"),     // 😁
		QString::fromUtf8("\xF0\x9F\xA4\x94"),     // 🤔
		QString::fromUtf8("\xF0\x9F\xA4\xAF"),     // 🤯
		QString::fromUtf8("\xF0\x9F\x98\xB1"),     // 😱
		QString::fromUtf8("\xF0\x9F\xA4\xAC"),     // 🤬
		QString::fromUtf8("\xF0\x9F\x98\xA2"),     // 😢
		QString::fromUtf8("\xF0\x9F\x8E\x89"),     // 🎉
		QString::fromUtf8("\xF0\x9F\xA4\xA9"),     // 🤩
		QString::fromUtf8("\xF0\x9F\xA4\xAE"),     // 🤮
		QString::fromUtf8("\xF0\x9F\x92\xA9"),     // 💩
		QString::fromUtf8("\xF0\x9F\x99\x8F"),     // 🙏
		QString::fromUtf8("\xF0\x9F\x91\x8C"),     // 👌
		QString::fromUtf8("\xF0\x9F\x95\x8A"),     // 🕊
		QString::fromUtf8("\xF0\x9F\xA4\xA1"),     // 🤡
		QString::fromUtf8("\xF0\x9F\xA5\xB1"),     // 🥱
		QString::fromUtf8("\xF0\x9F\xA5\xB4"),     // 🥴
		QString::fromUtf8("\xF0\x9F\x98\x8D"),     // 😍
		QString::fromUtf8("\xF0\x9F\x90\xB3"),     // 🐳
		QString::fromUtf8("\xE2\x9D\xA4\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x94\xA5"), // ❤‍🔥
		QString::fromUtf8("\xF0\x9F\x8C\x9A"),     // 🌚
		QString::fromUtf8("\xF0\x9F\x8C\xAD"),     // 🌭
		QString::fromUtf8("\xF0\x9F\x92\xAF"),     // 💯
		QString::fromUtf8("\xF0\x9F\xA4\xA3"),     // 🤣
		QString::fromUtf8("\xE2\x9A\xA1"),          // ⚡
		QString::fromUtf8("\xF0\x9F\x8D\x8C"),     // 🍌
		QString::fromUtf8("\xF0\x9F\x8F\x86"),     // 🏆
		QString::fromUtf8("\xF0\x9F\x92\x94"),     // 💔
		QString::fromUtf8("\xF0\x9F\xA4\x9D"),     // 🤝
		QString::fromUtf8("\xF0\x9F\x98\x98"),     // 😘
		QString::fromUtf8("\xF0\x9F\x91\x80"),     // 👀
		QString::fromUtf8("\xF0\x9F\x98\x86"),     // 😆
		QString::fromUtf8("\xF0\x9F\x98\x85"),     // 😅
		QString::fromUtf8("\xF0\x9F\xA5\xB2"),     // 🥲
		QString::fromUtf8("\xF0\x9F\x98\x92"),     // 😒
		QString::fromUtf8("\xF0\x9F\x98\xB0"),     // 😰
		QString::fromUtf8("\xF0\x9F\x98\xA5"),     // 😥
		QString::fromUtf8("\xF0\x9F\x98\x80"),     // 😀
		QString::fromUtf8("\xE2\x9E\x95"),          // ➕
		QString::fromUtf8("\xF0\x9F\x8C\xB4"),     // 🌴
	}));

	mtsSession->users()->loadOrganizationMembers();
	// The titles of the organization profile fields, for the profiles.
	requestProfileFields(mainSession);
	restoreSavedGifs(mainSession);
	restoreLocalStickers(mainSession);
	// No archiving in MTS Link: the swipe "Archive" is disabled.
	{
		using Action = Dialogs::Ui::QuickDialogAction;
		auto &settings = Core::App().settings();
		if (settings.quickDialogAction() == Action::Archive) {
			settings.setQuickDialogAction(Action::Disabled);
			Core::App().saveSettingsDelayed();
		}
	}
	// The default notification settings are kept locally.
	restoreDefaultNotify(mainSession);
}

BareId uuidToBareId(const QString &uuid) {
	const auto hash = QCryptographicHash::hash(
		uuid.toUtf8(),
		QCryptographicHash::Md5);
	BareId result = 0;
	memcpy(&result, hash.constData(), qMin(int(sizeof(result)), hash.size()));
	// Keep within 48-bit range and ensure non-zero.
	return (result & BareId(0x0000FFFFFFFFFFFFULL)) | BareId(1);
}

PeerId chatIdToPeerId(const QString &chatId, ChatType type) {
	const auto it = ChatToPeerMap.constFind(chatId);
	if (it != ChatToPeerMap.constEnd()) {
		return it.value();
	}
	const auto bareId = uuidToBareId(chatId);
	const auto peerId = (type == ChatType::Dialog)
		? PeerId(::UserId(bareId))
		: PeerId(ChannelId(bareId));
	PeerToChatMap.insert(peerId, chatId);
	ChatToPeerMap.insert(chatId, peerId);
	PeerToChatTypeMap.insert(peerId, type);
	return peerId;
}

PeerId chatIdToPeerId(const QString &chatId) {
	const auto it = ChatToPeerMap.constFind(chatId);
	if (it != ChatToPeerMap.constEnd()) {
		return it.value();
	}
	return chatIdToPeerId(chatId, ChatType::Channel);
}

QString peerIdToChatId(PeerId peerId) {
	return PeerToChatMap.value(peerId);
}

bool hasChatId(PeerId peerId) {
	return PeerToChatMap.contains(peerId);
}

void markReadRequestSent(const QString &chatId) {
	ReadRequestSentChats.insert(chatId);
}

bool consumeReadRequestSent(const QString &chatId) {
	return ReadRequestSentChats.remove(chatId);
}

ChatType chatTypeForPeer(PeerId peerId) {
	return PeerToChatTypeMap.value(peerId, ChatType::Channel);
}

PeerId favoritesPeerId() {
	return FavoritesPeerIdValue;
}

std::vector<QString> searchEmojiByName(const QString &query, bool exact) {
	const auto normalized = NormalizeEmojiName(query);
	if (normalized.isEmpty()) {
		return {};
	}
	// 0: the name, 1: a keyword, 2: a word of a keyword (prefix matches).
	auto scored = std::vector<std::pair<int, int>>();
	const auto &list = EmojiNamesList();
	for (auto i = 0, count = int(list.size()); i != count; ++i) {
		auto best = -1;
		const auto &keywords = list[i].keywords;
		for (auto k = 0, kc = int(keywords.size()); k != kc; ++k) {
			const auto &keyword = keywords[k];
			if (exact) {
				if (keyword == normalized) {
					best = 0;
					break;
				}
				continue;
			}
			auto score = -1;
			if (keyword.startsWith(normalized)) {
				score = (k == 0) ? 0 : 1;
			} else {
				for (const auto &word : keyword.split(QChar(' '))) {
					if (word.startsWith(normalized)) {
						score = 2;
						break;
					}
				}
			}
			if (score >= 0 && (best < 0 || score < best)) {
				best = score;
			}
		}
		if (best >= 0) {
			scored.push_back({ best, i });
		}
	}
	ranges::stable_sort(scored, ranges::less(), [](const auto &p) {
		return p.first;
	});
	auto result = std::vector<QString>();
	result.reserve(scored.size());
	for (const auto &[score, index] : scored) {
		result.push_back(list[index].emoji);
	}
	return result;
}

QString emojiName(const QString &emoji) {
	const auto key = QString(emoji).remove(QChar(0xFE0F));
	for (const auto &entry : EmojiNamesList()) {
		if (entry.emoji == key) {
			return entry.name;
		}
	}
	return QString();
}

std::vector<MsgId> callMaterialMessages(PeerId peerId, MsgId rootId) {
	return CallMaterialItems.value(FullMsgId(peerId, rootId));
}

bool isThreadPeer(PeerId peerId) {
	return PeerToChatTypeMap.value(peerId) == ChatType::Thread;
}

QPair<PeerId, MsgId> threadParentInfo(PeerId threadPeerId) {
	return ThreadPeerInfoMap.value(threadPeerId);
}

QPair<ChatId, MessageId> threadTopicInfo(MsgId rootId) {
	return ThreadTopicMap.value(rootId);
}

const QHash<PeerId, QPair<PeerId, MsgId>> &threadPeerMap() {
	return ThreadPeerInfoMap;
}

PeerId threadAuthorPeerId(PeerId threadPeerId) {
	return ThreadAuthorMap.value(threadPeerId);
}

void applyPendingThreadUnreads(
		not_null<Main::Session*> session,
		PeerId chatPeerId) {
	auto it = PendingThreadUnread.begin();
	while (it != PendingThreadUnread.end()) {
		if (it.key().first != chatPeerId) {
			++it;
			continue;
		}
		const auto parentMsgId = it.key().second;
		const auto &info = it.value();
		const auto parent = session->data().message(
			chatPeerId, parentMsgId);
		if (parent) {
			if (const auto views
				= parent->Get<HistoryMessageViews>()) {
				HistoryMessageRepliesData repliesData;
				repliesData.isNull = false;
				repliesData.repliesCount =
					views->replies.count + info.count;
				repliesData.maxId =
					views->commentsMaxId + MsgId(info.count);
				LOG(("MtsLink applyPending: parent=%1 "
					"count=%2 newMax=%3")
					.arg(parentMsgId.bare)
					.arg(info.count)
					.arg((views->commentsMaxId
						+ MsgId(info.count)).bare));
				parent->setReplies(std::move(repliesData));
				session->data().requestItemViewRefresh(parent);
			}
			it = PendingThreadUnread.erase(it);
		} else {
			++it;
		}
	}
}

int pendingThreadUnreadCount(PeerId chatPeerId) {
	int count = 0;
	for (auto it = PendingThreadUnread.constBegin();
		it != PendingThreadUnread.constEnd(); ++it) {
		if (it.key().first == chatPeerId) {
			++count;
		}
	}
	return count;
}

MsgId firstPendingThreadUnreadParent(PeerId chatPeerId) {
	for (auto it = PendingThreadUnread.constBegin();
		it != PendingThreadUnread.constEnd(); ++it) {
		if (it.key().first == chatPeerId) {
			return it.key().second;
		}
	}
	return MsgId(0);
}

QString firstPendingThreadUnreadUuid(PeerId chatPeerId) {
	for (auto it = PendingThreadUnread.constBegin();
		it != PendingThreadUnread.constEnd(); ++it) {
		if (it.key().first == chatPeerId) {
			return it.value().parentUuid;
		}
	}
	return {};
}

namespace {

History *threadEntryHistory(
		not_null<Main::Session*> session,
		PeerId parentPeerId,
		MsgId rootId) {
	const auto it = ThreadReverseMap.constFind({ parentPeerId, rootId });
	if (it == ThreadReverseMap.constEnd()) {
		return nullptr;
	}
	const auto history = session->data().historyLoaded(it.value());
	return (history && history->folderKnown()) ? history : nullptr;
}

} // namespace

MsgId currentOpenThreadRoot(PeerId parentPeerId) {
	return CurrentOpenThreads.value(parentPeerId);
}

MsgId pinnedToShow(
		PeerId peerId,
		TimeId visibleBottomDate,
		MsgId clickedId) {
	const auto i = PinnedByDate.constFind(peerId);
	if (i == PinnedByDate.constEnd() || i->empty()) {
		return MsgId();
	}
	const auto &list = *i; // Oldest first.
	if (clickedId) {
		const auto j = ranges::find(
			list,
			clickedId,
			&std::pair<TimeId, MsgId>::second);
		if (j != end(list)) {
			// After the oldest one we go back to the newest, like Telegram.
			return (j == begin(list)) ? list.back().second : (j - 1)->second;
		}
	}
	for (auto j = list.rbegin(); j != list.rend(); ++j) {
		if (j->first <= visibleBottomDate) {
			return j->second;
		}
	}
	return list.front().second;
}

int pinnedDateIndex(PeerId peerId, MsgId msgId) {
	const auto i = PinnedByDate.constFind(peerId);
	if (i == PinnedByDate.constEnd()) {
		return -1;
	}
	const auto j = ranges::find(*i, msgId, &std::pair<TimeId, MsgId>::second);
	return (j == end(*i)) ? -1 : int(j - begin(*i));
}

TimeId pinnedDate(PeerId peerId, MsgId msgId) {
	const auto i = PinnedByDate.constFind(peerId);
	if (i == PinnedByDate.constEnd()) {
		return 0;
	}
	const auto j = ranges::find(*i, msgId, &std::pair<TimeId, MsgId>::second);
	return (j == end(*i)) ? 0 : j->first;
}

PeerId threadPeerFor(PeerId parentPeerId, MsgId rootId) {
	return ThreadReverseMap.value({ parentPeerId, rootId });
}

void requestMessageData(
		not_null<Main::Session*> session,
		PeerId peerId,
		MsgId msgId,
		Fn<void()> done) {
	const auto mts = session->account().mtsLinkSession();
	const auto chatId = peerIdToChatId(peerId);
	const auto rootUuid = msgIdToMtsLinkId(peerId, msgId);
	if (!mts || !mts->messages() || chatId.isEmpty() || rootUuid.isEmpty()) {
		LOG(("MtsLink: can't load message chat='%1' msg='%2' (id %3)")
			.arg(chatId)
			.arg(rootUuid)
			.arg(msgId.bare));
		if (done) {
			done();
		}
		return;
	}
	const auto weak = base::make_weak(session);
	const auto conn = std::make_shared<QMetaObject::Connection>();
	*conn = QObject::connect(
		mts->messages(),
		&Api::Messages::aroundMessagesLoaded,
		[=](
				const ChatId &loadedChatId,
				const MessageId &targetId,
				const QList<Api::MessageData> &messages,
				const QList<Api::MemberProfile> &profiles) {
			if (loadedChatId != chatId || targetId != rootUuid) {
				return;
			}
			QObject::disconnect(*conn);
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			for (const auto &p : profiles) {
				applyUserData(strong, p);
			}
			auto found = false;
			for (const auto &message : messages) {
				if (message.id == rootUuid) {
					addMessage(strong, message, true);
					found = true;
				}
			}
			LOG(("MtsLink: message %1 loaded=%2 (messages=%3)")
				.arg(rootUuid)
				.arg(found ? 1 : 0)
				.arg(messages.size()));
			if (done) {
				done();
			}
		});
	// "Around" with limit 2 loads one message after the previous one: the root.
	mts->messages()->loadAround(chatId, rootUuid, 2);
}

bool isThreadSubscribed(
		not_null<Main::Session*> session,
		PeerId parentPeerId,
		MsgId rootId) {
	const auto it = ThreadReverseMap.constFind({ parentPeerId, rootId });
	if (it == ThreadReverseMap.constEnd()) {
		return false;
	}
	const auto channel = session->data().channelLoaded(
		peerToChannel(it.value()));
	return channel && channel->amIn();
}

void setThreadSubscribed(
		not_null<Main::Session*> session,
		PeerId parentPeerId,
		MsgId rootId,
		bool subscribed) {
	const auto mts = session->account().mtsLinkSession();
	const auto chatId = peerIdToChatId(parentPeerId);
	const auto threadId = msgIdToMtsLinkId(parentPeerId, rootId);
	if (!mts || !mts->threads() || chatId.isEmpty() || threadId.isEmpty()) {
		LOG(("MtsLink Thread: can't change subscription chat='%1' thread='%2'")
			.arg(chatId)
			.arg(threadId));
		return;
	}
	if (subscribed) {
		mts->threads()->joinThread(chatId, threadId);
	} else {
		mts->threads()->leaveThread(chatId, threadId);
	}
}

void fillThreadSubscriptionActions(
		not_null<Main::Session*> session,
		PeerId parentPeerId,
		MsgId rootId,
		Fn<void(const QString&, Fn<void()>, const style::icon*)> addAction,
		bool withSubscribe) {
	const auto subscribed = isThreadSubscribed(session, parentPeerId, rootId);
	if (withSubscribe || subscribed) {
		addAction((subscribed
			? tr::lng_mtslink_thread_unsubscribe
			: tr::lng_mtslink_thread_subscribe)(tr::now), [=] {
			setThreadSubscribed(session, parentPeerId, rootId, !subscribed);
		}, subscribed ? &st::menuIconLeave : &st::menuIconAddToFolder);
	}
	if (subscribed) {
		const auto notifiable = isThreadNotifiable(parentPeerId, rootId);
		addAction((notifiable
			? tr::lng_mtslink_thread_mute
			: tr::lng_mtslink_thread_unmute)(tr::now), [=] {
			setThreadNotifiable(session, parentPeerId, rootId, !notifiable);
		}, notifiable ? &st::menuIconMute : &st::menuIconUnmute);
	}
}

bool isThreadNotifiable(PeerId parentPeerId, MsgId rootId) {
	const auto it = ThreadReverseMap.constFind({ parentPeerId, rootId });
	return (it == ThreadReverseMap.constEnd())
		|| ThreadNotifiable.value(it.value(), true);
}

void setThreadNotifiable(
		not_null<Main::Session*> session,
		PeerId parentPeerId,
		MsgId rootId,
		bool notifiable) {
	const auto mts = session->account().mtsLinkSession();
	const auto chatId = peerIdToChatId(parentPeerId);
	const auto threadId = msgIdToMtsLinkId(parentPeerId, rootId);
	if (mts && mts->threads() && !chatId.isEmpty() && !threadId.isEmpty()) {
		mts->threads()->setThreadNotifications(chatId, threadId, notifiable);
	}
}

void applyThreadNotifiable(
		not_null<Main::Session*> session,
		PeerId threadPeerId,
		bool notifiable) {
	ThreadNotifiable.insert(threadPeerId, notifiable);
	if (const auto peer = session->data().peerLoaded(threadPeerId)) {
		session->data().notifySettings().apply(
			peer,
			makeMuteSettings(!notifiable));
	}
}

void applyThreadNotifiable(
		not_null<Main::Session*> session,
		const QString &chatId,
		const QString &threadId,
		bool notifiable) {
	const auto parentPeerId = chatIdToPeerId(chatId);
	const auto rootId = localMsgId(threadId);
	const auto it = ThreadReverseMap.constFind({ parentPeerId, rootId });
	if (it != ThreadReverseMap.constEnd()) {
		applyThreadNotifiable(session, it.value(), notifiable);
	}
}

void applyThreadLeft(
		not_null<Main::Session*> session,
		const QString &chatId,
		const QString &threadId) {
	const auto parentPeerId = chatIdToPeerId(chatId);
	const auto rootId = localMsgId(threadId);
	const auto it = ThreadReverseMap.constFind({ parentPeerId, rootId });
	if (it == ThreadReverseMap.constEnd()) {
		return;
	}
	const auto channel = session->data().channelLoaded(
		peerToChannel(it.value()));
	if (!channel) {
		return;
	}
	channel->setFlags(channel->flags() | ChannelDataFlag::Left);
	if (const auto history = session->data().historyLoaded(channel)) {
		history->setUnreadCount(0);
		// Threads live in the "Threads" folder list only, refreshing
		// drops the entry there, as ChatFilter::contains() is false now.
		session->data().refreshChatListEntry(Dialogs::Key(history));
	}
	ThreadLoadRequested.remove(threadId);
	refreshThreadsMark(session, parentPeerId);
}

void addThreadEntryUnread(
		not_null<Main::Session*> session,
		PeerId parentPeerId,
		MsgId rootId,
		int delta,
		TimeId date) {
	const auto history = threadEntryHistory(session, parentPeerId, rootId);
	if (!history) {
		return;
	}
	if (delta > 0) {
		history->setUnreadCount(history->unreadCount() + delta);
	}
	if (date > history->chatListTimeId()) {
		history->setChatListTimeId(date);
	}
	session->data().refreshChatListEntry(Dialogs::Key(history));
}

int threadEntryUnreadCount(PeerId parentPeerId, MsgId rootId) {
	const auto it = ThreadReverseMap.constFind({ parentPeerId, rootId });
	if (it == ThreadReverseMap.constEnd()) {
		return -1;
	}
	for (const auto &account : Core::App().domain().accounts()) {
		if (const auto session = account.account->maybeSession()) {
			if (const auto history = session->data().historyLoaded(
					it.value())) {
				return history->unreadCountKnown()
					? history->unreadCount()
					: -1;
			}
		}
	}
	return -1;
}

// Diagnostics: all the chats and threads with something unread.
void refreshThreadsMark(
		not_null<Main::Session*> session,
		PeerId chatPeerId) {
	const auto history = session->data().historyLoaded(chatPeerId);
	if (!history) {
		return;
	}
	const auto subscribed = [&](MsgId rootId) {
		const auto i = ThreadReverseMap.constFind({ chatPeerId, rootId });
		if (i == ThreadReverseMap.constEnd()) {
			return false;
		}
		const auto channel = session->data().channelLoaded(
			peerToChannel(i.value()));
		return channel && !(channel->flags() & ChannelDataFlag::Left);
	};
	auto unread = false;
	for (const auto &block : history->blocks) {
		for (const auto &view : block->messages) {
			const auto item = view->data();
			const auto views = item->Get<HistoryMessageViews>();
			if (views
				&& views->commentsMaxId > views->commentsInboxReadTillId
				&& !subscribed(item->id)) {
				unread = true;
				break;
			}
		}
		if (unread) {
			break;
		}
	}
	if (!unread) {
		for (auto i = PendingThreadUnread.cbegin()
			; i != PendingThreadUnread.cend()
			; ++i) {
			if (i.key().first == chatPeerId
				&& i.value().count > 0
				&& !subscribed(i.key().second)) {
				unread = true;
				break;
			}
		}
	}
	history->mtsLinkSetThreadsMark(unread);
}

void resetThreadEntryUnread(
		not_null<Main::Session*> session,
		PeerId parentPeerId,
		MsgId rootId) {
	const auto history = threadEntryHistory(session, parentPeerId, rootId);
	if (history && history->unreadCount() > 0) {
		history->setUnreadCount(0);
		session->data().refreshChatListEntry(Dialogs::Key(history));
	}
}

void updateThreadParticipants(
		not_null<Main::Session*> session,
		PeerId parentPeerId,
		MsgId rootId,
		const std::vector<MsgId> &messageIds) {
	const auto it = ThreadReverseMap.constFind({ parentPeerId, rootId });
	if (it == ThreadReverseMap.constEnd()) {
		return;
	}
	const auto threadPeerId = it.value();
	const auto channel = session->data().channelLoaded(
		peerToChannel(threadPeerId));
	if (!channel) {
		return;
	}
	const auto mega = channel->asMegagroup();
	if (!mega || !mega->mgInfo) {
		return;
	}

	base::flat_set<not_null<UserData*>> seen;
	for (const auto &msgId : messageIds) {
		const auto item = session->data().message(parentPeerId, msgId);
		if (item) {
			if (const auto user = item->from()->asUser()) {
				seen.emplace(user);
			}
		}
	}

	mega->mgInfo->lastParticipants.clear();
	for (const auto &user : seen) {
		mega->mgInfo->lastParticipants.push_back(user);
	}
	mega->setMembersCount(int(seen.size()));
	mega->mgInfo->lastParticipantsStatus
		= MegagroupInfo::LastParticipantsUpToDate
		| MegagroupInfo::LastParticipantsOnceReceived;
	mega->mgInfo->lastParticipantsCount = int(seen.size());
	session->changes().peerUpdated(
		mega, Data::PeerUpdate::Flag::Members);
}

MTPPeerNotifySettings makeMuteSettings(bool muted) {
	using Flag = MTPDpeerNotifySettings::Flag;
	return MTP_peerNotifySettings(
		MTP_flags(Flag::f_mute_until),
		MTPBool(),
		MTPBool(),
		MTP_int(muted ? std::numeric_limits<int>::max() : 0),
		MTPNotificationSound(),
		MTPNotificationSound(),
		MTPNotificationSound(),
		MTPBool(),
		MTPBool(),
		MTPNotificationSound(),
		MTPNotificationSound(),
		MTPNotificationSound());
}

void applyDialogData(
		not_null<Main::Session*> session,
		const Api::ChannelData &src) {
	PeerId peerId;
	if (!src.interlocutorId.isEmpty()) {
		const auto bareId = uuidToBareId(src.interlocutorId);
		peerId = PeerId(::UserId(bareId));
		PeerToChatMap.insert(peerId, src.id);
		ChatToPeerMap.insert(src.id, peerId);
		PeerToChatTypeMap.insert(peerId, ChatType::Dialog);
		UserBareIdToUuidMap.insert(bareId, src.interlocutorId);
	} else {
		peerId = chatIdToPeerId(src.id, ChatType::Dialog);
	}
	const auto userId = peerToUser(peerId);
	const auto user = session->data().user(userId);

	// The dialog has only the full name, the member profile is better.
	if (user->name().isEmpty()) {
		user->setName(src.name, {}, {}, user->editableUsername());
	}
	user->setIsContact(true);
	if (!HasKnownUserpic(user)) {
		applyUserpic(user, src.avatarFileId, __LINE__);
	}

	const auto history = session->data().history(user->id);
	if (!history->folderKnown()) {
		history->clearFolder();
	}

	if (src.lastMessageTimestamp) {
		history->setChatListTimeId(
			TimeId(src.lastMessageTimestamp / 1000));
	} else if (!history->chatListTimeId()) {
		history->setChatListTimeId(TimeId(1));
	}
	if (src.unreadCount >= 0 && src.lastMessageTimestamp) {
		history->setUnreadCount(src.unreadCount);
	}
	session->data().notifySettings().apply(
		not_null<PeerData*>(user), makeMuteSettings(src.isMuted));

	if (src.type == ChatType::Favorites) {
		FavoritesPeerIdValue = peerId;
		session->data().setPinnedFromEntryList(
			Dialogs::Key(history), true);
		session->data().setChatPinned(history, FilterId(2), true);
	} else if (src.isPinned) {
		session->data().setPinnedFromEntryList(
			Dialogs::Key(history), true);
	}
}

[[nodiscard]] QString normalizeChannelRole(const QString &role) {
	return role.contains(u"Owner"_q)
		? u"Owner"_q
		: role.contains(u"Admin"_q)
		? u"Admin"_q
		: role.isEmpty()
		? QString()
		: u"Member"_q;
}

// Applies my role in the channel to the Telegram rights model.
void applyMyChannelRole(
		not_null<ChannelData*> channel,
		const QString &role) {
	const auto normalized = normalizeChannelRole(role);
	if (!normalized.isEmpty()) {
		MyChannelRoles.insert(channel->id, normalized);
	}
	const auto current = MyChannelRoles.value(channel->id);
	const auto owner = (current == u"Owner"_q);
	const auto admin = owner || (current == u"Admin"_q);

	auto rights = ChatAdminRights(0);
	if (chatTypeForPeer(channel->id) == ChatType::Channel
		&& (!channel->isBroadcast() || admin)) {
		rights |= ChatAdminRight::PostMessages;
	}
	if (admin) {
		rights |= ChatAdminRight::PostMessages
			| ChatAdminRight::EditMessages
			| ChatAdminRight::PinMessages
			| ChatAdminRight::DeleteMessages
			| ChatAdminRight::InviteByLinkOrAdd
			| ChatAdminRight::BanUsers
			| ChatAdminRight::ChangeInfo;
	}
	if (owner) {
		rights |= ChatAdminRight::AddAdmins;
	}
	channel->setAdminRights(rights);
	auto flags = channel->flags();
	if (owner) {
		flags |= ChannelDataFlag::Creator;
	} else {
		flags &= ~ChannelDataFlag::Creator;
	}
	channel->setFlags(flags);
}

void applyChannelData(
		not_null<Main::Session*> session,
		const Api::ChannelData &src) {
	const auto peerId = chatIdToPeerId(src.id, src.type);
	const auto channelId = peerToChannel(peerId);
	const auto channel = session->data().channel(channelId);
	if (src.memberRole.contains(u"None"_q)) {
		// A public channel I'm not in: preview with the join button.
		auto flags = channel->flags();
		flags |= ChannelDataFlag::Left;
		flags |= ChannelDataFlag::CanViewParticipants;
		if (src.isReadOnly) {
			flags |= ChannelDataFlag::Broadcast;
			flags &= ~ChannelDataFlag::Megagroup;
		} else {
			flags |= ChannelDataFlag::Megagroup;
			flags &= ~ChannelDataFlag::Broadcast;
		}
		channel->setFlags(flags);
		channel->setName(src.name, {});
		if (!src.description.isEmpty()) {
			channel->setAbout(src.description);
		}
		applyUserpic(channel, src.avatarFileId, __LINE__);
		if (src.memberCount > 0) {
			channel->setMembersCount(src.memberCount);
		}
		channel->setLoadedStatus(PeerData::LoadedStatus::Normal);
		MyChannelRoles.remove(channel->id);
		LOG(("MtsLink Channel: preview of %1").arg(src.id));
		return;
	}

	auto flags = channel->flags();
	flags &= ~ChannelDataFlag::Left;
	flags &= ~ChannelDataFlag::Forbidden;
	flags |= ChannelDataFlag::CanViewParticipants;

	if (src.isReadOnly) {
		flags |= ChannelDataFlag::Broadcast;
		flags &= ~ChannelDataFlag::Megagroup;
	} else {
		flags |= ChannelDataFlag::Megagroup;
		flags &= ~ChannelDataFlag::Broadcast;
	}
	channel->setFlags(flags);
	applyMyChannelRole(channel, src.memberRole);
	if (src.isPublicKnown) {
		setChannelPublic(channel->id, src.isPublic);
	}
	if (!src.description.isEmpty() || src.isPublicKnown) {
		channel->setAbout(src.description);
	}
	channel->setName(src.name, {});
	applyUserpic(channel, src.avatarFileId, __LINE__);
	if (src.memberCount > 0) {
		channel->setMembersCount(src.memberCount);
	}
	channel->setLoadedStatus(PeerData::LoadedStatus::Normal);
	channel->setAllowedReactions({
		.maxCount = 100,
		.type = Data::AllowedReactionsType::All,
	});

	const auto history = session->data().history(channel->id);
	if (!history->folderKnown()) {
		history->clearFolder();
	}

	if (src.lastMessageTimestamp) {
		history->setChatListTimeId(
			TimeId(src.lastMessageTimestamp / 1000));
	} else if (!history->chatListTimeId()) {
		history->setChatListTimeId(TimeId(1));
	}
	if (src.unreadCount >= 0 && src.lastMessageTimestamp) {
		history->setUnreadCount(src.unreadCount);
	}
	session->data().notifySettings().apply(
		channel, makeMuteSettings(src.isMuted));

	if (src.isPinned) {
		session->data().setPinnedFromEntryList(
			Dialogs::Key(history), true);
	}
	if (!history->inChatList() && history->folderKnown()) {
		// Added back after leaving or joined from a preview: the sort
		// position was computed while not a member, recompute it.
		history->updateChatListSortPosition();
		history->updateChatListExistence();
		LOG(("MtsLink Channel: %1 back to chat list=%2 amIn=%3 "
			"lastKnown=%4 last=%5 time=%6"
			).arg(src.id
			).arg(history->inChatList() ? 1 : 0
			).arg(channel->amIn() ? 1 : 0
			).arg(history->lastMessageKnown() ? 1 : 0
			).arg(history->lastMessage() ? 1 : 0
			).arg(history->chatListTimeId()));
	}
}

namespace {

QHash<PeerId, UserDetails> UserDetailsMap;
rpl::event_stream<PeerId> UserDetailsChanges;
std::vector<ProfileFieldInfo> ProfileFields;
bool ProfileFieldsRequested = false;
base::flat_set<PeerId> UserDetailsRequested;

// Profiles come from many answers, most of them without the details:
// a field is updated only by an answer that has it.
void applyUserDetails(PeerId peerId, const Api::MemberProfile &src) {
	auto &details = UserDetailsMap[peerId];
	const auto was = details;
	if (!src.email.isEmpty()) {
		details.email = src.email;
	}
	if (src.detailsKnown) {
		details.position = src.position;
		details.department = src.department;
		details.additional.clear();
		for (const auto &[id, value] : src.additionalFields) {
			details.additional.push_back({ id, value });
		}
		details.full = true;
	} else {
		if (!src.position.isEmpty()) {
			details.position = src.position;
		}
		if (!src.department.isEmpty()) {
			details.department = src.department;
		}
	}
	if (details.email != was.email
		|| details.position != was.position
		|| details.department != was.department
		|| details.additional != was.additional
		|| details.full != was.full) {
		UserDetailsChanges.fire_copy(peerId);
	}
}

void requestProfileFields(not_null<Main::Session*> session) {
	const auto mts = session->account().mtsLinkSession();
	// Requested again with the next profile shown, when connected.
	if (ProfileFieldsRequested
		|| !mts
		|| !mts->rpc()
		|| !mts->rpc()->isConnected()) {
		return;
	}
	ProfileFieldsRequested = true;
	const auto weak = base::make_weak(session);
	mts->rpc()->call(
		u"Organization.GetProfileFields"_q,
		QJsonObject{ { u"organizationId"_q, mts->organizationId() } },
		[=](const QJsonObject &result) {
			auto list = std::vector<std::pair<int, ProfileFieldInfo>>();
			const auto value = result.value(u"value"_q).toObject();
			for (const auto &group : value) {
				for (const auto &item : group.toArray()) {
					const auto field = item.toObject();
					const auto id = field.value(u"id"_q).toString();
					const auto title = field.value(u"title"_q).toString();
					if (!id.isEmpty() && !title.isEmpty()) {
						list.push_back({
							field.value(u"sort"_q).toInt(),
							ProfileFieldInfo{ id, title },
						});
					}
				}
			}
			ranges::stable_sort(list, ranges::less(), [](const auto &p) {
				return p.first;
			});
			ProfileFields.clear();
			for (const auto &[sort, field] : list) {
				ProfileFields.push_back(field);
			}
			LOG(("MtsLink Profile: %1 organization profile fields"
				).arg(ProfileFields.size()));
			for (const auto &peerId : UserDetailsMap.keys()) {
				UserDetailsChanges.fire_copy(peerId);
			}
		},
		[=](const QString &error) {
			ProfileFieldsRequested = false;
			LOG(("MtsLink Profile: GetProfileFields failed: %1").arg(error));
		});
}

} // namespace

rpl::producer<UserDetails> userDetailsValue(not_null<UserData*> user) {
	const auto peerId = user->id;
	return rpl::single(
		UserDetailsMap.value(peerId)
	) | rpl::then(UserDetailsChanges.events(
	) | rpl::filter(
		rpl::mappers::_1 == peerId
	) | rpl::map([=] {
		return UserDetailsMap.value(peerId);
	}));
}

const std::vector<ProfileFieldInfo> &profileFields() {
	return ProfileFields;
}

rpl::producer<QString> profileFieldTitle(const QString &title) {
	// The organization fields come with Russian titles.
	const auto lower = title.trimmed().toLower();
	if (lower == u"подразделение"_q) {
		return tr::lng_mtslink_info_department();
	} else if (lower == u"внутренний телефон"_q) {
		return tr::lng_mtslink_info_extension();
	} else if (lower == u"должность"_q) {
		return tr::lng_mtslink_info_position();
	}
	return rpl::single(title);
}

namespace {

[[nodiscard]] QString DefaultNotifyKey(Data::DefaultNotify type) {
	switch (type) {
	case Data::DefaultNotify::User: return u"notifyUsers"_q;
	case Data::DefaultNotify::Group: return u"notifyGroups"_q;
	case Data::DefaultNotify::Broadcast: return u"notifyChannels"_q;
	}
	return QString();
}

} // namespace

void saveDefaultNotify(
		Data::DefaultNotify type,
		const Data::PeerNotifySettings &value) {
	auto settings = readLocalSettings();
	auto object = QJsonObject();
	object.insert(u"muteUntil"_q, qint64(value.muteUntil().value_or(0)));
	if (const auto silent = value.silentPosts()) {
		object.insert(u"silent"_q, *silent);
	}
	settings.insert(DefaultNotifyKey(type), object);
	writeLocalSettings(settings);
	LOG(("MtsLink Notify: default %1 saved, mute until %2"
		).arg(DefaultNotifyKey(type)
		).arg(value.muteUntil().value_or(0)));
}

void restoreDefaultNotify(not_null<Main::Session*> session) {
	const auto settings = readLocalSettings();
	using Type = Data::DefaultNotify;
	for (const auto type : { Type::User, Type::Group, Type::Broadcast }) {
		const auto object = settings.value(DefaultNotifyKey(type)).toObject();
		if (object.isEmpty()) {
			continue;
		}
		using Flag = MTPDpeerNotifySettings::Flag;
		auto flags = Flag::f_mute_until | Flag();
		const auto silent = object.value(u"silent"_q);
		if (silent.isBool()) {
			flags |= Flag::f_silent;
		}
		session->data().notifySettings().apply(
			type,
			MTP_peerNotifySettings(
				MTP_flags(flags),
				MTPBool(),
				MTP_bool(silent.toBool()),
				MTP_int(object.value(u"muteUntil"_q).toInteger()),
				MTPNotificationSound(),
				MTPNotificationSound(),
				MTPNotificationSound(),
				MTPBool(),
				MTPBool(),
				MTPNotificationSound(),
				MTPNotificationSound(),
				MTPNotificationSound()));
	}
}

void requestUserDetails(not_null<UserData*> user) {
	// The profile shows the userpic large and opens it by a click: the
	// avatar file is checked to exist, else the user has no userpic.
	if (const auto fileId = UserpicFileIds.value(user->id)
		; !fileId.isEmpty()
		&& user->userpicPhotoId() == PhotoId(uuidToBareId(fileId))) {
		static auto checked = QSet<QString>();
		if (!checked.contains(fileId)) {
			checked.insert(fileId);
			static const auto manager = new QNetworkAccessManager();
			const auto url = avatarCdnBase() + fileId + u"_s.jpg"_q;
			const auto reply = manager->head(QNetworkRequest(QUrl(url)));
			const auto weak = base::make_weak(&user->session());
			const auto peerId = user->id;
			QObject::connect(reply, &QNetworkReply::finished, [=] {
				reply->deleteLater();
				const auto status = reply->attribute(
					QNetworkRequest::HttpStatusCodeAttribute).toInt();
				if (status != 404) {
					return;
				}
				LOG(("MtsLink Userpic: no avatar file %1, no userpic"
					).arg(fileId));
				MissingAvatarFiles.insert(fileId);
				if (const auto strong = weak.get()) {
					if (const auto peer = strong->data().peerLoaded(peerId)) {
						if (UserpicFileIds.value(peerId) == fileId) {
							ClearUserpic(peer);
						}
					}
				}
			});
		}
	}
	const auto session = &user->session();
	requestProfileFields(session);
	const auto mts = session->account().mtsLinkSession();
	const auto userId = UserBareIdToUuidMap.value(peerToUser(user->id).bare);
	if (!mts
		|| userId.isEmpty()
		|| UserDetailsMap.value(user->id).full
		|| UserDetailsRequested.contains(user->id)) {
		if (userId.isEmpty()) {
			LOG(("MtsLink Profile: no MTS Link id of the peer %1 (%2)"
				).arg(user->id.value
				).arg(user->name()));
		}
		return;
	}
	UserDetailsRequested.emplace(user->id);
	LOG(("MtsLink Profile: loading the details of %1").arg(userId));
	mts->users()->loadMember(userId, mts->organizationId());
}

void applyUserData(
		not_null<Main::Session*> session,
		const Api::MemberProfile &src,
		bool profileChanged) {
	if (src.customStatusKnown) {
		applyUserStatus(session, src.userId, src.customStatus);
	}
	const auto bareId = uuidToBareId(src.userId);
	UserBareIdToUuidMap.insert(bareId, src.userId);
	const auto user = session->data().user(::UserId(bareId));
	const auto isSelf = (user == session->user());

	const auto first = src.firstName;
	const auto last = src.lastName;
	const auto display = src.displayName;
	static bool selfLogged = false;
	if (isSelf && !selfLogged) {
		selfLogged = true;
		LOG(("MtsLink: applying SELF user data: '%1 %2' display='%3' avatar='%4'")
			.arg(first, last, display, src.avatarFileId));
	}
	// Name and userpic changes are notified by their setters, the rest
	// only when changed: an update of all the members on opening a chat
	// made all the userpics blink.
	const auto wasUsername = user->username();
	const auto wasPhone = user->phone();
	applyUserDetails(user->id, src);
	// The bio is not used: the details are shown in their own rows, an old
	// "email / position" bio may be stored locally (for the self user).
	if (!user->about().isEmpty()) {
		user->setAbout(QString());
	}
	// Some answers have no display name, it is kept then: the name and the
	// username jumped between the answers in an opened profile.
	user->setName(
		first.isEmpty() ? display : first,
		last,
		{},
		display.isEmpty() ? user->editableUsername() : display);
	user->setLoadedStatus(PeerData::LoadedStatus::Normal);
	user->removeFlags(UserDataFlag::Scam | UserDataFlag::Fake);
	user->setIsContact(true);
	// The server gives different avatars of the same user in different
	// answers (an old one in some member lists), so a loaded profile only
	// sets a missing userpic, a change comes with MemberProfileChanged.
	if (profileChanged || !HasKnownUserpic(user)) {
		applyUserpic(user, src.avatarFileId, __LINE__);
	} else {
		ensureUserpicFor(user, src.avatarFileId);
	}


	if (!src.phone.isEmpty()) {
		auto phone = src.phone;
		if (phone.startsWith('+')) {
			phone = phone.mid(1);
		}
		user->setPhone(phone);
	}


	auto onlineChanged = false;
	if (src.presence != MemberPresence::Unknown) {
		const auto status = (src.presence == MemberPresence::Online)
			? Data::LastseenStatus::OnlineTill(
				base::unixtime::now() + kMtsLinkOnlineHorizon)
			: Data::LastseenStatus::Recently();
		PresenceKnownUsers.insert(user->id);
		onlineChanged = user->updateLastseen(status);
	}
	if (src.inCall >= 0) {
		if (src.inCall) {
			InCallUsers.insert(user->id);
		} else {
			InCallUsers.remove(user->id);
		}
	}

	auto flags = Data::PeerUpdate::Flags();
	if (user->username() != wasUsername) {
		flags |= Data::PeerUpdate::Flag::Username;
	}
	if (onlineChanged) {
		flags |= Data::PeerUpdate::Flag::OnlineStatus;
	}
	if (user->phone() != wasPhone) {
		flags |= Data::PeerUpdate::Flag::PhoneNumber;
	}
	if (flags) {
		session->changes().peerUpdated(user, flags);
	}
}

void applyThreadChildrenCount(
		not_null<HistoryItem*> item,
		PeerId chatPeerId,
		MsgId msgId,
		const Api::MessageData &src) {
	if (src.threadChildrenCount <= 0) {
		return;
	}
	const auto views = item->Get<HistoryMessageViews>();
	auto unread = views
		? std::max(int(views->commentsMaxId.bare
			- views->commentsInboxReadTillId.bare), 0)
		: 0;
	const auto key = qMakePair(chatPeerId, msgId);
	const auto pendingIt = PendingThreadUnread.find(key);
	if (pendingIt != PendingThreadUnread.end()) {
		unread = std::max(unread, pendingIt.value().count);
		PendingThreadUnread.erase(pendingIt);
	}
	auto repliesData = HistoryMessageRepliesData();
	repliesData.isNull = false;
	repliesData.repliesCount = src.threadChildrenCount;
	repliesData.maxId = MsgId(src.threadChildrenCount);
	repliesData.readMaxId = MsgId(
		std::max(src.threadChildrenCount - unread, 1));
	item->setReplies(std::move(repliesData));
}

namespace {

// The own messages not read by the others: by the "isRead" of the server,
// LastReadMessageUpdatedEvent (the others have read till a message) and a
// new message of the others (the messages before it are read), as MTS Link
// shows them; with the parent message of their thread (empty in the chat).
QHash<PeerId, base::flat_map<MsgId, QString>> OutboxUnread;

void repaintOutbox(
		not_null<Main::Session*> session,
		PeerId peerId,
		MsgId msgId) {
	const auto item = session->data().message(peerId, msgId);
	if (!item) {
		return;
	}
	session->data().requestItemRepaint(item);
	session->changes().messageUpdated(
		item,
		Data::MessageUpdate::Flag::DialogRowRepaint);
	session->changes().historyUpdated(
		item->history(),
		Data::HistoryUpdate::Flag::OutboxRead);
}

void setOutboxUnread(
		not_null<Main::Session*> session,
		PeerId peerId,
		MsgId msgId,
		const QString &parentId,
		bool unread) {
	auto &list = OutboxUnread[peerId];
	const auto changed = unread
		? list.emplace(msgId, parentId).second
		: (list.remove(msgId) > 0);
	if (changed) {
		repaintOutbox(session, peerId, msgId);
	}
}

// The own messages of the chat (or of the thread) till the date are read.
void markOutboxReadTill(
		not_null<Main::Session*> session,
		PeerId peerId,
		const QString &parentId,
		TimeId date) {
	const auto i = OutboxUnread.find(peerId);
	if (i == OutboxUnread.end()) {
		return;
	}
	auto read = std::vector<MsgId>();
	for (const auto &[msgId, parent] : i.value()) {
		if (parent != parentId) {
			continue;
		}
		const auto item = session->data().message(peerId, msgId);
		if (!item || item->date() <= date) {
			read.push_back(msgId);
		}
	}
	for (const auto msgId : read) {
		i.value().remove(msgId);
		repaintOutbox(session, peerId, msgId);
	}
}

} // namespace

bool isOutboxUnread(PeerId peerId, MsgId msgId) {
	if (peerId == favoritesPeerId()) {
		// The server gives isRead=false for all the own messages there:
		// nobody else reads them, read as in Telegram.
		return false;
	}
	const auto i = OutboxUnread.constFind(peerId);
	return (i != OutboxUnread.cend()) && i->contains(msgId);
}

[[nodiscard]] bool mentionsUser(
		const Api::MessageData &src,
		const QString &userId) {
	if (userId.isEmpty()) {
		return false;
	}
	for (const auto &mention : src.mentions) {
		if (mention.userId == userId) {
			return true;
		}
	}
	const auto tag = u"<@u:"_q + userId + '>';
	return src.markdown.contains(tag) || src.text.contains(tag);
}

HistoryItem *addMessage(
		not_null<Main::Session*> session,
		const Api::MessageData &src,
		bool threadOnly,
		std::vector<not_null<HistoryItem*>> *batchItems) {
	if (src.isDeleted) {
		return nullptr;
	}
	if (src.type == MessageType::Call && !src.callMeta) {
		return nullptr;
	}
	const auto chatPeerId = chatIdToPeerId(src.chatId);

	const auto history = session->data().history(chatPeerId);
	if (!history->folderKnown()) {
		history->clearFolder();
	}

	const auto msgBareId = uuidToBareId(src.id);
	const auto msgId = MsgId(msgBareId & 0x7FFFFFFFLL);

	const auto fromBareId = uuidToBareId(src.authorId);
	const auto fromPeerId = PeerId(::UserId(fromBareId));
	const auto date = TimeId(src.createdAt / 1000);

	if (src.isRead) {
		const auto mts = session->account().mtsLinkSession();
		if (mts && src.authorId == mts->userId()) {
			setOutboxUnread(
				session,
				chatPeerId,
				msgId,
				src.parentId,
				!*src.isRead);
		}
	}

	auto &members = ChatMembersMap[chatPeerId];
	if (std::find(members.begin(), members.end(), fromBareId) == members.end()) {
		members.push_back(fromBareId);
	}

	auto flags = MessageFlags(MessageFlag::HasFromId);
	const auto mts = session->account().mtsLinkSession();
	const auto myUserId = (mts && !mts->userId().isEmpty())
		? mts->userId()
		: CachedMyUserId;
	if (!myUserId.isEmpty() && src.authorId == myUserId) {
		flags |= MessageFlag::Outgoing;
	} else if (mentionsUser(src, myUserId)) {
		// Notified even in a muted chat or thread (as in Telegram).
		flags |= MessageFlag::MentionsMe;
	}

	const auto user = session->data().user(peerToUser(fromPeerId));
	if (user->name().isEmpty() && mts && !src.authorId.isEmpty()
		&& !UserProfileRequested.contains(src.authorId)) {
		UserProfileRequested.insert(src.authorId);
		mts->users()->loadMember(src.authorId, mts->organizationId());
	}

	auto fields = HistoryItemCommonFields{
		.id = msgId,
		.flags = flags,
		.from = fromPeerId,
		.date = date,
	};

	if (!src.repliedMessageId.isEmpty()) {
		const auto it = MtsLinkIdToMsgMap.constFind(src.repliedMessageId);
		const auto replyMsgId = (it != MtsLinkIdToMsgMap.constEnd())
			? it.value().second
			: MsgId(uuidToBareId(src.repliedMessageId) & 0x7FFFFFFFLL);
		fields.replyTo = FullReplyTo{
			.messageId = FullMsgId(chatPeerId, replyMsgId),
		};
		fields.flags |= MessageFlag::HasReplyInfo;
	}
	if (!src.parentId.isEmpty()) {
		const auto parentMsgId = localMsgId(src.parentId);
		if (fields.replyTo.messageId.msg == 0) {
			fields.replyTo.messageId = FullMsgId(chatPeerId, parentMsgId);
		}
		fields.replyTo.topicRootId = parentMsgId;
		fields.flags |= MessageFlag::HasReplyInfo;
		registerThreadRoot(chatPeerId, msgId, parentMsgId);
	}
	if (src.forward) {
		const auto &fwd = *src.forward;
		fields.forwardDate = TimeId(fwd.createdAt / 1000);
		if (!fwd.authorId.isEmpty()) {
			const auto fwdBareId = uuidToBareId(fwd.authorId);
			fields.forwardFrom = PeerId(::UserId(fwdBareId));
			if (const auto fwdUser = session->data().userLoaded(
					::UserId(fwdBareId))) {
				fields.forwardSenderName = fwdUser->name();
			}
		}
		if (fields.forwardDate == 0) {
			fields.forwardDate = date;
		}
		if (!fwd.chatId.isEmpty()) {
			fields.forwardOriginalPeer = chatIdToPeerId(fwd.chatId);
		}
		if (!fwd.messageId.isEmpty()) {
			const auto it = MtsLinkIdToMsgMap.constFind(fwd.messageId);
			fields.forwardOriginalMsgId = (it != MtsLinkIdToMsgMap.constEnd())
				? it.value().second
				: MsgId(uuidToBareId(fwd.messageId) & 0x7FFFFFFFLL);
		}
	}

	auto text = parseMentionedText(src.text, src.markdown, src.mentions, session);

	const auto uuidIt = MtsLinkIdToMsgMap.constFind(src.id);
	if (uuidIt != MtsLinkIdToMsgMap.constEnd()
		&& uuidIt.value().second != msgId) {
		const auto existing = session->data().message(
			uuidIt.value().first, uuidIt.value().second);
		if (existing) {
			return existing;
		}
	}

	// Diagnostics: the ids are hashes of the uuids, two messages with the
	// same hash would show only the first one.
	if (const auto known = msgIdToMtsLinkId(chatPeerId, msgId)
		; !known.isEmpty() && known != src.id) {
		LOG(("MtsLink NewMsg: ID COLLISION in %1, msgId=%2 for %3 and %4"
			).arg(src.chatId
			).arg(msgId.bare
			).arg(known
			).arg(src.id));
	}
	registerMessageId(chatPeerId, msgId, src.id);

	const auto existing = session->data().message(chatPeerId, msgId);
	if (existing) {
		if (!src.mentions.isEmpty()) {
			existing->setText(text);
			session->data().requestItemTextRefresh(existing);
			existing->invalidateChatListEntry();
		}
		if (!src.parentId.isEmpty()) {
			const auto parentMsgId = localMsgId(src.parentId);
			existing->ensureReplyComponent();
			existing->setReplyFields(
				existing->replyToTop() ? existing->replyToTop() : parentMsgId,
				parentMsgId,
				false);
			registerThreadRoot(chatPeerId, msgId, parentMsgId);
		}
		if (const auto reply = existing->Get<HistoryMessageReply>()) {
			if (!reply->resolvedMessage) {
				existing->updateDependencyItem();
			}
		}
		if (!src.reactions.isEmpty()) {
			const auto mtp = buildMtpReactions(src.reactions);
			if (mtp) {
				existing->updateReactions(&*mtp);
			}
		} else if (!existing->reactions().empty()) {
			// All the reactions removed (missed the event).
			existing->updateReactions(nullptr);
		}
		if (src.type == MessageType::Text && !existing->isSending()) {
			// Edited (missed the event): the text of the server.
			auto text = parseMentionedText(
				src.text,
				src.markdown,
				src.mentions,
				session);
			if (existing->originalText() != text) {
				existing->setText(std::move(text));
				session->data().requestItemTextRefresh(existing);
			}
		}
		applyThreadChildrenCount(existing, chatPeerId, msgId, src);
		if (src.type == MessageType::Call && src.callMeta) {
			applyCallItem(session, existing, *src.callMeta);
		}
		if (!existing->mainView() && !threadOnly) {
			if (batchItems) {
				batchItems->push_back(existing);
			} else {
				history->reattachToBlock(existing);
			}
		}
		return existing;
	}

	if (src.type == MessageType::Call && src.callMeta) {
		const auto item = (threadOnly || batchItems)
			? history->makeMessage(
				std::move(fields),
				TextWithEntities(),
				MTP_messageMediaEmpty())
			: history->addNewExternalMessage(
				std::move(fields),
				TextWithEntities(),
				MTP_messageMediaEmpty());
		if (item && batchItems) {
			batchItems->push_back(item);
		}
		if (item) {
			applyCallItem(session, item, *src.callMeta);
			applyThreadChildrenCount(item, chatPeerId, msgId, src);
			CallServerChildren.insert(item->fullId(), src.threadChildrenCount);
			updateCallReplies(item);
		}
		if (item && threadOnly) {
			session->changes().messageUpdated(
				item,
				Data::MessageUpdate::Flag::NewMaybeAdded);
		}
		return item;
	}

	const auto multiFile = (src.files.size() > 1);
	if (multiFile) {
		fields.groupedId = msgBareId;
	}

	const auto media = (!src.files.isEmpty())
		? buildFileMedia(session, src.files.first(), date)
		: MTP_messageMediaEmpty();

	const auto item = (threadOnly || batchItems)
		? history->makeMessage(
			std::move(fields),
			std::move(text),
			media)
		: history->addNewExternalMessage(
			std::move(fields),
			std::move(text),
			media);
	if (item && batchItems) {
		batchItems->push_back(item);
	}
	if (item && !src.files.isEmpty()) {
		reapplyPhotoUrls(item, src.files.first());
	}
	if (item && multiFile) {
		for (int fi = 1; fi < src.files.size(); ++fi) {
			const auto extraId = MsgId(
				(uuidToBareId(src.id + QString::number(fi))
					& 0x7FFFFFFFLL));
			auto extraFields = HistoryItemCommonFields{
				.id = extraId,
				.flags = flags,
				.from = fromPeerId,
				.date = date,
				.groupedId = msgBareId,
			};
			const auto extraMedia = buildFileMedia(
				session, src.files[fi], date);
			const auto extra = (threadOnly || batchItems)
				? history->makeMessage(
					std::move(extraFields),
					TextWithEntities(),
					extraMedia)
				: history->addNewExternalMessage(
					std::move(extraFields),
					TextWithEntities(),
					extraMedia);
			if (extra) {
				reapplyPhotoUrls(extra, src.files[fi]);
				if (batchItems) {
					batchItems->push_back(extra);
				}
			}
		}
	}
	if (item) {
		applyThreadChildrenCount(item, chatPeerId, msgId, src);
	}
	if (item && src.updatedAt > 0 && src.updatedAt != src.createdAt) {
		item->setEditDate(TimeId(src.updatedAt / 1000));
	}
	if (item && !src.reactions.isEmpty()) {
		const auto mtp = buildMtpReactions(src.reactions);
		if (mtp) {
			item->updateReactions(&*mtp);
		}
	}
	if (item && threadOnly) {
		session->changes().messageUpdated(
			item,
			Data::MessageUpdate::Flag::NewMaybeAdded);
	}
	return item;
}

bool addOlderMessages(
		not_null<Main::Session*> session,
		const ChatId &chatId,
		const QList<Api::MessageData> &messages,
		const QList<Api::MemberProfile> &profiles,
		const MessageId &rawLastId,
		int rawCount) {
	for (const auto &p : profiles) {
		applyUserData(session, p);
	}

	const auto chatPeerId = chatIdToPeerId(chatId);
	const auto history = session->data().history(chatPeerId);

	if (rawCount == 0) {
		history->markLoadedAtTop();
		return true;
	}

	if (!rawLastId.isEmpty()) {
		setOldestLoadedMessageId(chatPeerId, rawLastId);
	}

	std::vector<not_null<HistoryItem*>> items;
	items.reserve(messages.size());
	int duplicates = 0;

	for (const auto &src : messages) {
		if (src.isDeleted) {
			continue;
		}
		if (src.type == MessageType::Call && !src.callMeta) {
			continue;
		}
		const auto msgBareId = uuidToBareId(src.id);
		const auto msgId = MsgId(msgBareId & 0x7FFFFFFFLL);

		const auto existing = session->data().message(chatPeerId, msgId);
		if (existing) {
			++duplicates;
			continue;
		}

		const auto fromBareId = uuidToBareId(src.authorId);
		const auto fromPeerId = PeerId(::UserId(fromBareId));
		const auto date = TimeId(src.createdAt / 1000);

		auto &members = ChatMembersMap[chatPeerId];
		if (std::find(members.begin(), members.end(), fromBareId) == members.end()) {
			members.push_back(fromBareId);
		}

		auto flags = MessageFlags(MessageFlag::HasFromId);
		const auto mts = session->account().mtsLinkSession();
		const auto myId = (mts && !mts->userId().isEmpty())
			? mts->userId()
			: CachedMyUserId;
		if (!myId.isEmpty() && src.authorId == myId) {
			flags |= MessageFlag::Outgoing;
		}
		auto fields = HistoryItemCommonFields{
			.id = msgId,
			.flags = flags,
			.from = fromPeerId,
			.date = date,
		};

		if (!src.repliedMessageId.isEmpty()) {
			const auto it = MtsLinkIdToMsgMap.constFind(
				src.repliedMessageId);
			const auto replyMsgId = (it != MtsLinkIdToMsgMap.constEnd())
				? it.value().second
				: MsgId(uuidToBareId(src.repliedMessageId) & 0x7FFFFFFFLL);
			fields.replyTo = FullReplyTo{
				.messageId = FullMsgId(chatPeerId, replyMsgId),
			};
			fields.flags |= MessageFlag::HasReplyInfo;
		}
		if (src.forward) {
			const auto &fwd = *src.forward;
			fields.forwardDate = TimeId(fwd.createdAt / 1000);
			if (!fwd.authorId.isEmpty()) {
				const auto fwdBareId = uuidToBareId(fwd.authorId);
				fields.forwardFrom = PeerId(::UserId(fwdBareId));
				if (const auto fwdUser = session->data().userLoaded(
						::UserId(fwdBareId))) {
					fields.forwardSenderName = fwdUser->name();
				}
			}
			if (fields.forwardDate == 0) {
				fields.forwardDate = date;
			}
			if (!fwd.chatId.isEmpty()) {
				fields.forwardOriginalPeer = chatIdToPeerId(fwd.chatId);
			}
			if (!fwd.messageId.isEmpty()) {
				const auto it = MtsLinkIdToMsgMap.constFind(fwd.messageId);
				fields.forwardOriginalMsgId =
					(it != MtsLinkIdToMsgMap.constEnd())
					? it.value().second
					: MsgId(uuidToBareId(fwd.messageId) & 0x7FFFFFFFLL);
			}
		}

		auto text = parseMentionedText(src.text, src.markdown, src.mentions, session);

		registerMessageId(chatPeerId, msgId, src.id);

		if (src.type == MessageType::Call && src.callMeta) {
			const auto callItem = history->makeMessage(
				std::move(fields),
				TextWithEntities(),
				MTP_messageMediaEmpty());
			applyCallItem(session, callItem, *src.callMeta);
			applyThreadChildrenCount(callItem, chatPeerId, msgId, src);
			CallServerChildren.insert(
				callItem->fullId(),
				src.threadChildrenCount);
			updateCallReplies(callItem);
			items.push_back(callItem);
			continue;
		}

		const auto multiFile = (src.files.size() > 1);
		if (multiFile) {
			fields.groupedId = msgBareId;
		}

		const auto media = (!src.files.isEmpty())
			? buildFileMedia(session, src.files.first(), date)
			: MTP_messageMediaEmpty();

		const auto item = history->makeMessage(
			std::move(fields),
			text,
			media);
		if (!src.files.isEmpty()) {
			reapplyPhotoUrls(item, src.files.first());
		}
		items.push_back(item);

		if (item && multiFile) {
			for (int fi = 1; fi < src.files.size(); ++fi) {
				const auto extraId = MsgId(
					(uuidToBareId(src.id + QString::number(fi))
						& 0x7FFFFFFFLL));
				auto extraFields = HistoryItemCommonFields{
					.id = extraId,
					.flags = flags,
					.from = fromPeerId,
					.date = date,
					.groupedId = msgBareId,
				};
				const auto extraMedia = buildFileMedia(
					session, src.files[fi], date);
				const auto extra = history->makeMessage(
					std::move(extraFields),
					TextWithEntities(),
					extraMedia);
				if (extra) {
					reapplyPhotoUrls(extra, src.files[fi]);
					items.push_back(extra);
				}
			}
		}
		if (item && src.threadChildrenCount > 0) {
			auto repliesData = HistoryMessageRepliesData();
			repliesData.isNull = false;
			repliesData.repliesCount = src.threadChildrenCount;
			repliesData.maxId = MsgId(src.threadChildrenCount);
			repliesData.readMaxId = MsgId(src.threadChildrenCount);
			item->setReplies(std::move(repliesData));
		}
		if (item && src.updatedAt > 0 && src.updatedAt != src.createdAt) {
			item->setEditDate(TimeId(src.updatedAt / 1000));
		}
		if (item && !src.reactions.isEmpty()) {
			const auto mtp = buildMtpReactions(src.reactions);
			if (mtp) {
				item->updateReactions(&*mtp);
			}
		}
	}

	// Set active call state from the newest Call message only.
	// messages[0] is the newest (server/cache sends newest-first).
	for (const auto &cm : messages) {
		if (cm.type == MessageType::Call && cm.callMeta) {
			if (cm.callMeta->status == "Started"
				&& !cm.callMeta->joinLink.isEmpty()) {
				setActiveCall(session, chatPeerId, *cm.callMeta);
			} else {
				setActiveCall(session, chatPeerId, QString());
			}
			break;
		}
	}

	if (items.empty()) {
		return (duplicates > 0);
	}

	std::reverse(items.begin(), items.end());
	history->addCreatedOlderSlice(items);
	const auto currentLast = history->lastMessage();
	if (!currentLast
		|| items.back()->date() >= currentLast->date()) {
		history->applyDialogTopMessage(items.back()->id);
	}

	for (const auto &item : items) {
		item->updateDependencyItem();
	}

	session->data().notifyHistoryChangeDelayed(history);
	session->data().sendHistoryChangeNotifications();
	refreshThreadsMark(session, chatPeerId);

	return true;
}

void handleNotificationEvent(
		not_null<Main::Session*> session,
		const QJsonObject &param) {
	const auto type = param.value("type").toString();
	const auto value = param.value("value").toObject();

	if (type == "MessageReadEvent") {
		const auto lastReads = value.value("lastReads").toArray();
		for (const auto &r : lastReads) {
			const auto obj = r.toObject();
			const auto chatId = obj.value("chatId").toString();
			const auto lastReadMsgId = obj.value("lastReadMessageId").toString();
			const auto threadId = obj.value("threadId").toString();
			if (chatId.isEmpty() || lastReadMsgId.isEmpty()) {
				continue;
			}
			const auto peerId = chatIdToPeerId(chatId);
			if (!hasChatId(peerId)) {
				continue;
			}
			markReadRequestSent(chatId);
			const auto readBareId = uuidToBareId(lastReadMsgId);
			const auto readMsgId = MsgId(readBareId & 0x7FFFFFFFLL);
			if (!threadId.isEmpty()) {
				const auto rootMsgId = localMsgId(threadId);
				if (auto cached = cachedRepliesList(peerId, rootMsgId)) {
					cached->setInboxReadTill(readMsgId, std::nullopt);
				}
			} else {
				const auto history = session->data().historyLoaded(peerId);
				if (history) {
					history->setInboxReadTill(readMsgId);
					if (const auto item = session->data().message(peerId, readMsgId)) {
						history->setMtsLinkInboxReadDate(item->date());
					}
				}
			}
		}
	}
}

void showMtsLinkToast(
		not_null<Main::Session*> session,
		const QString &text) {
	LOG(("MtsLink Toast: '%1' windows=%2"
		).arg(text).arg(session->windows().size()));
	if (!session->windows().empty()) {
		session->windows().front()->showToast(text, crl::time(5000));
	}
}

// Members events come in bursts and twice (chat and chat-user streams).
void scheduleChannelMembersReload(
		not_null<Main::Session*> session,
		const QString &chatId) {
	if (MembersReloadScheduled.contains(chatId)) {
		return;
	}
	MembersReloadScheduled.insert(chatId);
	const auto weak = base::make_weak(session);
	base::call_delayed(300, [=] {
		MembersReloadScheduled.remove(chatId);
		if (!weak) {
			return;
		}
		if (const auto mts = weak->account().mtsLinkSession()) {
			mts->users()->loadChatMembers(chatId);
		}
	});
}

void applyChannelLeft(
		not_null<Main::Session*> session,
		not_null<ChannelData*> channel) {
	MyChannelRoles.remove(channel->id);
	ChannelOwners.remove(channel->id);
	ChatMembersRoles.remove(channel->id);
	channel->setFlags(channel->flags() | ChannelDataFlag::Left);
	// A private channel can't be joined back, only by an invitation,
	// so the chat is closed and removed from the lists, as in Telegram.
	Core::App().closeChatFromWindows(channel);
	if (const auto history = session->data().historyLoaded(channel)) {
		history->setUnreadCount(0);
		if (history->folderKnown()) {
			const auto key = Dialogs::Key(history);
			if (history->isPinnedDialog(FilterId())) {
				session->data().setChatPinned(key, FilterId(), false);
			}
			history->updateChatListExistence();
			LOG(("MtsLink Channel: left %1, inChatList=%2"
				).arg(channel->name()
				).arg(history->inChatList() ? 1 : 0));
		}
	}
}

// GetMyChannelsV3 returns all my channels, the ones restored from the cache
// and missing there were left or deleted while the app was closed.
void removeStaleChannels(
		not_null<Main::Session*> session,
		const QList<Api::ChannelData> &channels,
		ChatType type) {
	auto actual = QSet<QString>();
	for (const auto &ch : channels) {
		actual.insert(ch.id);
	}
	auto stale = std::vector<not_null<ChannelData*>>();
	for (auto i = ChatToPeerMap.cbegin(); i != ChatToPeerMap.cend(); ++i) {
		const auto peerId = i.value();
		if (actual.contains(i.key())
			|| isThreadPeer(peerId)
			|| PeerToChatTypeMap.value(peerId) != type) {
			continue;
		}
		const auto channel = session->data().channelLoaded(
			peerToChannel(peerId));
		if (channel && channel->amIn()) {
			stale.push_back(channel);
		}
	}
	for (const auto &channel : stale) {
		LOG(("MtsLink Channel: %1 is not in my channels anymore"
			).arg(channel->name()));
		applyChannelLeft(session, channel);
	}
}

[[nodiscard]] QStringList eventUsers(const QJsonObject &value) {
	auto result = QStringList();
	for (const auto &user : value.value("users").toArray()) {
		if (user.isString()) {
			result.push_back(user.toString());
		} else {
			result.push_back(user.toObject().value("userId").toString());
		}
	}
	return result;
}

void handleChatEvent(
		not_null<Main::Session*> session,
		const QString &dst,
		const QJsonObject &param) {
	const auto type = param.value("type").toString();
	const auto value = param.value("value").toObject();
	const auto isUserLevel = dst.startsWith(u"chat-user-"_q)
		|| dst.startsWith(u"chat-org-"_q);
	const auto chatId = [&] {
		auto result = isUserLevel
			? value.value("chatId").toString()
			: extractChatIdFromDst(dst);
		if (result.isEmpty()) {
			result = value.value("updated").toObject()
				.value("chatId").toString();
		}
		// Channel events keep the id in other fields.
		if (result.isEmpty()) {
			result = value.value("channel").toObject()
				.value("chatId").toString();
		}
		if (result.isEmpty()) {
			result = value.value("chat").toObject()
				.value("chatId").toString();
		}
		if (result.isEmpty()) {
			result = value.value("channelId").toString();
		}
		if (result.isEmpty()) {
			result = value.value("groupChat").toObject()
				.value("chatId").toString();
		}
		return result;
	}();
	LOG(("MtsLink Event: type=%1 chatId=%2 dst=%3")
		.arg(type).arg(chatId).arg(dst));

	if (chatId.isEmpty()) {
		return;
	}

	const auto chatKnown = ChatToPeerMap.contains(chatId);
	if (!chatKnown && type == "NewMessageV2Event") {
		PendingChatEvents[chatId].append({ dst, param });
		LOG(("MtsLink NewMsg: chat %1 is unknown, waiting for its info, "
			"%2 pending").arg(chatId).arg(PendingChatEvents[chatId].size()));
		if (!ChatInfoRequested.contains(chatId)) {
			ChatInfoRequested.insert(chatId);
			const auto mts = session->account().mtsLinkSession();
			if (mts) {
				LOG(("MtsLink: unknown chatId %1, requesting info").arg(chatId));
				mts->channels()->loadChatInfo(chatId);
			}
			// Chat.GetChatV3 has no failure handling: the request is
			// repeated once, the lost messages are logged.
			const auto weak = base::make_weak(session);
			base::call_delayed(10000, [=] {
				const auto strong = weak.get();
				if (!strong || !PendingChatEvents.contains(chatId)) {
					return;
				}
				LOG(("MtsLink NewMsg: chat %1 info is not loaded in 10s, "
					"%2 messages pending, requesting again"
					).arg(chatId
					).arg(PendingChatEvents.value(chatId).size()));
				ChatInfoRequested.remove(chatId);
				if (const auto mts = strong->account().mtsLinkSession()) {
					ChatInfoRequested.insert(chatId);
					mts->channels()->loadChatInfo(chatId);
				}
			});
		}
		return;
	}

	if (type == "NewMessageV2Event") {
		const auto m = value.value("message").toObject();
		if (m.isEmpty()) {
			return;
		}
		if (m.value("isDeleted").toBool()) {
			return;
		}
		// The event has the author profile, the message doesn't wait for
		// Organization.GetMemberV2 then.
		for (const auto &profile : value.value("memberProfiles").toArray()) {
			const auto parsed = Api::ParseMemberProfile(profile.toObject());
			if (!parsed.userId.isEmpty()) {
				applyUserData(session, parsed);
			}
		}
		const auto authorId = m.value("authorId").toString();
		if (!authorId.isEmpty() && !AuthorWaitExpired.contains(authorId)) {
			const auto authorBareId = uuidToBareId(authorId);
			const auto authorPeerId = PeerId(::UserId(authorBareId));
			const auto user = session->data().userLoaded(
				peerToUser(authorPeerId));
			if (!user || user->name().isEmpty()) {
				PendingUserEvents[authorId].append({ dst, param });
				LOG(("MtsLink NewMsg: author %1 is unknown, the message "
					"waits for the profile, %2 pending"
					).arg(authorId
					).arg(PendingUserEvents[authorId].size()));
				if (!UserProfileRequested.contains(authorId)) {
					UserProfileRequested.insert(authorId);
					const auto mts = session->account().mtsLinkSession();
					if (mts) {
						mts->users()->loadMember(
							authorId, mts->organizationId());
					}
					// The profile may never come (no failure handling,
					// bots, guests): the messages are shown anyway.
					const auto weak = base::make_weak(session);
					base::call_delayed(5000, [=] {
						const auto strong = weak.get();
						if (!strong) {
							return;
						}
						UserProfileRequested.remove(authorId);
						const auto pending = PendingUserEvents.take(authorId);
						if (pending.isEmpty()) {
							return;
						}
						LOG(("MtsLink NewMsg: no profile of %1 in 5s, "
							"showing %2 messages"
							).arg(authorId).arg(pending.size()));
						AuthorWaitExpired.insert(authorId);
						for (const auto &ev : pending) {
							handleChatEvent(strong, ev.dst, ev.param);
						}
						AuthorWaitExpired.remove(authorId);
					});
				}
				return;
			}
		}
		Api::MessageData msg;
		msg.id = m.value("id").toString();
		msg.chatId = chatId;
		msg.authorId = m.value("authorId").toString();
		msg.text = m.value("text").toString();
		msg.markdown = m.value("markdown").toString();
		if (msg.markdown.isEmpty()) {
			const auto blocksArr = m.value("blocks").toArray();
			if (!blocksArr.isEmpty()) {
				msg.markdown = markdownFromBlocks(blocksArr);
			}
		}
		msg.createdAt = parseTimestamp(m, "createdAtMs", "createdAt");
		msg.updatedAt = parseTimestamp(m, "updatedAtMs", "updatedAt");
		if (m.contains("isRead")) {
			msg.isRead = m.value("isRead").toBool();
		}
		const auto repliedMsg = m.value("repliedMessage").toObject();
		msg.repliedMessageId = repliedMsg.value("id").toString();
		msg.parentId = value.value("threadId").toString();
		if (msg.parentId.isEmpty()) {
			msg.parentId = m.value("parentMessage").toObject().value("id").toString();
		}
		msg.type = [&] {
			const auto t = m.value("type").toString();
			if (t == "Forward") return MessageType::Forward;
			if (t == "Call") return MessageType::Call;
			if (t == "System") return MessageType::System;
			return MessageType::Text;
		}();
		{
			const auto fwd = m.value("forward").toObject();
			if (!fwd.isEmpty()) {
				msg.forward = Api::ForwardInfo{
					.authorId = fwd.value("authorId").toString(),
					.messageId = fwd.value("messageId").toString(),
					.chatId = fwd.value("chatId").toString(),
					.createdAt = qint64(fwd.value("createdAt").toDouble()),
				};
			}
		}
		if (msg.type == MessageType::Call) {
			const auto metaObj = m.value("metadata").toObject();
			if (metaObj.value("type").toString() == "CallMetadata") {
				const auto mv = metaObj.value("value").toObject();
				msg.callMeta = Api::CallMetadata{
					.status = mv.value("status").toString(),
					.joinLink = mv.value("joinLink").toString(),
					.webinarEventId = mv.value("webinarEventId").toString(),
					.duration = int(mv.value("duration").toDouble() / 1000),
					.statusReason = mv.value("statusReasonV2").toString(
						mv.value("statusReason").toString()),
				};
			}
		}
		{
			auto mentionsArr = m.value("metadata").toObject()
				.value("value").toObject()
				.value("mentions").toArray();
			if (mentionsArr.isEmpty()) {
				mentionsArr = m.value("mentions").toArray();
			}
			for (const auto &mi : mentionsArr) {
				const auto mo = mi.toObject();
				if (mo.value("type").toString() == "User") {
					msg.mentions.push_back({
						.userId = mo.value("id").toString(),
						.name = mo.value("name").toString(),
					});
				}
			}
		}
		const auto filesArr = m.value("files").toArray();
		for (const auto &f : filesArr) {
			const auto fo = f.toObject();
			msg.files.push_back(Api::ParseFileData(f.toObject()));
		}
		if (msg.type == MessageType::Call) {
			const auto meta = m.value("metadata").toObject();
			if (meta.value("type").toString() == "CallMetadata") {
				const auto v = meta.value("value").toObject();
				msg.callMeta = Api::CallMetadata{
					.status = v.value("status").toString(),
					.joinLink = v.value("joinLink").toString(),
					.webinarEventId = v.value("webinarEventId").toString(),
					.duration = int(v.value("duration").toDouble() / 1000),
					.statusReason = v.value("statusReasonV2").toString(
						v.value("statusReason").toString()),
				};
			}
		}
		const auto clientId = m.value("clientId").toString();
		const auto isThreadReply = takePendingThreadSend(clientId);
		const auto chatPeerId = chatIdToPeerId(chatId);
		const auto isThread = !msg.parentId.isEmpty();
		const auto msgBareId = uuidToBareId(msg.id);
		const auto msgIdVal = MsgId(msgBareId & 0x7FFFFFFFLL);
		HistoryItem *newItem = nullptr;
		if (isThreadReply) {
			replacePendingWithReal(session, chatPeerId, msg, clientId);
		} else if (!replacePendingWithReal(
				session,
				chatPeerId,
				msg,
				clientId)) {
			newItem = addMessage(session, msg, isThread);
		}
		if (const auto mts = session->account().mtsLinkSession()
			; mts && msg.authorId != mts->userId()) {
			markOutboxReadTill(
				session,
				chatPeerId,
				msg.parentId,
				TimeId(msg.createdAt / 1000));
		}

		// A call started just now: the active call of the chat.
		if (!isThread
			&& msg.type == MessageType::Call
			&& msg.callMeta
			&& msg.callMeta->status == u"Started"_q
			&& !msg.callMeta->joinLink.isEmpty()) {
			setActiveCall(session, chatPeerId, *msg.callMeta);
		}
		{
			const auto mts = session->account().mtsLinkSession();
			const auto isOutgoing =
				mts && (msg.authorId == mts->userId());
			if (!isOutgoing) {
				const auto history =
					session->data().history(chatPeerId);
				// The server does not count the thread replies as unread
				// in the chat itself (ChatUnreadMessageCountUpdatedEvent):
				// they are unread in the thread entry only.
				const auto channel = session->data().channelLoaded(
					peerToChannel(chatPeerId));
				const auto skipUnread = isThread;
				// Read already (the open chat at the bottom of the active
				// window): not counted, an unread bar stopped following it.
				const auto readAlready = newItem && !newItem->unread(history);
				if (!skipUnread
					&& !readAlready
					&& history->unreadCountKnown()) {
					history->setUnreadCount(
						history->unreadCount() + 1);
				}
				if (newItem && !isThread && newItem->showNotification()) {
					auto notification = Data::ItemNotification{
						.item = newItem,
						.type = Data::ItemNotificationType::Message,
					};
					newItem->notificationThread()->pushNotification(
						notification);
					Core::App().notifications().schedule(notification);
				}
				// Someone calls: the incoming call window, in channels only
				// the notification opens the window to join the call.
				// A channel of MTS Link (writable channels are megagroups
				// here): no ringing, the notification and the call bar only.
				const auto broadcast = (channel && channel->isBroadcast())
					|| (chatTypeForPeer(chatPeerId) == ChatType::Channel);
				if (newItem
					&& !isThread
					&& msg.type == MessageType::Call
					&& msg.callMeta
					&& msg.callMeta->status == u"Started"_q
					&& !msg.callMeta->joinLink.isEmpty()) {
					CallMessages.insert(newItem->fullId());
					rememberCallEvent(
						msg.callMeta->joinLink,
						msg.callMeta->webinarEventId);
					const auto author = broadcast
						? nullptr
						: newItem->from()->asUser();
					if (author) {
						Core::App().calls().showMtsLinkIncomingCall(
							history->peer,
							author,
							msg.callMeta->joinLink);
					}
				}
			}
		}
		if (isThread) {
			const auto parentMsgId = localMsgId(msg.parentId);
			const auto parent = session->data().message(
				chatPeerId, parentMsgId);
			const auto threadIsOpen = isThreadOpen(chatPeerId, parentMsgId);
			const auto mts = session->account().mtsLinkSession();
			const auto myUserId = (mts && !mts->userId().isEmpty())
				? mts->userId()
				: CachedMyUserId;
			const auto isOwn = !myUserId.isEmpty()
				&& (msg.authorId == myUserId);
			if (parent) {
				// The count itself comes from MessageChildrenCountUpdatedEvent.
				const auto views = parent->Get<HistoryMessageViews>();
				if (isOwn && views && views->commentsMaxId) {
					auto repliesData = HistoryMessageRepliesData();
					repliesData.isNull = false;
					repliesData.repliesCount = views->replies.count;
					repliesData.maxId = views->commentsMaxId;
					repliesData.readMaxId = views->commentsMaxId;
					parent->setReplies(std::move(repliesData));
				}
				session->data().requestItemViewRefresh(parent);
			} else if (!isOwn) {
				const auto key = qMakePair(chatPeerId, parentMsgId);
				auto &info = PendingThreadUnread[key];
				info.count++;
				info.parentUuid = msg.parentId;
			}
			refreshThreadsMark(session, chatPeerId);
			const auto threadIt = ThreadReverseMap.constFind(
				{ chatPeerId, parentMsgId });
			if (threadIt != ThreadReverseMap.constEnd()) {
				// The counter itself comes in ChatUnreadMessageCountUpdated
				// with the "parentId" of the thread.
				addThreadEntryUnread(
					session,
					chatPeerId,
					parentMsgId,
					0,
					TimeId(msg.createdAt / 1000));
				const auto notifiable = ThreadNotifiable.value(
					threadIt.value(),
					true);
				// Unsubscribed (LeaveFromThreadEvent): no notifications, as
				// in MTS Link (no counter comes for it either).
				const auto threadChannel = session->data().channelLoaded(
					peerToChannel(threadIt.value()));
				const auto left = !threadChannel
					|| (threadChannel->flags() & ChannelDataFlag::Left);
				// A mention of me is notified even then.
				const auto mentionsMe = newItem && newItem->mentionsMe();
				// The open thread of an inactive window is notified too.
				const auto windowActive = ranges::any_of(
					session->windows(),
					[](not_null<Window::SessionController*> window) {
						return window->widget()->markingAsRead();
					});
				if (newItem
					&& !isOwn
					&& (!threadIsOpen || !windowActive)
					&& ((notifiable && !left) || mentionsMe)) {
					auto notification = Data::ItemNotification{
						.item = newItem,
						.type = Data::ItemNotificationType::Message,
					};
					newItem->notificationThread()->pushNotification(
						notification);
					Core::App().notifications().schedule(notification);
				}
			}
			// A thread missing from the list is a thread without the
			// subscription (Chat.GetMyThreadV2 answered thread_not_found),
			// a new subscription comes with JoinToThreadEvent.
			const auto history = session->data().history(chatPeerId);
			const auto last = history->lastMessage();
			if (last) {
				last->invalidateChatListEntry();
			} else {
				history->updateChatListEntry();
			}
		}
	} else if (type == "MessageDeletedEvent") {
		const auto messageId = value.value("messageId").toString();
		deleteMessage(session, chatId, messageId);
	} else if (type == "MessageUpdatedV2Event") {
		const auto messageId = value.value("messageId").toString();
		const auto newText = value.value("text").toString();
		auto newMarkdown = value.value("markdown").toString();
		if (newMarkdown.isEmpty()) {
			const auto blocksArr = value.value("blocks").toArray();
			if (!blocksArr.isEmpty()) {
				newMarkdown = markdownFromBlocks(blocksArr);
			}
		}
		const auto updatedAt = value.value("updatedAt").toDouble();

		if (messageId.isEmpty()) {
			return;
		}

		QList<Api::MentionInfo> mentions;
		const auto mentionsArr = value.value("mentions").toArray();
		for (const auto &mi : mentionsArr) {
			const auto mo = mi.toObject();
			if (mo.value("type").toString() == "User") {
				mentions.push_back({
					.userId = mo.value("id").toString(),
					.name = mo.value("name").toString(),
				});
			}
		}

		const auto chatPeerId = chatIdToPeerId(chatId);
		const auto msgBareId = uuidToBareId(messageId);
		const auto existingMsgId = MsgId(msgBareId & 0x7FFFFFFFLL);
		const auto existing = session->data().message(
			chatPeerId, existingMsgId);
		if (existing) {
			existing->setText(parseMentionedText(
				newText, newMarkdown, mentions, session));
			if (updatedAt > 0) {
				existing->setEditDate(TimeId(
					qint64(updatedAt) / 1000));
			}
			session->data().requestItemViewRefresh(existing);
			existing->invalidateChatListEntry();
		}
	} else if (type == "MessageFileDeletedV2Event") {
		const auto messageId = value.value("messageId").toString();
		const auto chatPeerId = chatIdToPeerId(chatId);
		const auto msgBareId = uuidToBareId(messageId);
		const auto msgId = MsgId(msgBareId & 0x7FFFFFFFLL);
		const auto item = session->data().message(chatPeerId, msgId);
		if (item) {
			const auto text = item->originalText();
			const auto date = item->date();
			const auto fromId = item->from()->id;
			item->destroy();
			auto fields = HistoryItemCommonFields{
				.id = msgId,
				.flags = MessageFlag::HasFromId,
				.from = fromId,
				.date = date,
			};
			const auto history = session->data().history(chatPeerId);
			history->addNewExternalMessage(
				std::move(fields),
				TextWithEntities{ text },
				MTP_messageMediaEmpty());
		}
	} else if (type == "MessageUnpinnedEvent") {
		const auto messageId = value.value("messageId").toString();
		const auto chatPeerId = chatIdToPeerId(chatId);
		const auto msgBareId = uuidToBareId(messageId);
		const auto msgId = MsgId(msgBareId & 0x7FFFFFFFLL);
		const auto item = session->data().message(chatPeerId, msgId);
		if (item) {
			item->setIsPinned(false);
		}
		auto &byDate = PinnedByDate[chatPeerId];
		byDate.erase(
			ranges::remove(byDate, msgId, &std::pair<TimeId, MsgId>::second),
			end(byDate));
	} else if (type == "PinnedMessageCountUpdatedEvent") {
		const auto chatPeerId = chatIdToPeerId(chatId);
		const auto mts = session->account().mtsLinkSession();
		if (mts && !chatId.isEmpty()) {
			mts->messages()->reloadPinned(chatId);
		}
	} else if (type == "ChatUnreadMessageCountUpdatedEvent") {
		const auto eventChatId = value.value("chatId").toString();
		if (eventChatId.isEmpty()) {
			return;
		}
		const auto peerId = chatIdToPeerId(eventChatId);
		if (!hasChatId(peerId)) {
			return;
		}
		// With "parentId" it is the unread counter of that thread, not of
		// the chat itself: MTS Link shows it in the "Threads" folder only.
		if (const auto parentId = value.value("parentId").toString()
			; !parentId.isEmpty()) {
			const auto rootId = localMsgId(parentId);
			const auto entry = threadEntryHistory(session, peerId, rootId);
			const auto count = value.value("unreadMessageCount").toInt();
			if (entry) {
				entry->setUnreadCount(count);
				session->data().refreshChatListEntry(Dialogs::Key(entry));
			}
			// The unread dot on the root message in the chat.
			if (const auto root = session->data().message(peerId, rootId)) {
				const auto views = root->Get<HistoryMessageViews>();
				const auto replies = std::max(
					views ? views->replies.count : 0,
					count);
				if (replies > 0) {
					auto repliesData = HistoryMessageRepliesData();
					repliesData.isNull = false;
					repliesData.repliesCount = replies;
					repliesData.maxId = MsgId(replies);
					repliesData.readMaxId = MsgId(
						std::max(replies - count, 1));
					root->setReplies(std::move(repliesData));
					session->data().requestItemViewRefresh(root);
				}
			}
			return;
		}
		const auto history = session->data().historyLoaded(peerId);
		if (!history) {
			return;
		}
		const auto count = value.value("unreadMessageCount").toInt();
		const auto wasReadRequest = consumeReadRequestSent(eventChatId);
		// The server counter is the exact one: a chat read in another
		// client comes here only (the thread counters have "parentId").
		history->setUnreadCount(count);
		if (wasReadRequest && count == 0) {
			history->destroyUnreadBar();
			history->clearFirstUnreadMessage();
		}
		if (const auto last = history->lastMessage()) {
			last->invalidateChatListEntry();
		}
	} else if (type == "ThreadNotificationsSettedEvent") {
		applyThreadNotifiable(
			session,
			value.value("chatId").toString(),
			value.value("messageId").toString(),
			value.value("isNotifiable").toBool(true));
	} else if (type == "LeaveFromThreadEvent") {
		applyThreadLeft(
			session,
			value.value("chatId").toString(),
			value.value("threadId").toString());
	} else if (type == "JoinToThreadEvent") {
		const auto threadId = value.value("threadId").toString();
		const auto mts = session->account().mtsLinkSession();
		if (!threadId.isEmpty() && mts && mts->threads()) {
			mts->threads()->loadThread(threadId);
		}
	} else if (type == "PinnedChatEvent") {
		const auto eventChatId = value.value("chatId").toString();
		if (eventChatId.isEmpty()) {
			return;
		}
		const auto peerId = chatIdToPeerId(eventChatId);
		if (!hasChatId(peerId)) {
			return;
		}
		const auto history = session->data().historyLoaded(peerId);
		if (history) {
			session->data().setChatPinned(history, FilterId(), true);
			scheduleFolderPinsSync(session);
		}
	} else if (type == "UnpinnedChatEvent") {
		const auto eventChatId = value.value("chatId").toString();
		if (eventChatId.isEmpty()) {
			return;
		}
		const auto peerId = chatIdToPeerId(eventChatId);
		if (!hasChatId(peerId)) {
			return;
		}
		const auto history = session->data().historyLoaded(peerId);
		if (history) {
			session->data().setChatPinned(history, FilterId(), false);
			scheduleFolderPinsSync(session);
		}
	} else if (type == "ChatNotificationsSettedEvent") {
		const auto eventChatId = value.value("chatId").toString();
		if (eventChatId.isEmpty()) {
			return;
		}
		const auto peerId = chatIdToPeerId(eventChatId);
		if (!hasChatId(peerId)) {
			return;
		}
		const auto isNotifiable = value.value("isNotifiable").toBool(true);
		session->data().notifySettings().apply(
			session->data().peer(peerId), makeMuteSettings(!isNotifiable));
	} else if (type == "MessageChildrenCountUpdatedEvent") {
		const auto messageId = value.value("messageId").toString();
		const auto childrenCount = value.value("childrenCount").toInt();
		if (messageId.isEmpty()) {
			return;
		}
		const auto chatPeerId = chatIdToPeerId(chatId);
		const auto mapped = MtsLinkIdToMsgMap.constFind(messageId);
		const auto msgId = (mapped != MtsLinkIdToMsgMap.constEnd())
			? mapped.value().second
			: MsgId(uuidToBareId(messageId) & 0x7FFFFFFFLL);
		const auto item = session->data().message(chatPeerId, msgId);
		if (item) {
			auto repliesData = HistoryMessageRepliesData();
			repliesData.isNull = false;
			repliesData.repliesCount = childrenCount;
			repliesData.maxId = MsgId(childrenCount);
			item->setReplies(std::move(repliesData));
			if (CallMetaByItem.contains(item->fullId())) {
				// The local materials are in the thread as well.
				CallServerChildren.insert(item->fullId(), childrenCount);
				updateCallReplies(item);
			}
			session->data().requestItemViewRefresh(item);
			refreshThreadsMark(session, chatPeerId);
		}
	} else if (type == "MessageReactionsUpdatedEvent") {
		const auto messageId = value.value("messageId").toString();
		if (messageId.isEmpty()) {
			return;
		}
		const auto chatPeerId = chatIdToPeerId(chatId);
		const auto mapped = MtsLinkIdToMsgMap.constFind(messageId);
		const auto msgId = (mapped != MtsLinkIdToMsgMap.constEnd())
			? mapped.value().second
			: MsgId(uuidToBareId(messageId) & 0x7FFFFFFFLL);
		const auto item = session->data().message(chatPeerId, msgId);
		if (!item) {
			return;
		}
		if (session->data().reactions().sending(item)) {
			session->data().reactions().clearMtsLinkSending(item);
			return;
		}
		const auto arr = value.value("reactions").toArray();
		bool hasDoubleCount = false;
		for (const auto &r : arr) {
			if (r.toObject().value("count").toInt() >= 2) {
				hasDoubleCount = true;
				break;
			}
		}
		if (!hasDoubleCount) {
			return;
		}
		QList<Api::ReactionData> reactions;
		for (const auto &r : arr) {
			const auto ro = r.toObject();
			const auto count = ro.value("count").toInt();
			reactions.push_back({
				.emojiId = ro.value("emojiId").toString(),
				.emoji = ro.value("emoji").toString(),
				.count = count,
				.selected = (count >= 2),
			});
		}
		const auto mtp = buildMtpReactions(reactions);
		if (mtp) {
			item->updateReactions(&*mtp);
		} else {
			item->updateReactions(nullptr);
		}
	} else if (type == "MessageReactionAddedEvent"
		|| type == "MessageReactionDeletedEvent") {
		const auto emoji = value.value("emoji").toString();
		const auto emojiId = value.value("emojiId").toString();
		const auto reactionUserId = value.value("userId").toString();
		const auto reactionMsgId = value.value("messageId").toString();
		const auto isAdded = (type == "MessageReactionAddedEvent");
		if (!emoji.isEmpty() && !emojiId.isEmpty()) {
			const auto isNew = !IdToEmojiMap.contains(emojiId)
				|| IdToEmojiMap[emojiId] != emoji;
			IdToEmojiMap[emojiId] = emoji;
			EmojiToIdMap[emoji] = emojiId;
			if (isNew) {
				saveEmojiMaps();
			}
		}
		if (!reactionMsgId.isEmpty() && !emojiId.isEmpty()) {
			const auto chatPeerId = chatIdToPeerId(chatId);
			const auto mapped = MtsLinkIdToMsgMap.constFind(reactionMsgId);
			const auto msgId = (mapped != MtsLinkIdToMsgMap.constEnd())
				? mapped.value().second
				: MsgId(uuidToBareId(reactionMsgId) & 0x7FFFFFFFLL);
			const auto item = session->data().message(chatPeerId, msgId);
			if (!item) {
				return;
			}
			{
				const auto isMine =
					(reactionUserId == CachedMyUserId);
				const auto resolvedEmoji = emoji.isEmpty()
					? IdToEmojiMap.value(emojiId)
					: emoji;
				if (resolvedEmoji.isEmpty()) {
					return;
				}
				if (isMine) {
					return;
				}
				const auto &existing = item->reactions();
				QList<Api::ReactionData> reactions;
				auto found = false;
				for (const auto &r : existing) {
					const auto rEmoji = r.id.emoji();
					if (rEmoji == resolvedEmoji) {
						found = true;
						if (!isAdded) {
							if (r.count <= 1) {
								continue;
							}
							reactions.push_back({
								.emojiId = emojiId,
								.emoji = resolvedEmoji,
								.count = r.count - 1,
								.selected = r.my,
							});
						} else {
							reactions.push_back({
								.emojiId = emojiId,
								.emoji = resolvedEmoji,
								.count = r.count,
								.selected = r.my,
							});
						}
					} else if (!rEmoji.isEmpty()) {
						reactions.push_back({
							.emojiId = EmojiToIdMap.value(rEmoji),
							.emoji = rEmoji,
							.count = r.count,
							.selected = r.my,
						});
					}
				}
				if (!found && isAdded) {
					reactions.push_back({
						.emojiId = emojiId,
						.emoji = resolvedEmoji,
						.count = 1,
						.selected = false,
					});
				}
				const auto mtp = buildMtpReactions(reactions);
				if (mtp) {
					item->updateReactions(&*mtp);
				} else {
					item->updateReactions(nullptr);
				}
			}
		}
	} else if (type == "MessageUpdatedEvent") {
		const auto updated = value.value("updated").toObject();
		if (updated.value("type").toString() == "Call") {
			const auto messageId = updated.value("id").toString();
			if (messageId.isEmpty()) {
				return;
			}
			const auto meta = updated.value("metadata").toObject();
			if (meta.value("type").toString() != "CallMetadata") {
				return;
			}
			const auto mv = meta.value("value").toObject();
			const auto callMeta = Api::CallMetadata{
				.status = mv.value("status").toString(),
				.joinLink = mv.value("joinLink").toString(),
				.webinarEventId = mv.value("webinarEventId").toString(),
				.duration = int(mv.value("duration").toDouble() / 1000),
				.statusReason = mv.value("statusReasonV2").toString(
					mv.value("statusReason").toString()),
			};
			const auto &status = callMeta.status;
			const auto &joinLink = callMeta.joinLink;
			const auto chatPeerId = chatIdToPeerId(chatId);
			const auto callMapped = MtsLinkIdToMsgMap.constFind(messageId);
			const auto msgId = (callMapped != MtsLinkIdToMsgMap.constEnd())
				? callMapped.value().second
				: MsgId(uuidToBareId(messageId) & 0x7FFFFFFFLL);
			const auto item = session->data().message(chatPeerId, msgId);
			if (status == "Ended") {
				setActiveCall(session, chatPeerId, QString());
			} else if (status == "Started"
				&& !joinLink.isEmpty()) {
				setActiveCall(session, chatPeerId, callMeta);
			}
			if (item) {
				applyCallItem(
					session,
					item,
					callMeta,
					(status == u"Ended"_q));
				session->data().requestItemViewRefresh(item);
			}
		}
	} else if (type == "AddChannelAdministratorsEvent"
		|| type == "RemovedChannelAdministratorsEvent") {
		const auto channel = session->data().channelLoaded(
			peerToChannel(chatIdToPeerId(chatId)));
		const auto mts = session->account().mtsLinkSession();
		if (!channel || !mts) {
			return;
		}
		const auto added = (type == "AddChannelAdministratorsEvent");
		if (eventUsers(value).contains(mts->userId())) {
			const auto was = MyChannelRoles.value(channel->id);
			const auto now = added
				? ((was == u"Owner"_q) ? was : u"Admin"_q)
				: u"Member"_q;
			LOG(("MtsLink Channel: my role %1 -> %2 in %3"
				).arg(was, now, chatId));
			if (was != now) {
				applyMyChannelRole(channel, now);
				// Ownership transfer makes the old owner an admin quietly.
				if (now == u"Admin"_q && was != u"Owner"_q) {
					showMtsLinkToast(
						session,
						tr::lng_mtslink_role_admin_granted(
							tr::now,
							lt_chat,
							channel->name()));
				} else if (now == u"Member"_q) {
					showMtsLinkToast(
						session,
						tr::lng_mtslink_role_admin_revoked(
							tr::now,
							lt_chat,
							channel->name()));
				}
				session->changes().peerUpdated(
					channel,
					Data::PeerUpdate::Flag::Rights);
			}
		}
		scheduleChannelMembersReload(session, chatId);
	} else if (type == "ChatOwnerChangedEvent") {
		const auto channel = session->data().channelLoaded(
			peerToChannel(chatIdToPeerId(chatId)));
		const auto mts = session->account().mtsLinkSession();
		if (!channel || !mts) {
			return;
		}
		const auto userId = value.value("userId").toString();
		const auto ownerPeerId = PeerId(::UserId(uuidToBareId(userId)));
		const auto mine = (userId == mts->userId());
		const auto was = MyChannelRoles.value(channel->id);
		LOG(("MtsLink Channel: owner of %1 is %2, my role was %3"
			).arg(chatId, userId, was));
		ChannelOwners.insert(channel->id, ownerPeerId);
		if (mine && was != u"Owner"_q) {
			applyMyChannelRole(channel, u"Owner"_q);
			showMtsLinkToast(
				session,
				tr::lng_mtslink_role_owner_granted(
					tr::now,
					lt_chat,
					channel->name()));
			session->changes().peerUpdated(
				channel,
				Data::PeerUpdate::Flag::Rights);
		} else if (!mine && was == u"Owner"_q) {
			// AddChannelAdministratorsEvent makes us an admin after that.
			applyMyChannelRole(channel, u"Member"_q);
			session->changes().peerUpdated(
				channel,
				Data::PeerUpdate::Flag::Rights);
		}
		scheduleChannelMembersReload(session, chatId);
	} else if (type == "ChannelUsersAddedEvent"
		|| type == "ChannelUsersRemovedEvent"
		|| type == "GroupChatUsersAddedEvent"
		|| type == "GroupChatUsersRemovedEvent") {
		const auto mts = session->account().mtsLinkSession();
		if (!mts) {
			return;
		}
		const auto removed = (type == "ChannelUsersRemovedEvent")
			|| (type == "GroupChatUsersRemovedEvent");
		const auto self = eventUsers(value).contains(mts->userId());
		const auto channel = session->data().channelLoaded(
			peerToChannel(chatIdToPeerId(chatId)));
		if (self && removed) {
			if (channel && channel->amIn() && SelfLeavingChats.contains(chatId)) {
				LOG(("MtsLink Channel: left %1").arg(chatId));
				applyChannelLeft(session, channel);
			} else if (channel && channel->amIn()) {
				LOG(("MtsLink Channel: removed from %1").arg(chatId));
				showMtsLinkToast(
					session,
					tr::lng_mtslink_removed_from_channel(
						tr::now,
						lt_chat,
						channel->name()));
				applyChannelLeft(session, channel);
			}
			return;
		} else if (self && (!channel || !channel->amIn())) {
			LOG(("MtsLink Channel: added to %1").arg(chatId));
			JustJoinedChats.insert(chatId);
			mts->channels()->loadChatInfo(chatId);
			return;
		}
		if (channel) {
			scheduleChannelMembersReload(session, chatId);
		}
	} else if (type == "LastReadMessageUpdatedEvent") {
		// The others have read the own messages till this one.
		const auto messageId = value.value("messageId").toString();
		const auto parentId = value.value("parentMessageId").toString();
		const auto peerId = chatIdToPeerId(chatId);
		// A sent message keeps the id of its local message.
		const auto known = MtsLinkIdToMsgMap.constFind(messageId);
		const auto msgId = (known != MtsLinkIdToMsgMap.constEnd())
			? known->second
			: MsgId(uuidToBareId(messageId) & 0x7FFFFFFFLL);
		const auto item = messageId.isEmpty()
			? nullptr
			: session->data().message(peerId, msgId);
		if (item) {
			markOutboxReadTill(session, peerId, parentId, item->date());
		}
	} else if (type == "ChannelMembersCountChanged"
		|| type == "GroupChatMembersCountChanged") {
		const auto channel = session->data().channelLoaded(
			peerToChannel(chatIdToPeerId(chatId)));
		const auto count = value.value("membersCount").toInt();
		if (channel && count > 0) {
			channel->setMembersCount(count);
		}
	} else if (type == "ChannelUpdatedEvent"
		|| type == "ChannelUpdatedEventV2"
		|| type == "GroupChatUpdatedEvent"
		|| type == "GroupChatUpdatedEventV2") {
		const auto channel = session->data().channelLoaded(
			peerToChannel(chatIdToPeerId(chatId)));
		if (!channel) {
			return;
		}
		const auto updated = value.value("updated").toObject();
		const auto name = updated.value("name").toString();
		if (!name.isEmpty() && name != channel->name()) {
			channel->setName(name, {});
		}
		if (updated.contains("description")) {
			channel->setAbout(updated.value("description").toString());
		}
		if (updated.contains("isPublic")) {
			setChannelPublic(channel->id, updated.value("isPublic").toBool());
		}
		if (updated.contains("isReadOnly")) {
			const auto readOnly = updated.value("isReadOnly").toBool();
			if (readOnly != channel->isBroadcast()) {
				auto flags = channel->flags();
				if (readOnly) {
					flags |= ChannelDataFlag::Broadcast;
					flags &= ~ChannelDataFlag::Megagroup;
				} else {
					flags |= ChannelDataFlag::Megagroup;
					flags &= ~ChannelDataFlag::Broadcast;
				}
				channel->setFlags(flags);
				applyMyChannelRole(channel, QString());
			}
		}
	} else if (type == "ChannelCoverAddedEvent"
		|| type == "GroupChatCoverAddedEvent") {
		const auto channel = session->data().channelLoaded(
			peerToChannel(chatIdToPeerId(chatId)));
		const auto fileId = value.value("coverFileId").toString();
		if (channel && !fileId.isEmpty()) {
			applyUserpic(channel, fileId, __LINE__);
		}
	} else if (type == "NewChannelEvent"
		|| type == "MemberJoinedChannelEvent"
		|| type == "NewGroupChatEvent"
		|| type == "MemberJoinedGroupChatEvent") {
		const auto channel = session->data().channelLoaded(
			peerToChannel(chatIdToPeerId(chatId)));
		if (!channel || !channel->amIn()) {
			if (const auto mts = session->account().mtsLinkSession()) {
				LOG(("MtsLink Channel: %1, loading %2").arg(type, chatId));
				JustJoinedChats.insert(chatId);
				mts->channels()->loadChatInfo(chatId);
			}
		}
	} else if (type == "MemberLeftChannelEvent"
		|| type == "ChannelDeletedEvent"
		|| type == "MemberLeftGroupChatEvent"
		|| type == "GroupChatDeletedEvent") {
		const auto channel = session->data().channelLoaded(
			peerToChannel(chatIdToPeerId(chatId)));
		if (channel && channel->amIn()) {
			LOG(("MtsLink Channel: %1 %2").arg(type, chatId));
			applyChannelLeft(session, channel);
		}
	} else if (type == "CallStatusUpdatedEvent") {
		const auto messageId = value.value("messageId").toString();
		if (messageId.isEmpty()) {
			return;
		}
		const auto meta = value.value("metadata").toObject();
		const auto callMeta = Api::CallMetadata{
			.status = meta.value("status").toString(),
			.joinLink = meta.value("joinLink").toString(),
			.webinarEventId = meta.value("webinarEventId").toString(),
			.duration = int(meta.value("duration").toDouble() / 1000),
			.statusReason = meta.value("statusReasonV2").toString(
				meta.value("statusReason").toString()),
		};
		const auto &status = callMeta.status;
		const auto &joinLink = callMeta.joinLink;
		const auto chatPeerId = chatIdToPeerId(chatId);
		if (status == "Ended") {
			setActiveCall(session, chatPeerId, QString());
			if (const auto peer = session->data().peerLoaded(chatPeerId)) {
				Core::App().calls().mtsLinkCallEnded(peer);
			}
		} else if (status == "Started" && !joinLink.isEmpty()) {
			setActiveCall(session, chatPeerId, callMeta);
		}
		const auto csMapped = MtsLinkIdToMsgMap.constFind(messageId);
		const auto msgId = (csMapped != MtsLinkIdToMsgMap.constEnd())
			? csMapped.value().second
			: MsgId(uuidToBareId(messageId) & 0x7FFFFFFFLL);
		const auto item = session->data().message(chatPeerId, msgId);
		if (item) {
			applyCallItem(session, item, callMeta);
			session->data().requestItemViewRefresh(item);
		}
	}
}

void refreshChat(not_null<Main::Session*> session, PeerId peerId) {
	const auto mts = session->account().mtsLinkSession();
	const auto chatId = peerIdToChatId(peerId);
	if (!mts || chatId.isEmpty()) {
		return;
	}
	LOG(("MtsLink: refresh of %1 requested").arg(chatId));
	mts->messages()->load(chatId);
	mts->messages()->reloadPinned(chatId);
	mts->channels()->loadChatInfo(chatId);
	if (chatTypeForPeer(peerId) != ChatType::Dialog) {
		mts->users()->loadChatMembers(chatId);
	}
}

void refreshThread(
		not_null<Main::Session*> session,
		PeerId parentPeerId,
		MsgId rootId) {
	const auto mts = session->account().mtsLinkSession();
	const auto threadId = msgIdToMtsLinkId(parentPeerId, rootId);
	if (!mts || !mts->threads() || threadId.isEmpty()) {
		return;
	}
	LOG(("MtsLink: refresh of the thread %1 requested").arg(threadId));
	mts->threads()->loadThread(threadId);
	if (const auto list = cachedRepliesList(parentPeerId, rootId)) {
		list->mtsLinkRefresh();
	}
}

void deleteMessage(
		not_null<Main::Session*> session,
		const ChatId &chatId,
		const MessageId &messageId) {
	const auto chatPeerId = chatIdToPeerId(chatId);
	const auto mapped = MtsLinkIdToMsgMap.constFind(messageId);
	const auto msgId = (mapped != MtsLinkIdToMsgMap.constEnd())
		? mapped.value().second
		: MsgId(uuidToBareId(messageId) & 0x7FFFFFFFLL);
	if (const auto item = session->data().message(chatPeerId, msgId)) {
		item->destroy();
	}
	// The cached messages of the chat are dropped even if the message is
	// already destroyed (deleted here, the event comes after that): it
	// was shown from the cache after a restart otherwise.
	const auto hash = QCryptographicHash::hash(
		chatId.toUtf8(), QCryptographicHash::Md5);
	uint64 low = 0;
	memcpy(&low, hash.constData(), sizeof(low));
	session->data().cache().remove(
		Storage::Cache::Key{ kMtsLinkMsgCacheTag, low });
}

void updateMessage(
		not_null<Main::Session*> session,
		const Api::MessageData &src) {
	const auto chatPeerId = chatIdToPeerId(src.chatId);
	const auto mapped = MtsLinkIdToMsgMap.constFind(src.id);
	const auto msgId = (mapped != MtsLinkIdToMsgMap.constEnd())
		? mapped.value().second
		: MsgId(uuidToBareId(src.id) & 0x7FFFFFFFLL);
	const auto item = session->data().message(chatPeerId, msgId);
	if (item) {
		item->setText(parseMentionedText(src.text, src.markdown, src.mentions, session));
		session->data().requestItemTextRefresh(item);
	}
}

void registerMessageId(PeerId peerId, MsgId msgId, const QString &mtsLinkId) {
	MsgIdToMtsLinkIdMap.insert(makeMsgKey(peerId, msgId), mtsLinkId);
	MtsLinkIdToMsgMap.insert(mtsLinkId, {peerId, msgId});
}

void setPendingTempMessage(PeerId peerId, MsgId msgId) {
	PendingTempMessages[peerId].push_back(msgId);
}

void addPendingThreadSend(const QString &clientId) {
	PendingThreadClientIds.insert(clientId);
}

bool takePendingThreadSend(const QString &clientId) {
	return PendingThreadClientIds.remove(clientId);
}

void clearPendingTempMessage(
		not_null<Main::Session*> session,
		PeerId peerId) {
	const auto it = PendingTempMessages.find(peerId);
	if (it == PendingTempMessages.end()) {
		return;
	}
	const auto ids = it.value();
	PendingTempMessages.erase(it);
	for (const auto &tempMsgId : ids) {
		if (const auto item = session->data().message(peerId, tempMsgId)) {
			item->destroy();
		}
	}
}

namespace {

struct PendingSend {
	PeerId peerId = 0;
	MsgId localId = 0;
	QString chatId;
	QString text;
	QJsonArray blocks;
	QJsonArray mentionsMeta;
	QString replyToId;
	QStringList fileIds;
	QString parentId;
};
QHash<QString, PendingSend> PendingSends; // By the client id.

struct PendingUpload {
	PeerId peerId = 0;
	MsgId localId = 0;
	Fn<void()> upload;
};
std::vector<PendingUpload> PendingUploads;

void sendTracked(not_null<Main::Session*> session, const QString &clientId) {
	const auto i = PendingSends.constFind(clientId);
	const auto mts = session->account().mtsLinkSession();
	if (i == PendingSends.constEnd() || !mts || !mts->rpc()) {
		return;
	} else if (!mts->rpc()->isConnected()) {
		LOG(("MtsLink Sending: %1 waits for the connection").arg(clientId));
		return;
	}
	const auto &send = i.value();
	const auto weak = base::make_weak(session);
	mts->sending()->sendMessage(
		send.chatId,
		send.text,
		send.blocks,
		send.mentionsMeta,
		send.replyToId,
		send.fileIds,
		send.parentId,
		clientId,
		[=](const QJsonObject &result) {
			const auto strong = weak.get();
			const auto type = result.value(u"type"_q).toString();
			if (!strong || type != u"BusinessError"_q) {
				return;
			}
			// Not to be sent: the failed message (red), may be deleted.
			const auto send = PendingSends.take(clientId);
			LOG(("MtsLink Sending: %1 failed").arg(clientId));
			markLocalFailed(strong, send.peerId, send.localId);
		},
		[=](const QString &error) {
			// Lost with the connection: sent again after a reconnect.
			LOG(("MtsLink Sending: %1 %2, waits").arg(clientId, error));
		});
}

} // namespace

void addPendingUpload(PeerId peerId, MsgId localId, Fn<void()> upload) {
	PendingUploads.push_back({ peerId, localId, std::move(upload) });
}

void resendPendingSends(not_null<Main::Session*> session) {
	for (auto &pending : base::take(PendingUploads)) {
		if (!session->data().message(pending.peerId, pending.localId)) {
			continue; // Deleted meanwhile.
		}
		LOG(("MtsLink Sending: upload of %1 again").arg(pending.localId.bare));
		pending.upload();
	}
	for (const auto &clientId : PendingSends.keys()) {
		const auto &send = PendingSends[clientId];
		if (!session->data().message(send.peerId, send.localId)) {
			PendingSends.remove(clientId); // Deleted meanwhile.
			continue;
		}
		LOG(("MtsLink Sending: %1 sent again").arg(clientId));
		sendTracked(session, clientId);
	}
}

void markLocalSending(
		not_null<Main::Session*> session,
		PeerId peerId,
		MsgId localId) {
	if (const auto item = session->data().message(peerId, localId)) {
		item->mtsLinkSetSending(true);
	}
}

void markLocalFailed(
		not_null<Main::Session*> session,
		PeerId peerId,
		MsgId localId) {
	// Not replaced by a message sent back by the server.
	if (const auto it = PendingTempMessages.find(peerId)
		; it != PendingTempMessages.end()) {
		it->removeOne(localId);
	}
	if (const auto item = session->data().message(peerId, localId)) {
		if (!item->isSending()) {
			item->mtsLinkSetSending(true);
		}
		item->sendFailed();
	}
}

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
		QString clientId) {
	if (clientId.isEmpty()) {
		clientId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	}
	PendingSends.insert(clientId, PendingSend{
		.peerId = peerId,
		.localId = localId,
		.chatId = chatId,
		.text = text,
		.blocks = blocks,
		.mentionsMeta = mentionsMeta,
		.replyToId = replyToId,
		.fileIds = fileIds,
		.parentId = parentId,
	});
	markLocalSending(session, peerId, localId);
	sendTracked(session, clientId);
	return clientId;
}

bool replacePendingWithReal(
		not_null<Main::Session*> session,
		PeerId peerId,
		const Api::MessageData &realMsg,
		const QString &clientId) {
	// The local message of this send (by the client id), or the first one.
	const auto sent = PendingSends.take(clientId);
	const auto it = PendingTempMessages.find(peerId);
	const auto inList = (it != PendingTempMessages.end()) && !it->isEmpty();
	auto tempMsgId = MsgId();
	if (sent.localId
		&& sent.peerId == peerId
		&& session->data().message(peerId, sent.localId)) {
		// By the client id: the sends to a thread are not in the list (the
		// GIF / file sent to a thread kept its upload progress forever).
		tempMsgId = sent.localId;
	} else if (inList) {
		tempMsgId = it->first();
	} else {
		return false;
	}
	if (inList) {
		it->removeOne(tempMsgId);
		if (it->isEmpty()) {
			PendingTempMessages.erase(it);
		}
	}
	const auto item = session->data().message(peerId, tempMsgId);
	if (!item) {
		return false;
	}
	item->mtsLinkSetSending(false);
	if (const auto media = item->media()) {
		if (!realMsg.files.isEmpty()) {
			const auto &f = realMsg.files.first();
			if (const auto photo = media->photo()) {
				const auto thumbUrl = privateCdnThumbBase()
					+ f.id + u"_s.jpg"_q;
				const auto fullUrl = privateCdnThumbBase()
					+ f.id + u".jpg"_q;
				const auto w = f.width > 0 ? f.width : 100;
				const auto h = f.height > 0 ? f.height : 100;
				photo->clearImages();
				photo->updateImages(
					QByteArray(),
					ImageWithLocation{},
					ImageWithLocation{
						.location = ImageLocation(
							DownloadLocation{
								PlainUrlLocation{ thumbUrl } },
							w, h),
					},
					ImageWithLocation{
						.location = ImageLocation(
							DownloadLocation{
								PlainUrlLocation{ fullUrl } },
							w, h),
					},
					ImageWithLocation{},
					ImageWithLocation{},
					crl::time(0));
			} else if (const auto doc = media->document()) {
				LOG(("MtsLink Files: sent document %1 mime=%2 size=%3x%4 "
					"thumbnail=%5"
					).arg(f.id
					).arg(f.mime
					).arg(f.width
					).arg(f.height
					).arg(doc->hasThumbnail() ? 1 : 0));
				doc->uploadingData = nullptr;
				doc->setContentUrl(
					fileDownloadBase() + f.id + u"/download"_q);
				if (f.width > 0 && f.height > 0) {
					doc->dimensions = QSize(f.width, f.height);
				}
				if (doc->isGifv()) {
					// Saved and sent by the real file of MTS Link.
					GifFiles.insert(doc->id, f);
				}
				session->data().requestItemResize(item);
			}
		}
	}
	item->setText(parseMentionedText(
		realMsg.text, realMsg.markdown, realMsg.mentions, session));
	registerMessageId(peerId, tempMsgId, realMsg.id);
	if (realMsg.isRead) {
		setOutboxUnread(
			session,
			peerId,
			tempMsgId,
			realMsg.parentId,
			!*realMsg.isRead);
	}
	session->data().requestItemTextRefresh(item);
	item->invalidateChatListEntry();
	return true;
}

QString msgIdToMtsLinkId(PeerId peerId, MsgId msgId) {
	return MsgIdToMtsLinkIdMap.value(makeMsgKey(peerId, msgId));
}

namespace {

QString chatTypeSlug(ChatType type) {
	switch (type) {
	case ChatType::Channel: return u"channel"_q;
	case ChatType::GroupChat: return u"group"_q;
	case ChatType::Discussion: return u"group"_q;
	case ChatType::Dialog: return u"direct"_q;
	case ChatType::Favorites: return u"direct"_q;
	}
	return u"channel"_q;
}

} // namespace

QString buildChatLink(PeerId peerId) {
	const auto chatId = peerIdToChatId(peerId);
	if (chatId.isEmpty()) {
		return {};
	}
	const auto type = chatTypeForPeer(peerId);
	return EnvConfig::instance().webinarHost()
		+ u"/chats/"_q + chatTypeSlug(type)
		+ u"/"_q + chatId;
}

QString buildMessageLink(
		not_null<HistoryItem*> item,
		bool inRepliesContext) {
	const auto peerId = item->history()->peer->id;
	const auto chatId = peerIdToChatId(peerId);
	if (chatId.isEmpty()) {
		return {};
	}
	const auto type = chatTypeForPeer(peerId);
	const auto base = EnvConfig::instance().webinarHost()
		+ u"/chats/"_q + chatTypeSlug(type)
		+ u"/"_q + chatId;

	const auto msgUuid = msgIdToMtsLinkId(peerId, item->id);
	if (msgUuid.isEmpty()) {
		return base;
	}

	const auto rootId = threadRootFor(peerId, item->id);
	if (rootId && inRepliesContext) {
		const auto threadUuid = msgIdToMtsLinkId(peerId, rootId);
		if (!threadUuid.isEmpty()) {
			return base
				+ u"/thread/"_q + threadUuid
				+ u"/message/"_q + msgUuid;
		}
	}
	return base + u"/thread/"_q + msgUuid;
}

void shortenAndCopy(
		not_null<Main::Session*> session,
		const QString &fullUrl) {
	QGuiApplication::clipboard()->setText(fullUrl);
	const auto mts = session->account().mtsLinkSession();
	if (!mts) {
		return;
	}
	QJsonObject param;
	param["url"] = fullUrl;
	mts->rpc()->call(
		"Shortener.Shorten",
		param,
		[](const QJsonObject &result) {
			const auto shortUrl = result.value("value").toObject()
				.value("shortUrl").toString();
			if (!shortUrl.isEmpty()) {
				QGuiApplication::clipboard()->setText(shortUrl);
			}
		});
}

void registerThreadRoot(PeerId peerId, MsgId msgId, MsgId rootId) {
	ThreadRootMap[makeMsgKey(peerId, msgId)] = rootId;
}

MsgId threadRootFor(PeerId peerId, MsgId msgId) {
	return ThreadRootMap.value(makeMsgKey(peerId, msgId));
}

QHash<quint64, ThreadScrollState> ThreadScrollMap;

void saveThreadScroll(PeerId peerId, MsgId rootId, const ThreadScrollState &state) {
	ThreadScrollMap[makeMsgKey(peerId, rootId)] = state;
}

std::optional<ThreadScrollState> threadScroll(PeerId peerId, MsgId rootId) {
	const auto it = ThreadScrollMap.constFind(makeMsgKey(peerId, rootId));
	if (it != ThreadScrollMap.constEnd()) {
		return *it;
	}
	return std::nullopt;
}


rpl::event_stream<> &OpenThreadChangesStream() {
	static auto result = rpl::event_stream<>();
	return result;
}

void setCurrentOpenThread(PeerId peerId, MsgId rootId) {
	if (CurrentOpenThreads.value(peerId) != rootId) {
		CurrentOpenThreads[peerId] = rootId;
		OpenThreadChangesStream().fire({});
	}
}

void clearCurrentOpenThread(PeerId peerId, MsgId rootId) {
	const auto i = CurrentOpenThreads.find(peerId);
	if (i != CurrentOpenThreads.end() && i.value() == rootId) {
		CurrentOpenThreads.erase(i);
		OpenThreadChangesStream().fire({});
	}
}

rpl::producer<> openThreadChanges() {
	return OpenThreadChangesStream().events();
}

bool isThreadOpen(PeerId peerId, MsgId rootId) {
	const auto it = CurrentOpenThreads.constFind(peerId);
	return it != CurrentOpenThreads.constEnd() && it.value() == rootId;
}

PeerId currentOpenThreadAuthor(PeerId parentPeerId) {
	const auto it = CurrentOpenThreads.constFind(parentPeerId);
	if (it == CurrentOpenThreads.constEnd()) {
		return PeerId(0);
	}
	const auto rootId = it.value();
	const auto rev = ThreadReverseMap.constFind({ parentPeerId, rootId });
	if (rev == ThreadReverseMap.constEnd()) {
		return PeerId(0);
	}
	return ThreadAuthorMap.value(rev.value());
}

using RepliesKey = std::pair<PeerId, MsgId>;
static QMap<RepliesKey, std::shared_ptr<Data::RepliesList>> RepliesListCache;

void cacheRepliesList(PeerId peerId, MsgId rootId, std::shared_ptr<Data::RepliesList> replies) {
	RepliesListCache[{peerId, rootId}] = std::move(replies);
}

std::shared_ptr<Data::RepliesList> cachedRepliesList(PeerId peerId, MsgId rootId) {
	const auto it = RepliesListCache.constFind({peerId, rootId});
	if (it != RepliesListCache.constEnd()) {
		return *it;
	}
	return nullptr;
}

void fetchThreadLastRead(
		not_null<Main::Session*> session,
		PeerId peerId,
		MsgId rootId) {
	const auto mts = session->account().mtsLinkSession();
	if (!mts) {
		return;
	}
	const auto chatId = peerIdToChatId(peerId);
	const auto rootMtsId = msgIdToMtsLinkId(peerId, rootId);
	if (chatId.isEmpty() || rootMtsId.isEmpty()) {
		return;
	}
	QJsonObject param;
	param["organizationId"] = mts->organizationId();
	param["chatId"] = chatId;
	param["messageId"] = rootMtsId;
	mts->rpc()->call(
		"Chat.GetLastReadChildMessage",
		param,
		[session, peerId, rootId](const QJsonObject &result) {
			const auto obj = result.value("value").toObject();
			const auto lastReadId = obj.value("id").toString();
			if (lastReadId.isEmpty()) {
				return;
			}
			const auto bareId = uuidToBareId(lastReadId);
			const auto msgId = MsgId(bareId & 0x7FFFFFFFLL);
			if (auto cached = cachedRepliesList(peerId, rootId)) {
				cached->setInboxReadTill(msgId, std::nullopt);
				const auto createdAt = obj.value("createdAt").toDouble();
				if (createdAt > 0) {
					cached->setMtsLinkInboxReadDate(
						TimeId(qint64(createdAt) / 1000));
				}
			}
		});
}

void setOldestLoadedMessageId(PeerId peerId, const QString &mtsLinkId) {
	OldestLoadedMsgMap[peerId] = mtsLinkId;
}

QString oldestLoadedMessageId(PeerId peerId) {
	return OldestLoadedMsgMap.value(peerId);
}

namespace {

Storage::Cache::Key messageCacheKey(const QString &chatId) {
	const auto hash = QCryptographicHash::hash(
		chatId.toUtf8(), QCryptographicHash::Md5);
	uint64 low = 0;
	memcpy(&low, hash.constData(), sizeof(low));
	return { kMtsLinkMsgCacheTag, low };
}

QByteArray serializeMessages(
		const QList<Api::MessageData> &messages,
		const QList<Api::MemberProfile> &profiles) {
	QByteArray result;
	QDataStream s(&result, QIODevice::WriteOnly);
	s.setVersion(QDataStream::Qt_5_1);

	s << qint32(4); // format version
	qint32 msgCount = 0;
	for (const auto &m : messages) {
		if (m.type == MessageType::Call
			&& m.callMeta
			&& m.callMeta->status == "Started") {
			continue;
		}
		++msgCount;
	}
	s << msgCount;
	for (const auto &m : messages) {
		if (m.type == MessageType::Call
			&& m.callMeta
			&& m.callMeta->status == "Started") {
			continue;
		}
		s << m.id << m.chatId << m.authorId
			<< m.text << m.markdown
			<< qint32(int(m.type))
			<< QJsonDocument(QJsonObject{{"b", m.blocks}}).toJson(QJsonDocument::Compact)
			<< qint32(m.files.size());
		for (const auto &f : m.files) {
			s << f.id << f.name << f.url << f.size << f.mime
				<< qint32(f.width) << qint32(f.height)
				<< f.voice << qint32(f.duration) << f.waveform;
		}
		s << qint32(m.mentions.size());
		for (const auto &mn : m.mentions) {
			s << mn.userId << mn.name;
		}
		s << qint32(m.reactions.size());
		for (const auto &r : m.reactions) {
			s << r.emojiId << r.emoji << qint32(r.count) << r.selected;
		}
		s << m.createdAt << m.updatedAt << m.isDeleted
			<< m.repliedMessageId << m.parentId
			<< qint32(m.threadChildrenCount)
			<< qint32(m.threadUnreadCount)
			<< m.forward.has_value();
		if (m.forward) {
			s << m.forward->authorId << m.forward->messageId
				<< m.forward->chatId << m.forward->createdAt;
		}
		s << m.callMeta.has_value();
		if (m.callMeta) {
			s << m.callMeta->status << m.callMeta->joinLink
				<< qint32(m.callMeta->duration)
				<< m.callMeta->statusReason;
		}
	}
	s << qint32(profiles.size());
	for (const auto &p : profiles) {
		s << p.userId << p.organizationId << p.email
			<< p.phone << p.position << p.department
			<< p.firstName << p.lastName << p.displayName
			<< qint32(int(p.presence))
			<< p.avatarFileId << qint32(int(p.role));
	}
	return result;
}

struct CachedMessages {
	QList<Api::MessageData> messages;
	QList<Api::MemberProfile> profiles;
};

std::optional<CachedMessages> deserializeMessages(const QByteArray &data) {
	if (data.isEmpty()) {
		return std::nullopt;
	}
	QDataStream s(data);
	s.setVersion(QDataStream::Qt_5_1);

	qint32 version = 0;
	s >> version;
	// version 2 adds callMeta, version 3 adds callMeta statusReason,
	// version 4 adds the voice fields of the files
	if (version < 1 || version > 4) {
		return std::nullopt;
	}

	qint32 msgCount = 0;
	s >> msgCount;
	if (s.status() != QDataStream::Ok || msgCount < 0) {
		return std::nullopt;
	}

	CachedMessages result;
	result.messages.reserve(msgCount);
	for (int i = 0; i < msgCount; ++i) {
		Api::MessageData m;
		qint32 typeInt = 0, filesCount = 0, mentionsCount = 0,
			reactionsCount = 0, threadChildren = 0, threadUnread = 0;
		bool hasForward = false;
		QByteArray blocksJson;

		s >> m.id >> m.chatId >> m.authorId
			>> m.text >> m.markdown >> typeInt
			>> blocksJson >> filesCount;
		m.type = MessageType(typeInt);
		{
			const auto doc = QJsonDocument::fromJson(blocksJson);
			m.blocks = doc.object().value("b").toArray();
		}
		for (int fi = 0; fi < filesCount; ++fi) {
			Api::FileData f;
			qint32 w = 0, h = 0;
			s >> f.id >> f.name >> f.url >> f.size >> f.mime >> w >> h;
			f.width = w;
			f.height = h;
			if (version >= 4) {
				qint32 duration = 0;
				s >> f.voice >> duration >> f.waveform;
				f.duration = duration;
			}
			m.files.push_back(std::move(f));
		}
		s >> mentionsCount;
		for (int mi = 0; mi < mentionsCount; ++mi) {
			Api::MentionInfo mn;
			s >> mn.userId >> mn.name;
			m.mentions.push_back(std::move(mn));
		}
		s >> reactionsCount;
		for (int ri = 0; ri < reactionsCount; ++ri) {
			Api::ReactionData r;
			qint32 cnt = 0;
			s >> r.emojiId >> r.emoji >> cnt >> r.selected;
			r.count = cnt;
			m.reactions.push_back(std::move(r));
		}
		s >> m.createdAt >> m.updatedAt >> m.isDeleted
			>> m.repliedMessageId >> m.parentId
			>> threadChildren >> threadUnread >> hasForward;
		m.threadChildrenCount = threadChildren;
		m.threadUnreadCount = threadUnread;
		if (hasForward) {
			Api::ForwardInfo fwd;
			s >> fwd.authorId >> fwd.messageId
				>> fwd.chatId >> fwd.createdAt;
			m.forward = std::move(fwd);
		}
		if (version >= 2) {
			bool hasCallMeta = false;
			s >> hasCallMeta;
			if (hasCallMeta) {
				Api::CallMetadata cm;
				qint32 dur = 0;
				s >> cm.status >> cm.joinLink >> dur;
				cm.duration = dur;
				if (version >= 3) {
					s >> cm.statusReason;
				}
				m.callMeta = std::move(cm);
			}
		}
		if (s.status() != QDataStream::Ok) {
			return std::nullopt;
		}
		result.messages.push_back(std::move(m));
	}

	qint32 profCount = 0;
	s >> profCount;
	result.profiles.reserve(profCount);
	for (int i = 0; i < profCount; ++i) {
		Api::MemberProfile p;
		qint32 presInt = 0, roleInt = 0;
		s >> p.userId >> p.organizationId >> p.email
			>> p.phone >> p.position >> p.department
			>> p.firstName >> p.lastName >> p.displayName
			>> presInt >> p.avatarFileId >> roleInt;
		// A cached presence is stale, the server reports the current one.
		Q_UNUSED(presInt);
		p.presence = MemberPresence::Unknown;
		p.role = MemberRole(roleInt);
		if (s.status() != QDataStream::Ok) {
			return std::nullopt;
		}
		result.profiles.push_back(std::move(p));
	}
	return result;
}

[[nodiscard]] Storage::Cache::Key threadCacheKey(
		const QString &chatId,
		const QString &parentId) {
	const auto hash = QCryptographicHash::hash(
		(chatId + ':' + parentId).toUtf8(),
		QCryptographicHash::Md5);
	uint64 low = 0;
	memcpy(&low, hash.constData(), sizeof(low));
	return { kMtsLinkThreadCacheTag, low };
}

} // namespace

void saveThreadToCache(
		not_null<Main::Session*> session,
		const QString &chatId,
		const QString &parentId,
		const QList<Api::MessageData> &messages,
		const QList<Api::MemberProfile> &profiles) {
	if (chatId.isEmpty() || parentId.isEmpty()) {
		return;
	}
	session->data().cache().put(
		threadCacheKey(chatId, parentId),
		serializeMessages(messages, profiles));
}

void loadThreadFromCache(
		not_null<Main::Session*> session,
		const QString &chatId,
		const QString &parentId,
		Fn<void(
			QList<Api::MessageData> messages,
			QList<Api::MemberProfile> profiles)> done) {
	if (chatId.isEmpty() || parentId.isEmpty()) {
		return;
	}
	const auto weak = base::make_weak(session);
	session->data().cache().get(threadCacheKey(chatId, parentId), [=](
			QByteArray &&data) {
		crl::on_main(weak, [=, data = std::move(data)] {
			if (const auto cached = deserializeMessages(data)) {
				if (!cached->messages.isEmpty()) {
					done(cached->messages, cached->profiles);
				}
			}
		});
	});
}

namespace {

constexpr auto kMtsLinkChatListTag = uint64(0xBC02'0000'0000'0000ULL);

Storage::Cache::Key chatListCacheKey() {
	return { kMtsLinkChatListTag, 0 };
}

QByteArray serializeChatList(const QList<Api::ChannelData> &channels) {
	QByteArray result;
	QDataStream s(&result, QIODevice::WriteOnly);
	s.setVersion(QDataStream::Qt_5_1);

	s << qint32(1); // format version
	s << qint32(channels.size());
	for (const auto &ch : channels) {
		s << ch.id << ch.name << ch.description
			<< qint32(int(ch.type))
			<< ch.organizationId
			<< qint32(ch.unreadCount)
			<< ch.lastMessageId << ch.lastMessageText
			<< ch.lastMessageTimestamp
			<< ch.avatarFileId
			<< ch.isMuted << ch.isPinned << ch.isReadOnly
			<< qint32(ch.pinnedMessageCount)
			<< ch.memberRole << ch.interlocutorId
			<< qint32(ch.memberCount);
	}
	return result;
}

std::optional<QList<Api::ChannelData>> deserializeChatList(
		const QByteArray &data) {
	if (data.isEmpty()) {
		return std::nullopt;
	}
	QDataStream s(data);
	s.setVersion(QDataStream::Qt_5_1);

	qint32 version = 0;
	s >> version;
	if (version != 1) {
		return std::nullopt;
	}

	qint32 count = 0;
	s >> count;
	if (s.status() != QDataStream::Ok || count < 0) {
		return std::nullopt;
	}

	QList<Api::ChannelData> result;
	result.reserve(count);
	for (int i = 0; i < count; ++i) {
		Api::ChannelData ch;
		qint32 typeInt = 0, unread = 0, pinned = 0, members = 0;
		s >> ch.id >> ch.name >> ch.description
			>> typeInt >> ch.organizationId
			>> unread >> ch.lastMessageId >> ch.lastMessageText
			>> ch.lastMessageTimestamp >> ch.avatarFileId
			>> ch.isMuted >> ch.isPinned >> ch.isReadOnly
			>> pinned >> ch.memberRole >> ch.interlocutorId
			>> members;
		ch.type = ChatType(typeInt);
		ch.unreadCount = unread;
		ch.pinnedMessageCount = pinned;
		ch.memberCount = members;
		if (s.status() != QDataStream::Ok) {
			return std::nullopt;
		}
		result.push_back(std::move(ch));
	}
	return result;
}

constexpr auto kMtsLinkFiltersTag = uint64(0xBC03'0000'0000'0000ULL);

Storage::Cache::Key filtersCacheKey() {
	return { kMtsLinkFiltersTag, 0 };
}

QByteArray serializeFilters(
		const std::vector<Data::ChatFilter> &filters) {
	QByteArray result;
	QDataStream s(&result, QIODevice::WriteOnly);
	s.setVersion(QDataStream::Qt_5_1);

	s << qint32(1); // format version
	s << qint32(int(filters.size()));
	for (const auto &f : filters) {
		s << qint32(f.id())
			<< f.titleText().text
			<< f.iconEmoji()
			<< qint32(f.colorIndex().value_or(-1))
			<< quint16(f.flags().value());
	}
	return result;
}

struct CachedFilter {
	FilterId id = 0;
	QString title;
	QString iconEmoji;
	std::optional<uint8> colorIndex;
	Data::ChatFilter::Flags flags;
};

std::optional<std::vector<CachedFilter>> deserializeFilters(
		const QByteArray &data) {
	if (data.isEmpty()) {
		return std::nullopt;
	}
	QDataStream s(data);
	s.setVersion(QDataStream::Qt_5_1);

	qint32 version = 0;
	s >> version;
	if (version != 1) {
		return std::nullopt;
	}

	qint32 count = 0;
	s >> count;
	if (s.status() != QDataStream::Ok || count < 0) {
		return std::nullopt;
	}

	std::vector<CachedFilter> result;
	result.reserve(count);
	for (int i = 0; i < count; ++i) {
		CachedFilter f;
		qint32 id = 0, colorIdx = 0;
		quint16 flags = 0;
		s >> id >> f.title >> f.iconEmoji >> colorIdx >> flags;
		if (s.status() != QDataStream::Ok) {
			return std::nullopt;
		}
		f.id = FilterId(id);
		f.colorIndex = (colorIdx >= 0)
			? std::make_optional(uint8(colorIdx))
			: std::nullopt;
		f.flags = Data::ChatFilter::Flags::from_raw(flags);
		result.push_back(std::move(f));
	}
	return result;
}

} // anonymous namespace

namespace {

constexpr auto kMtsLinkMaterialsCacheTag = uint64(0xBC05'0000'0000'0000ULL);

[[nodiscard]] Storage::Cache::Key callMaterialsCacheKey(
		const QString &eventId) {
	const auto hash = QCryptographicHash::hash(
		eventId.toUtf8(),
		QCryptographicHash::Md5);
	uint64 low = 0;
	memcpy(&low, hash.constData(), sizeof(low));
	return { kMtsLinkMaterialsCacheTag, low };
}

} // namespace

void saveCallMaterialsToCache(
		not_null<Main::Session*> session,
		const QString &eventId,
		bool done,
		const QList<Api::MessageData> &messages) {
	// The "done" flag and the messages as of a chat.
	auto data = QByteArray();
	{
		QDataStream s(&data, QIODevice::WriteOnly);
		s.setVersion(QDataStream::Qt_5_1);
		s << qint32(1) << done << serializeMessages(messages, {});
	}
	session->data().cache().put(
		callMaterialsCacheKey(eventId),
		std::move(data));
}

void loadCallMaterialsFromCache(
		not_null<Main::Session*> session,
		const QString &eventId,
		Fn<void(bool done, QList<Api::MessageData> messages)> done) {
	const auto weak = base::make_weak(session);
	session->data().cache().get(callMaterialsCacheKey(eventId), [=](
			QByteArray &&data) {
		crl::on_main(weak, [=, data = std::move(data)] {
			auto finished = false;
			auto messages = QList<Api::MessageData>();
			if (!data.isEmpty()) {
				QDataStream s(data);
				s.setVersion(QDataStream::Qt_5_1);
				auto version = qint32();
				auto serialized = QByteArray();
				s >> version >> finished >> serialized;
				if (s.status() == QDataStream::Ok && version == 1) {
					if (const auto cached = deserializeMessages(serialized)) {
						messages = cached->messages;
					}
				} else {
					finished = false;
				}
			}
			done(finished, std::move(messages));
		});
	});
}

void saveMessagesToCache(
		not_null<Main::Session*> session,
		const QString &chatId,
		const QList<Api::MessageData> &messages,
		const QList<Api::MemberProfile> &profiles) {
	auto data = serializeMessages(messages, profiles);
	session->data().cache().put(
		messageCacheKey(chatId),
		std::move(data));
}

void loadMessagesFromCache(
		not_null<Main::Session*> session,
		const QString &chatId) {
	const auto weak = base::make_weak(session);
	session->data().cache().get(messageCacheKey(chatId), [=](QByteArray &&data) {
		crl::on_main(weak, [=, data = std::move(data)]() mutable {
			auto cached = deserializeMessages(data);
			if (!cached || cached->messages.isEmpty()) {
				return;
			}
			const auto strong = weak.get();
			const auto oldestId = cached->messages.last().id;
			(void)addOlderMessages(
				strong,
				chatId,
				cached->messages,
				cached->profiles,
				oldestId,
				cached->messages.size());
		});
	});
}

void saveChatListToCache(
		not_null<Main::Session*> session,
		const QList<Api::ChannelData> &channels) {
	auto data = serializeChatList(channels);
	session->data().cache().put(
		chatListCacheKey(),
		std::move(data));
}

void loadChatListFromCache(
		not_null<Main::Session*> session) {
	const auto weak = base::make_weak(session);
	session->data().cache().get(chatListCacheKey(), [=](QByteArray &&data) {
		crl::on_main(weak, [=, data = std::move(data)]() mutable {
			auto cached = deserializeChatList(data);
			if (!cached || cached->isEmpty()) {
				return;
			}
			const auto strong = weak.get();
			LOG(("MtsLink Cache: loaded %1 chats from cache")
				.arg(cached->size()));
			applyChatList(strong, *cached);
		});
	});
}

// Default folders keep the title of the language they were created with,
// a not renamed one is shown in the current language.
[[nodiscard]] QString defaultFolderTitle(
		FilterId id,
		Data::ChatFilter::Flags flags,
		const QString &title) {
	using Flag = Data::ChatFilter::Flag;
	struct Default {
		FilterId id = 0;
		Flag flag = Flag();
		QStringList known;
		QString current;
	};
	const auto defaults = std::array<Default, 5>{ {
		{ 1, Flag::NoRead, { u"Unread"_q, u"Новые"_q, u"Непрочитанные"_q },
			tr::lng_filters_name_unread(tr::now) },
		{ 2, Flag::Contacts, { u"People"_q, u"Люди"_q, u"Личные"_q },
			tr::lng_filters_name_people(tr::now) },
		{ 3, Flag::Groups, { u"Groups"_q, u"Группы"_q },
			tr::lng_filters_type_groups(tr::now) },
		{ 4, Flag::Channels, { u"Channels"_q, u"Каналы"_q },
			tr::lng_filters_type_channels(tr::now) },
		{ 5, Flag::Threads, { u"Threads"_q, u"Обсуждения"_q, u"Треды"_q },
			tr::lng_threads_folder(tr::now) },
	} };
	for (const auto &entry : defaults) {
		if (entry.id == id
			&& (flags & entry.flag)
			&& entry.known.contains(title)) {
			return entry.current;
		}
	}
	return title;
}

void saveFiltersToCache(not_null<Main::Session*> session) {
	const auto &filters = session->data().chatsFilters().list();
	auto data = serializeFilters(filters);
	session->data().cache().put(filtersCacheKey(), std::move(data));
}

void loadFiltersFromCache(
		not_null<Main::Session*> session,
		Fn<void(bool loaded)> done) {
	const auto weak = base::make_weak(session);
	session->data().cache().get(filtersCacheKey(), [=](QByteArray &&data) {
		crl::on_main(weak, [=, data = std::move(data)]() mutable {
			auto cached = deserializeFilters(data);
			if (!cached || cached->empty()) {
				done(false);
				return;
			}
			const auto strong = weak.get();
			auto &chatFilters = strong->data().chatsFilters();
			for (const auto &f : *cached) {
				if (!f.id) {
					continue;
				}
				chatFilters.set(Data::ChatFilter(
					f.id,
					{ { defaultFolderTitle(f.id, f.flags, f.title) } },
					f.iconEmoji,
					f.colorIndex,
					f.flags,
					{}, {}, {}));
			}
			auto order = std::vector<FilterId>();
			order.reserve(cached->size());
			for (const auto &f : *cached) {
				order.push_back(f.id);
			}
			chatFilters.saveOrder(order);
			done(true);
		});
	});
}

void setFileAuthToken(const QString &token) {
	FileAuthTokenValue = token;
}

QString fileAuthToken() {
	return FileAuthTokenValue;
}

void setFileRefreshToken(const QString &token) {
	FileRefreshTokenValue = token;
}

QString fileRefreshToken() {
	return FileRefreshTokenValue;
}

void setFileAuthCookies(const QList<QNetworkCookie> &cookies) {
	FileAuthCookies = cookies;
	for (const auto &cookie : cookies) {
		LOG(("MtsLink Auth: set cookie %1 domain=%2").arg(
			QString::fromLatin1(cookie.name()), cookie.domain()));
		if (cookie.name() == "refresh") {
			FileRefreshTokenValue = QString::fromUtf8(cookie.value());
			LOG(("MtsLink: extracted refresh token from cookie"));
		}
	}
	LOG(("MtsLink: stored %1 auth cookies").arg(cookies.size()));
}

QList<QNetworkCookie> fileAuthCookies() {
	return FileAuthCookies;
}

void setTokenRefreshCallback(std::function<void()> callback) {
	TokenRefreshCallback = std::move(callback);
}

void requestTokenRefresh() {
	if (TokenRefreshInProgress) {
		return;
	}
	TokenRefreshInProgress = true;
	LOG(("MtsLink: requesting token refresh (CDN auth expired)"));
	if (TokenRefreshCallback) {
		TokenRefreshCallback();
	}
}

QSet<ChatId> MessageCacheLoadedChats;

void applyChatList(
		not_null<Main::Session*> session,
		const QList<Api::ChannelData> &channels) {
	const auto isDialogType = [](const Api::ChannelData &ch) {
		return ch.type == ChatType::Dialog
			|| ch.type == ChatType::Favorites;
	};
	for (const auto &ch : channels) {
		if (isDialogType(ch) && (ch.isPinned || ch.type == ChatType::Favorites)) {
			applyDialogData(session, ch);
		}
	}
	for (const auto &ch : channels) {
		if (isDialogType(ch) && (ch.isPinned || ch.type == ChatType::Favorites)) {
			continue;
		}
		if (isDialogType(ch)) {
			applyDialogData(session, ch);
		} else {
			applyChannelData(session, ch);
		}
	}
	session->data().chatsList()->setLoaded();
	scheduleFolderPinsSync(session);
	for (const auto &ch : channels) {
		if (!MessageCacheLoadedChats.contains(ch.id)) {
			MessageCacheLoadedChats.insert(ch.id);
			loadMessagesFromCache(session, ch.id);
		}
	}
	if (!ActiveChatRestored && DialogsApplied) {
		ActiveChatRestored = true;
		const auto savedChatId =
			session->account().local().readMtsLinkActiveChat();
		if (!savedChatId.isEmpty()) {
			const auto peerId = chatIdToPeerId(savedChatId);
			if (peerId && session->data().historyLoaded(peerId)) {
				const auto &windows = session->windows();
				if (!windows.empty()) {
					windows.front()->showPeerHistory(peerId);
				}
			}
		}
	}
}

QString userBareIdToUuid(uint64 bareId) {
	return UserBareIdToUuidMap.value(bareId);
}

MtsLinkMessageContent convertMentionsForSending(
		const TextWithTags &textWithTags,
		not_null<Main::Session*> session) {
	const auto &text = textWithTags.text;
	const auto &tags = textWithTags.tags;

	static const auto styleMap = QHash<QString, QString>{
		{u"**"_q, u"bold"_q},
		{u"__"_q, u"italic"_q},
		{u"~~"_q, u"strike"_q},
	};
	static const auto mdMap = QHash<QString, QString>{
		{u"**"_q, u"**"_q},
		{u"__"_q, u"*"_q},
		{u"^^"_q, u"__"_q},
		{u"~~"_q, u"~~"_q},
		{u"`"_q, u"`"_q},
		{u"||"_q, u"||"_q},
	};

	const auto mts = session->account().mtsLinkSession();
	const auto orgId = mts ? mts->organizationId() : QString();

	enum class HitType { Mention, Link };
	struct TagHit {
		int offset = 0;
		int length = 0;
		HitType type = HitType::Mention;
		QString uuid;
		QString displayName;
		QString url;
	};
	QList<TagHit> hits;
	QJsonArray mentionsMeta;
	QSet<QString> addedMentions;

	struct StyleRange {
		int offset, length;
		QString name;
	};
	QList<StyleRange> styleRanges;

	QMap<int, QString> allMdMarkers;
	QMap<int, QString> blockMdMarkers;

	struct BlockquoteRange { int offset, length; };
	QList<BlockquoteRange> blockquoteRanges;

	for (const auto &tag : tags) {
		if (TextUtilities::IsMentionLink(tag.id)) {
			const auto data = TextUtilities::MentionEntityData(tag.id);
			if (data.isEmpty()) continue;
			const auto fields = TextUtilities::MentionNameDataToFields(data);
			const auto uuid = userBareIdToUuid(fields.userId);
			if (uuid.isEmpty()) continue;
			TagHit hit;
			hit.offset = tag.offset;
			hit.length = tag.length;
			hit.type = HitType::Mention;
			hit.uuid = uuid;
			hit.displayName = text.mid(tag.offset, tag.length);
			hits.push_back(std::move(hit));
			if (!addedMentions.contains(uuid)) {
				addedMentions.insert(uuid);
				mentionsMeta.append(QJsonObject{
					{u"id"_q, uuid},
					{u"type"_q, u"User"_q},
					{u"name"_q, text.mid(tag.offset, tag.length)},
				});
			}
		} else if (!tag.id.isEmpty()
			&& (tag.id.contains(u"://"_q) || tag.id.contains('.'))) {
			TagHit hit;
			hit.offset = tag.offset;
			hit.length = tag.length;
			hit.type = HitType::Link;
			hit.url = tag.id;
			hits.push_back(std::move(hit));
		} else if (tag.id == u">"_q || tag.id == u">^"_q) {
			blockquoteRanges.push_back({tag.offset, tag.length});
		} else {
			if (styleMap.contains(tag.id)) {
				styleRanges.push_back({
					tag.offset, tag.length, styleMap[tag.id]});
			}
			if (tag.id.startsWith(u"```"_q)) {
				const auto lang = tag.id.mid(3);
				const auto open = u"```"_q + lang + u"\n"_q;
				const auto close = u"\n```"_q;
				allMdMarkers[tag.offset] += open;
				allMdMarkers[tag.offset + tag.length] += close;
				blockMdMarkers[tag.offset] += open;
				blockMdMarkers[tag.offset + tag.length] += close;
			} else {
				const auto mit = mdMap.constFind(tag.id);
				if (mit != mdMap.constEnd()) {
					allMdMarkers[tag.offset] += *mit;
					allMdMarkers[tag.offset + tag.length] += *mit;
					if (!styleMap.contains(tag.id)) {
						blockMdMarkers[tag.offset] += *mit;
						blockMdMarkers[tag.offset + tag.length] += *mit;
					}
				}
			}
		}
	}

	{
		auto parsed = TextWithEntities{ text };
		TextUtilities::ParseEntities(parsed, TextParseLinks);
		for (const auto &entity : parsed.entities) {
			if (entity.type() != EntityType::Url) {
				continue;
			}
			const auto eo = entity.offset();
			const auto el = entity.length();
			bool overlaps = false;
			for (const auto &h : hits) {
				if (eo < h.offset + h.length && eo + el > h.offset) {
					overlaps = true;
					break;
				}
			}
			if (!overlaps) {
				auto url = text.mid(eo, el);
				if (!url.startsWith(u"http://"_q, Qt::CaseInsensitive)
					&& !url.startsWith(u"https://"_q, Qt::CaseInsensitive)) {
					url = u"https://"_q + url;
				}
				TagHit hit;
				hit.offset = eo;
				hit.length = el;
				hit.type = HitType::Link;
				hit.url = url;
				hits.push_back(std::move(hit));
			}
		}
	}

	if (hits.isEmpty() && styleRanges.isEmpty()
		&& allMdMarkers.isEmpty() && blockquoteRanges.isEmpty()) {
		MtsLinkMessageContent plain;
		plain.text = text;
		return plain;
	}

	std::sort(hits.begin(), hits.end(),
		[](const auto &a, const auto &b) { return a.offset < b.offset; });

	const auto insertMarkers = [&](
			const QMap<int, QString> &map,
			int from, int to) -> QString {
		QString segment;
		int p = from;
		for (auto it = map.lowerBound(from);
			it != map.end() && it.key() <= to; ++it) {
			if (it.key() > p) {
				segment += text.mid(p, it.key() - p);
			}
			segment += it.value();
			p = it.key();
		}
		if (to > p) {
			segment += text.mid(p, to - p);
		}
		return segment;
	};

	// Build markdown text field.
	QString mtsText;
	{
		int pos = 0;
		for (const auto &hit : hits) {
			mtsText += insertMarkers(allMdMarkers, pos, hit.offset);
			if (hit.type == HitType::Mention) {
				mtsText += u"<@u:"_q + hit.uuid + u">"_q;
			} else {
				const auto lt = text.mid(hit.offset, hit.length);
				mtsText += u"["_q + lt + u"]("_q + hit.url + u")"_q;
			}
			pos = hit.offset + hit.length;
		}
		mtsText += insertMarkers(allMdMarkers, pos, text.size());
	}

	if (!blockquoteRanges.isEmpty()) {
		QSet<int> quotedLines;
		for (const auto &bq : blockquoteRanges) {
			const auto startLine = text.left(bq.offset).count('\n');
			const auto endLine = text.left(bq.offset + bq.length).count('\n');
			for (int l = startLine; l <= endLine; ++l) {
				quotedLines.insert(l);
			}
		}
		auto lines = mtsText.split('\n');
		for (int i = 0; i < lines.size(); ++i) {
			if (quotedLines.contains(i)) {
				lines[i] = u"> "_q + lines[i];
			}
		}
		mtsText = lines.join('\n');
	}

	// Build blocks: split text by style and special boundaries.
	QSet<int> splitSet;
	splitSet.insert(0);
	splitSet.insert(text.size());
	for (const auto &sr : styleRanges) {
		splitSet.insert(sr.offset);
		splitSet.insert(sr.offset + sr.length);
	}
	for (const auto &h : hits) {
		splitSet.insert(h.offset);
		splitSet.insert(h.offset + h.length);
	}
	// The quoted lines start with "> " in the blocks as well, the other
	// clients show the blocks.
	QSet<int> quotedLineStarts;
	for (const auto &bq : blockquoteRanges) {
		// From -1 lastIndexOf searches from the end.
		auto start = (bq.offset > 0)
			? (text.lastIndexOf('\n', bq.offset - 1) + 1)
			: 0;
		const auto end = bq.offset + bq.length;
		while (start <= end && start < text.size()) {
			quotedLineStarts.insert(start);
			splitSet.insert(start);
			const auto next = text.indexOf('\n', start);
			if (next < 0) {
				break;
			}
			start = next + 1;
		}
	}
	QJsonArray blocks;
	const auto quotePrefix = [&](int from) {
		if (quotedLineStarts.contains(from)) {
			blocks.append(QJsonObject{
				{u"type"_q, u"TextElement"_q},
				{u"value"_q, QJsonObject{ { u"text"_q, u"> "_q } }},
			});
		}
	};
	auto splits = splitSet.values();
	std::sort(splits.begin(), splits.end());

	for (int si = 0; si + 1 < splits.size(); ++si) {
		const auto from = splits[si];
		const auto to = splits[si + 1];
		if (from >= to) continue;

		const TagHit *inHit = nullptr;
		for (const auto &h : hits) {
			if (from >= h.offset && from < h.offset + h.length) {
				inHit = &h;
				break;
			}
		}

		if (inHit) {
			if (from != inHit->offset) continue;
			quotePrefix(from);
			if (inHit->type == HitType::Mention) {
				blocks.append(QJsonObject{
					{u"type"_q, u"MentionElement"_q},
					{u"value"_q, QJsonObject{
						{u"id"_q, inHit->uuid},
						{u"type"_q, u"User"_q},
						{u"organizationId"_q, orgId},
					}},
				});
			} else {
				const auto lt = text.mid(
					inHit->offset, inHit->length);
				blocks.append(QJsonObject{
					{u"type"_q, u"LinkElement"_q},
					{u"value"_q, QJsonObject{
						{u"elements"_q, QJsonArray{QJsonObject{
							{u"type"_q, u"TextElement"_q},
							{u"value"_q, QJsonObject{
								{u"text"_q, lt}}},
						}}},
						{u"url"_q, inHit->url},
					}},
				});
			}
			continue;
		}

		QJsonObject style;
		for (const auto &sr : styleRanges) {
			if (sr.offset <= from && from < sr.offset + sr.length) {
				style[sr.name] = true;
			}
		}

		const auto segText = insertMarkers(blockMdMarkers, from, to);
		if (segText.isEmpty()) continue;

		QJsonObject value;
		value[u"text"_q] = (quotedLineStarts.contains(from)
			? u"> "_q
			: QString()) + segText;
		if (!style.isEmpty()) {
			value[u"style"_q] = style;
		}
		blocks.append(QJsonObject{
			{u"type"_q, u"TextElement"_q},
			{u"value"_q, value},
		});
	}

	MtsLinkMessageContent result;
	result.text = mtsText;
	result.blocks = blocks;
	result.mentionsMeta = mentionsMeta;
	return result;
}

void setEmojiMapping(const QHash<QString, QString> &emojiToId) {
	ensureEmojiMapsInitialized();
	for (auto it = emojiToId.constBegin(); it != emojiToId.constEnd(); ++it) {
		EmojiToIdMap[it.key()] = it.value();
		IdToEmojiMap[it.value()] = it.key();
	}
}

void setEmojiIdMapping(const QString &emojiId, const QString &emoji) {
	ensureEmojiMapsInitialized();
	if (!emojiId.isEmpty() && !emoji.isEmpty()
		&& !IdToEmojiMap.contains(emojiId)) {
		IdToEmojiMap[emojiId] = emoji;
		EmojiToIdMap[emoji] = emojiId;
	}
}

QString emojiToId(const QString &emoji) {
	ensureEmojiMapsInitialized();
	const auto result = EmojiToIdMap.value(emoji);
	if (!result.isEmpty()) {
		return result;
	}
	auto stripped = emoji;
	stripped.remove(QChar(0xFE0F));
	return EmojiToIdMap.value(stripped);
}

QString idToEmoji(const QString &emojiId) {
	ensureEmojiMapsInitialized();
	return IdToEmojiMap.value(emojiId);
}

std::vector<not_null<UserData*>> chatMtsLinkUsers(
		not_null<Main::Session*> session,
		PeerId chatPeerId) {
	auto result = std::vector<not_null<UserData*>>();
	const auto it = ChatMembersMap.constFind(chatPeerId);
	if (it == ChatMembersMap.constEnd()) {
		return result;
	}
	const auto &members = it.value();
	result.reserve(members.size());
	for (const auto bareId : members) {
		if (const auto user = session->data().userLoaded(
				::UserId(bareId))) {
			result.push_back(user);
		}
	}
	std::sort(result.begin(), result.end(),
		[](not_null<UserData*> a, not_null<UserData*> b) {
			return a->name().compare(b->name(), Qt::CaseInsensitive) < 0;
		});
	return result;
}

bool isMtsLinkUrl(const QString &url) {
	const auto lower = url.toLower();
	return lower.contains(u"mts-link.ru/"_q)
		|| lower.contains(u"webinar.ru/"_q);
}

namespace {

void navigateToChat(
		const QString &chatId,
		const QString &threadId,
		const QString &messageId,
		const QVariant &context) {
	LOG(("MtsLink Navigate: navigateToChat chatId='%1' threadId='%2' messageId='%3'")
		.arg(chatId).arg(threadId).arg(messageId));
	const auto peerId = chatIdToPeerId(chatId);
	if (!peerId) {
		LOG(("MtsLink Navigate: peerId is 0, abort"));
		return;
	}
	const auto my = context.value<ClickHandlerContext>();
	const auto controller = my.sessionWindow.get();
	if (!controller) {
		LOG(("MtsLink Navigate: controller is null, abort"));
		return;
	}

	const auto way = Window::SectionShow::Way::ClearStack;
	if (!messageId.isEmpty()) {
		const auto bareId = uuidToBareId(messageId);
		const auto msgId = MsgId(bareId & 0x7FFFFFFFLL);
		const auto item = controller->session().data().message(
			peerId, msgId);
		if (item) {
			controller->showPeerHistory(peerId, way, msgId);
		} else {
			const auto mts = controller->session().account().mtsLinkSession();
			if (mts) {
				const auto weak = my.sessionWindow;
				const auto conn = std::make_shared<QMetaObject::Connection>();
				*conn = QObject::connect(
					mts->messages(),
					&Api::Messages::aroundMessagesLoaded,
					[weak, peerId, msgId, chatId, messageId, conn](
							const ChatId &cid,
							const MessageId &targetId,
							const QList<Api::MessageData> &messages,
							const QList<Api::MemberProfile> &profiles) {
						if (cid != chatId || targetId != messageId) {
							return;
						}
						QObject::disconnect(*conn);
						const auto ctrl = weak.get();
						if (!ctrl) {
							return;
						}
						for (const auto &p : profiles) {
							applyUserData(&ctrl->session(), p);
						}
						for (const auto &src : messages) {
							addMessage(&ctrl->session(), src);
						}
						const auto w = Window::SectionShow::Way::ClearStack;
						const auto loaded = ctrl->session().data().message(
							peerId, msgId);
						if (loaded) {
							ctrl->showPeerHistory(peerId, w, msgId);
						} else {
							ctrl->showPeerHistory(peerId, w);
						}
					});
				mts->messages()->loadAround(chatId, messageId, 50);
			} else {
				controller->showPeerHistory(peerId, way);
			}
		}
	} else {
		controller->showPeerHistory(peerId, way);
	}
}

bool tryNavigateDirectUrl(
		const QUrl &parsed,
		const QVariant &context) {
	const auto path = parsed.path();
	LOG(("MtsLink Navigate: tryNavigateDirectUrl path='%1'").arg(path));
	static const auto re = QRegularExpression(
		u"^/chats/(?:channel|group|direct|dialog)/([0-9a-f-]+)"
		"(?:/thread/([0-9a-f-]+))?"
		"(?:/message/([0-9a-f-]+))?$"_q);
	const auto match = re.match(path);
	if (!match.hasMatch()) {
		LOG(("MtsLink Navigate: regex no match"));
		return false;
	}
	const auto chatId = match.captured(1);
	const auto threadOrMsg = match.captured(2);
	const auto msgInThread = match.captured(3);
	const auto targetMsg = !msgInThread.isEmpty()
		? msgInThread
		: threadOrMsg;
	LOG(("MtsLink Navigate: chatId='%1' targetMsg='%2'")
		.arg(chatId).arg(targetMsg));
	navigateToChat(chatId, QString(), targetMsg, context);
	return true;
}

} // namespace

// Conference links (https://my.mts-link.ru/j/MTC/<id>...) are opened in
// the call window or the system browser, as set in the settings.
[[nodiscard]] bool tryOpenConferenceUrl(const QUrl &original) {
	// A link in a text may come without the scheme.
	const auto url = original.host().isEmpty()
		? QUrl::fromUserInput(original.toString())
		: original;
	// A call record: in the call window, signed in.
	if (url.host().endsWith(u"mts-link.ru"_q, Qt::CaseInsensitive)
		&& url.path().contains(u"/record-new/"_q)) {
		LOG(("MtsLink Navigate: record link %1").arg(url.toString()));
		openCallLink(url.toString());
		return true;
	}
	if (!url.host().endsWith(u"mts-link.ru"_q, Qt::CaseInsensitive)
		|| !url.path().startsWith(u"/j/"_q)) {
		return false;
	}
	LOG(("MtsLink Navigate: conference link %1").arg(url.toString()));
	joinCallLink(url.toString());
	return true;
}

void handleMtsLinkUrl(
		const QString &url,
		const QVariant &context) {
	LOG(("MtsLink Navigate: handleMtsLinkUrl url='%1'").arg(url));
	const auto parsed = QUrl(url);
	const auto path = parsed.path();
	if (tryOpenConferenceUrl(parsed)
		|| tryNavigateDirectUrl(parsed, context)) {
		return;
	}
	if (!path.startsWith(u"/r/"_q)) {
		File::OpenUrl(url);
		return;
	}
	const auto my = context.value<ClickHandlerContext>();
	const auto controller = my.sessionWindow.get();
	if (!controller) {
		File::OpenUrl(url);
		return;
	}
	const auto mts = controller->session().account().mtsLinkSession();
	if (!mts) {
		File::OpenUrl(url);
		return;
	}
	const auto linkId = path.mid(3); // strip "/r/"
	QJsonObject param;
	param["linkId"] = linkId;
	LOG(("MtsLink Navigate: resolving short link /r/%1").arg(linkId));
	mts->rpc()->call(
		"Shortener.GetOrigin",
		param,
		[context, url](const QJsonObject &result) {
			LOG(("MtsLink Navigate: GetOrigin result: %1")
				.arg(QString::fromUtf8(
					QJsonDocument(result).toJson(QJsonDocument::Compact))));
			const auto origin = result.value("value").toObject()
				.value("originLink").toString();
			if (!origin.isEmpty()) {
				LOG(("MtsLink Navigate: origin='%1'").arg(origin));
				const auto originUrl = QUrl(origin);
				if (!tryOpenConferenceUrl(originUrl)
					&& !tryNavigateDirectUrl(originUrl, context)) {
					File::OpenUrl(origin);
				}
			} else {
				File::OpenUrl(url);
			}
		},
		[url](const QString &) {
			File::OpenUrl(url);
		});
}

namespace {

void performStartCall(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		CallBrowser browser,
		bool video = true) {
	static auto starting = QSet<QString>();
	const auto chatId = peerIdToChatId(peer->id);
	const auto mts = session->account().mtsLinkSession();
	const auto showError = [=] {
		if (!session->windows().empty()) {
			session->windows().front()->showToast(
				tr::lng_cant_do_this(tr::now));
		}
	};
	if (chatId.isEmpty() || !mts || !mts->channels()) {
		LOG(("MtsLink Call: can't start, chatId='%1' session=%2")
			.arg(chatId)
			.arg(mts ? 1 : 0));
		showError();
		return;
	} else if (starting.contains(chatId)) {
		return;
	} else if (const auto active = activeCallJoinLink(peer->id)
		; !active.isEmpty()) {
		LOG(("MtsLink Call: joining active call chatId=%1").arg(chatId));
		joinCallLink(active, peer->name(), browser, video);
		return;
	}
	starting.insert(chatId);
	LOG(("MtsLink Call: CreateCallV2 chatId=%1").arg(chatId));
	const auto weak = base::make_weak(session);
	mts->channels()->createCall(
		chatId,
		peer->name(),
		[=](QString joinLink) {
			starting.remove(chatId);
			LOG(("MtsLink Call: created, joinLink=%1").arg(joinLink));
			openCallLink(joinLink, peer->name(), browser, video);
		},
		[=] {
			starting.remove(chatId);
			LOG(("MtsLink Call: CreateCallV2 failed chatId=%1").arg(chatId));
			if (weak) {
				showError();
			}
		});
}

} // namespace

bool isUserInCall(PeerId userPeerId) {
	return InCallUsers.contains(userPeerId);
}

namespace {

struct UserStatusEmoji {
	QString emoji;
	TimeId expiresAt = 0;
};
QHash<PeerId, UserStatusEmoji> UserStatuses;

} // namespace

QString userStatusEmoji(PeerId userPeerId) {
	const auto i = UserStatuses.constFind(userPeerId);
	if (i == UserStatuses.constEnd()
		|| (i->expiresAt && i->expiresAt <= base::unixtime::now())) {
		return QString();
	}
	return i->emoji;
}

void applyUserStatus(
		not_null<Main::Session*> session,
		const QString &userId,
		const QJsonObject &status) {
	if (userId.isEmpty()) {
		return;
	}
	const auto peerId = PeerId(::UserId(uuidToBareId(userId)));
	const auto emoji = idToEmoji(status.value("emoji").toString());
	const auto expiresAt = TimeId(status.value("expiresAt").toDouble());
	const auto was = UserStatuses.value(peerId);
	if (was.emoji == emoji && was.expiresAt == expiresAt) {
		return;
	}
	if (emoji.isEmpty()) {
		UserStatuses.remove(peerId);
	} else {
		UserStatuses.insert(peerId, { emoji, expiresAt });
	}
	if (const auto user = session->data().userLoaded(peerToUser(peerId))) {
		// The status emoji is painted over the userpic.
		session->changes().peerUpdated(
			user,
			Data::PeerUpdate::Flag::Photo | Data::PeerUpdate::Flag::EmojiStatus);
	}
	static auto repaintScheduled = false;
	if (!repaintScheduled) {
		repaintScheduled = true;
		const auto weak = base::make_weak(session);
		base::call_delayed(100, [=] {
			repaintScheduled = false;
			if (!weak) {
				return;
			}
			for (const auto &window : weak->windows()) {
				window->widget()->update();
			}
		});
	}
}

namespace {

// Thread rows of the "Threads" folder are separate peers.
[[nodiscard]] std::pair<PeerId, MsgId> ResolveTypingTarget(
		PeerId peerId,
		MsgId rootId) {
	if (isThreadPeer(peerId)) {
		const auto [parentPeerId, threadRoot] = threadParentInfo(peerId);
		return { parentPeerId, threadRoot };
	}
	return { peerId, rootId };
}

} // namespace

void subscribeTyping(
		not_null<Main::Session*> session,
		PeerId peerId,
		MsgId rootId) {
	const auto mts = session->account().mtsLinkSession();
	if (!mts || !mts->typing()) {
		return;
	}
	const auto [chatPeerId, threadRoot] = ResolveTypingTarget(peerId, rootId);
	const auto chatId = peerIdToChatId(chatPeerId);
	if (chatId.isEmpty()) {
		return;
	}
	if (threadRoot) {
		const auto threadId = msgIdToMtsLinkId(chatPeerId, threadRoot);
		if (!threadId.isEmpty()) {
			mts->typing()->subscribeToThread(chatId, threadId);
		}
	} else {
		mts->typing()->subscribeToChat(chatId);
	}
}

void sendTyping(
		not_null<Main::Session*> session,
		PeerId peerId,
		MsgId rootId) {
	const auto mts = session->account().mtsLinkSession();
	if (!mts || !mts->typing()) {
		return;
	}
	const auto [chatPeerId, threadRoot] = ResolveTypingTarget(peerId, rootId);
	const auto chatId = peerIdToChatId(chatPeerId);
	if (chatId.isEmpty()) {
		return;
	}
	const auto threadId = threadRoot
		? msgIdToMtsLinkId(chatPeerId, threadRoot)
		: QString();
	mts->typing()->sendTyping(chatId, threadId);
}

QString myChannelRole(PeerId channelPeerId) {
	return MyChannelRoles.value(channelPeerId);
}

PeerId channelOwner(PeerId channelPeerId) {
	return ChannelOwners.value(channelPeerId);
}

bool isChannelAdmin(PeerId channelPeerId, PeerId userPeerId) {
	const auto it = ChatMembersRoles.constFind(channelPeerId);
	if (it == ChatMembersRoles.constEnd()) {
		return false;
	}
	const auto role = it->value(userPeerId, MemberRole::Member);
	return (role == MemberRole::Admin) || (role == MemberRole::Owner);
}

namespace {

void channelAction(
		not_null<Main::Session*> session,
		const QString &what,
		Fn<void(not_null<Session*>, Api::Channels::Done)> call) {
	const auto mts = session->account().mtsLinkSession();
	if (!mts || !mts->channels()) {
		return;
	}
	const auto weak = base::make_weak(session);
	LOG(("MtsLink Channel: %1").arg(what));
	call(mts, [=](bool ok) {
		LOG(("MtsLink Channel: %1 -> %2").arg(what, ok ? "ok" : "fail"));
		if (!ok && weak) {
			showMtsLinkToast(weak.get(), tr::lng_cant_do_this(tr::now));
		}
	});
}

} // namespace

void setChannelAdmin(
		not_null<Main::Session*> session,
		not_null<PeerData*> channel,
		not_null<UserData*> user,
		bool admin) {
	const auto chatId = peerIdToChatId(channel->id);
	const auto userId = userBareIdToUuid(peerToUser(user->id).bare);
	channelAction(
		session,
		(admin ? u"add admin %1 in %2"_q : u"remove admin %1 in %2"_q)
			.arg(userId, chatId),
		[=](not_null<Session*> mts, Api::Channels::Done done) {
			if (admin) {
				mts->channels()->addAdministrators(
					chatId,
					{ userId },
					std::move(done));
			} else {
				mts->channels()->removeAdministrators(
					chatId,
					{ userId },
					std::move(done));
			}
		});
}

void giveChannelOwnership(
		not_null<Main::Session*> session,
		not_null<PeerData*> channel,
		not_null<UserData*> user) {
	const auto chatId = peerIdToChatId(channel->id);
	const auto userId = userBareIdToUuid(peerToUser(user->id).bare);
	channelAction(
		session,
		u"give ownership of %1 to %2"_q.arg(chatId, userId),
		[=](not_null<Session*> mts, Api::Channels::Done done) {
			if (isGroupChat(channel->id)) {
				mts->channels()->giveGroupOwnership(
					chatId,
					userId,
					std::move(done));
			} else {
				mts->channels()->giveOwnership(
					chatId,
					userId,
					std::move(done));
			}
		});
}

void inviteChannelMembers(
		not_null<Main::Session*> session,
		not_null<PeerData*> channel,
		const std::vector<not_null<UserData*>> &users) {
	const auto chatId = peerIdToChatId(channel->id);
	auto userIds = QStringList();
	for (const auto &user : users) {
		const auto id = userBareIdToUuid(peerToUser(user->id).bare);
		if (!id.isEmpty()) {
			userIds.push_back(id);
		}
	}
	if (userIds.isEmpty()) {
		return;
	}
	channelAction(
		session,
		u"invite %1 to %2"_q.arg(userIds.join(','), chatId),
		[=](not_null<Session*> mts, Api::Channels::Done done) {
			if (isGroupChat(channel->id)) {
				mts->channels()->addGroupUsers(
					chatId,
					userIds,
					mts->organizationId(),
					std::move(done));
			} else {
				mts->channels()->addUsers(
					chatId,
					userIds,
					mts->organizationId(),
					std::move(done));
			}
		});
}

void createChannel(
		not_null<Main::Session*> session,
		const QString &title,
		const QString &description,
		bool isPublic,
		bool isReadOnly,
		QImage cover,
		Fn<void(ChannelData*)> done) {
	const auto mts = session->account().mtsLinkSession();
	if (!mts || !mts->channels()) {
		done(nullptr);
		return;
	}
	const auto weak = base::make_weak(session);
	const auto selfId = mts->userId();
	LOG(("MtsLink Channel: create '%1' public=%2 readOnly=%3"
		).arg(title).arg(isPublic ? 1 : 0).arg(isReadOnly ? 1 : 0));
	mts->channels()->createChannel(
		title,
		description,
		isPublic,
		isReadOnly,
		mts->organizationId(),
		[=](std::optional<Api::ChannelData> result) {
			if (!weak) {
				return;
			} else if (!result) {
				LOG(("MtsLink Channel: create failed"));
				showMtsLinkToast(weak.get(), tr::lng_cant_do_this(tr::now));
				done(nullptr);
				return;
			}
			auto data = *result;
			// The creator is the owner, CreateChannelV2 returns ownerID.
			data.memberRole = (data.memberRole == selfId
				|| data.memberRole.isEmpty())
				? u"Owner"_q
				: data.memberRole;
			LOG(("MtsLink Channel: created %1 role=%2"
				).arg(data.id, data.memberRole));
			applyChannelData(weak.get(), data);
			const auto peerId = chatIdToPeerId(data.id, ChatType::Channel);
			const auto channel = weak->data().channelLoaded(
				peerToChannel(peerId));
			if (const auto strong = weak->account().mtsLinkSession()) {
				strong->users()->loadChatMembers(data.id);
				if (!cover.isNull()) {
					auto bytes = QByteArray();
					QBuffer buffer(&bytes);
					buffer.open(QIODevice::WriteOnly);
					cover.save(&buffer, "PNG");
					const auto chatId = data.id;
					strong->files()->uploadAvatar(
						u"Avatar.png"_q,
						bytes,
						u"image/png"_q,
						[=](const Api::UploadResult &uploaded) {
							if (!weak) {
								return;
							}
							const auto mts = weak->account().mtsLinkSession();
							if (!mts) {
								return;
							}
							LOG(("MtsLink Channel: cover %1 uploaded for %2"
								).arg(uploaded.id, chatId));
							attachChatCover(
								weak.get(),
								chatId,
								false,
								uploaded.id);
						},
						[=](const QString &error) {
							LOG(("MtsLink Channel: cover upload failed: %1"
								).arg(error));
						});
				}
			}
			done(channel);
		});
}

void leaveChannel(
		not_null<Main::Session*> session,
		not_null<ChannelData*> channel) {
	const auto chatId = peerIdToChatId(channel->id);
	SelfLeavingChats.insert(chatId);
	const auto weak = base::make_weak(session);
	channelAction(
		session,
		u"leave %1"_q.arg(chatId),
		[=](not_null<Session*> mts, Api::Channels::Done done) {
			mts->channels()->leaveChat(chatId, [=](bool ok) {
				if (ok && weak) {
					if (const auto strong = weak->data().channelLoaded(
							peerToChannel(channel->id))) {
						applyChannelLeft(weak.get(), strong);
					}
				}
				done(ok);
			});
		});
}

void deleteChannel(
		not_null<Main::Session*> session,
		not_null<ChannelData*> channel) {
	const auto chatId = peerIdToChatId(channel->id);
	SelfLeavingChats.insert(chatId);
	const auto weak = base::make_weak(session);
	channelAction(
		session,
		u"delete %1"_q.arg(chatId),
		[=](not_null<Session*> mts, Api::Channels::Done done) {
			const auto method = isGroupChat(channel->id)
				? &Api::Channels::deleteGroupChat
				: &Api::Channels::deleteChannel;
			(mts->channels()->*method)(chatId, [=](bool ok) {
				if (ok && weak) {
					if (const auto strong = weak->data().channelLoaded(
							peerToChannel(channel->id))) {
						applyChannelLeft(weak.get(), strong);
					}
				}
				done(ok);
			});
		});
}

void searchPeersGlobal(
		not_null<Main::Session*> session,
		const QString &query,
		Fn<void(
			std::vector<not_null<PeerData*>> my,
			std::vector<not_null<PeerData*>> peers)> done) {
	const auto mts = session->account().mtsLinkSession();
	if (!mts || !mts->channels() || !mts->users()) {
		done({}, {});
		return;
	}
	struct State {
		std::vector<not_null<PeerData*>> my;
		std::vector<not_null<PeerData*>> peers;
		int waiting = 2;
	};
	const auto state = std::make_shared<State>();
	const auto weak = base::make_weak(session);
	const auto finish = [=] {
		if (--state->waiting == 0 && weak) {
			LOG(("MtsLink Search: peers '%1' my=%2 peers=%3"
				).arg(query
				).arg(state->my.size()
				).arg(state->peers.size()));
			done(std::move(state->my), std::move(state->peers));
		}
	};
	mts->channels()->searchChannels(
		query,
		mts->organizationId(),
		0,
		30,
		[=](QList<Api::ChannelData> channels) {
			if (weak) {
				for (const auto &ch : channels) {
					const auto known = ChatToPeerMap.contains(ch.id);
					const auto channel = known
						? weak->data().channelLoaded(
							peerToChannel(chatIdToPeerId(ch.id)))
						: nullptr;
					if (channel && channel->amIn()) {
						state->my.push_back(channel);
					} else if (ch.isPublic) {
						// Opened as a preview with the join button.
						auto preview = ch;
						preview.memberRole = u"ChatMemberRoleNone"_q;
						applyChannelData(weak.get(), preview);
						state->peers.push_back(weak->data().channel(
							peerToChannel(
								chatIdToPeerId(ch.id, ChatType::Channel))));
					}
				}
			}
			finish();
		});
	mts->users()->searchMembers(
		query,
		0,
		30,
		[=](QList<Api::MemberProfile> profiles) {
			if (weak) {
				const auto selfId = weak->account().mtsLinkSession()
					? weak->account().mtsLinkSession()->userId()
					: QString();
				for (const auto &profile : profiles) {
					if (profile.userId == selfId) {
						continue;
					}
					applyUserData(weak.get(), profile);
					state->peers.push_back(weak->data().user(
						::UserId(uuidToBareId(profile.userId))));
				}
			}
			finish();
		});
}

void searchMessagesGlobal(
		not_null<Main::Session*> session,
		const QString &query,
		int offset,
		int limit,
		Fn<void(std::vector<not_null<HistoryItem*>>, int, bool)> done) {
	const auto mts = session->account().mtsLinkSession();
	if (!mts || !mts->messages()) {
		done({}, 0, true);
		return;
	}
	const auto weak = base::make_weak(session);
	mts->messages()->searchGlobal(
		query,
		mts->organizationId(),
		offset,
		limit,
		[=](
				QList<Api::MessageData> messages,
				QList<Api::MemberProfile> profiles,
				int total,
				int rawCount) {
			if (!weak) {
				return;
			}
			for (const auto &profile : profiles) {
				applyUserData(weak.get(), profile);
			}
			auto items = std::vector<not_null<HistoryItem*>>();
			auto skipped = 0;
			for (const auto &message : messages) {
				// Messages of chats I'm not in can't be opened.
				if (!ChatToPeerMap.contains(message.chatId)) {
					++skipped;
					continue;
				}
				// Detached items, as Telegram search results: adding them
				// as new messages would notify and break the history.
				auto detached = std::vector<not_null<HistoryItem*>>();
				if (const auto item = addMessage(
						weak.get(),
						message,
						false,
						&detached)) {
					items.push_back(item);
				}
			}
			const auto full = (rawCount < limit)
				|| (offset + rawCount >= total);
			LOG(("MtsLink Search: messages '%1' from %2: %3 of %4, "
				"skipped %5, full=%6"
				).arg(query
				).arg(offset
				).arg(items.size()
				).arg(total
				).arg(skipped
				).arg(full ? 1 : 0));
			done(std::move(items), total, full);
		});
}

void reloadChannelMembers(
		not_null<Main::Session*> session,
		PeerId channelPeerId) {
	const auto chatId = peerIdToChatId(channelPeerId);
	if (chatId.isEmpty()) {
		return;
	}
	// The members panel is created on every chat opening, the members
	// themselves change by server events (they reload the list).
	constexpr auto kReloadTimeout = crl::time(60 * 1000);
	const auto loaded = MembersLoadedAt.value(chatId);
	if (loaded && crl::now() - loaded < kReloadTimeout) {
		return;
	}
	scheduleChannelMembersReload(session, chatId);
}

rpl::producer<bool> channelPublicValue(PeerId channelPeerId) {
	return rpl::single(
		ChannelPublic.value(channelPeerId, false)
	) | rpl::then(ChannelPublicChanges.events(
	) | rpl::filter(
		rpl::mappers::_1 == channelPeerId
	) | rpl::map([=] {
		return ChannelPublic.value(channelPeerId, false);
	}));
}

void requestChatInfo(not_null<Main::Session*> session, PeerId peerId) {
	const auto chatId = peerIdToChatId(peerId);
	const auto mts = session->account().mtsLinkSession();
	if (mts && !chatId.isEmpty()) {
		mts->channels()->loadChatInfo(chatId);
	}
}

void updateChatInfo(
		not_null<Main::Session*> session,
		not_null<ChannelData*> channel,
		const QString &title,
		const QString &description,
		bool isPublic,
		bool isReadOnly,
		QImage cover,
		Fn<void(bool ok)> done) {
	const auto mts = session->account().mtsLinkSession();
	const auto chatId = peerIdToChatId(channel->id);
	if (!mts || chatId.isEmpty()) {
		done(false);
		return;
	}
	const auto group = isGroupChat(channel->id);
	const auto weak = base::make_weak(session);
	const auto uploadCover = [=] {
		if (cover.isNull() || !weak) {
			return;
		}
		const auto mts = weak->account().mtsLinkSession();
		if (!mts) {
			return;
		}
		auto bytes = QByteArray();
		QBuffer buffer(&bytes);
		buffer.open(QIODevice::WriteOnly);
		cover.save(&buffer, "PNG");
		mts->files()->uploadAvatar(
			u"Avatar.png"_q,
			bytes,
			u"image/png"_q,
			[=](const Api::UploadResult &uploaded) {
				const auto mts = weak
					? weak->account().mtsLinkSession()
					: nullptr;
				if (!mts) {
					return;
				}
				attachChatCover(weak.get(), chatId, group, uploaded.id);
			},
			[=](const QString &error) {
				LOG(("MtsLink Edit: cover upload failed: %1").arg(error));
			});
	};
	const auto finish = [=](bool ok) {
		LOG(("MtsLink Edit: update %1 -> %2"
			).arg(chatId, ok ? "ok" : "fail"));
		if (!ok) {
			if (weak) {
				showMtsLinkToast(weak.get(), tr::lng_cant_do_this(tr::now));
			}
		} else {
			uploadCover();
		}
		done(ok);
	};
	LOG(("MtsLink Edit: update %1 name='%2' public=%3 readOnly=%4 cover=%5"
		).arg(chatId, title
		).arg(isPublic ? 1 : 0
		).arg(isReadOnly ? 1 : 0
		).arg(cover.isNull() ? 0 : 1));
	if (group) {
		mts->channels()->updateGroupChat(chatId, title, finish);
	} else {
		mts->channels()->updateChannel(
			chatId,
			title,
			description,
			isPublic,
			isReadOnly,
			finish);
	}
}

// The storage processes an uploaded cover for a few seconds, Add*Cover
// fails with "internalError" before that (the MTS Link client retries too).
void attachChatCover(
		not_null<Main::Session*> session,
		const QString &chatId,
		bool group,
		const QString &fileId,
		int attempt) {
	constexpr auto kDelays = std::array<crl::time, 6>{
		1500, 2000, 3000, 4000, 6000, 8000 };
	const auto mts = session->account().mtsLinkSession();
	if (!mts) {
		return;
	}
	const auto weak = base::make_weak(session);
	const auto applied = [=](bool ok) {
		LOG(("MtsLink Cover: %1 attempt %2 -> %3"
			).arg(chatId).arg(attempt + 1).arg(ok ? "ok" : "fail"));
		if (!weak) {
			return;
		} else if (ok) {
			if (const auto peer = weak->data().peerLoaded(
					chatIdToPeerId(chatId))) {
				applyUserpic(peer, fileId, __LINE__);
			}
		} else if (attempt < int(kDelays.size())) {
			base::call_delayed(kDelays[attempt], [=] {
				if (weak) {
					attachChatCover(
						weak.get(),
						chatId,
						group,
						fileId,
						attempt + 1);
				}
			});
		} else {
			showMtsLinkToast(weak.get(), tr::lng_cant_do_this(tr::now));
		}
	};
	if (group) {
		mts->channels()->addGroupChatCover(chatId, fileId, applied);
	} else {
		mts->channels()->addChannelCover(chatId, fileId, applied);
	}
}

bool containsEmoji(const QString &text) {
	const auto end = text.constData() + text.size();
	for (auto ch = text.constData(); ch != end; ++ch) {
		auto length = 0;
		if (Ui::Emoji::Find(ch, end, &length)) {
			return true;
		}
	}
	return false;
}

bool isNewerMessageId(const QString &a, const QString &b) {
	return uuidV6Time(a) > uuidV6Time(b);
}

bool isGroupChat(PeerId peerId) {
	return chatTypeForPeer(peerId) == ChatType::GroupChat;
}

void joinChannel(
		not_null<Main::Session*> session,
		not_null<ChannelData*> channel) {
	const auto chatId = peerIdToChatId(channel->id);
	SelfLeavingChats.remove(chatId);
	const auto weak = base::make_weak(session);
	channelAction(
		session,
		u"join %1"_q.arg(chatId),
		[=](not_null<Session*> mts, Api::Channels::Done done) {
			mts->channels()->joinChat(chatId, [=](bool ok) {
				if (ok && weak) {
					// Messages posted while I was not a member may be
					// missing in the preview, the newest ones are reloaded.
					if (const auto strong = weak->account().mtsLinkSession()) {
						LOG(("MtsLink Channel: reload messages of %1 after join"
							).arg(chatId));
						strong->messages()->load(chatId);
					}
					// The chat list entry appears with the full info.
					if (const auto strong = weak->account().mtsLinkSession()) {
						strong->channels()->loadChatInfo(chatId);
					}
				}
				done(ok);
			});
		});
}

void createGroupChat(
		not_null<Main::Session*> session,
		const QString &title,
		const std::vector<not_null<UserData*>> &users,
		QImage cover,
		Fn<void(ChannelData*)> done) {
	const auto mts = session->account().mtsLinkSession();
	if (!mts || !mts->channels()) {
		done(nullptr);
		return;
	}
	auto userIds = QStringList();
	for (const auto &user : users) {
		const auto id = userBareIdToUuid(peerToUser(user->id).bare);
		if (!id.isEmpty()) {
			userIds.push_back(id);
		}
	}
	const auto weak = base::make_weak(session);
	const auto selfId = mts->userId();
	LOG(("MtsLink Group: create '%1' with %2 of %3 users"
		).arg(title, userIds.join(',')).arg(users.size()));
	mts->channels()->createGroupChat(
		title,
		mts->organizationId(),
		userIds,
		[=](std::optional<Api::ChannelData> result) {
			if (!weak) {
				return;
			} else if (!result) {
				LOG(("MtsLink Group: create failed"));
				showMtsLinkToast(weak.get(), tr::lng_cant_do_this(tr::now));
				done(nullptr);
				return;
			}
			auto data = *result;
			data.memberRole = (data.memberRole == selfId
				|| data.memberRole.isEmpty())
				? u"Owner"_q
				: data.memberRole;
			LOG(("MtsLink Group: created %1").arg(data.id));
			applyChannelData(weak.get(), data);
			const auto peerId = chatIdToPeerId(data.id, ChatType::GroupChat);
			const auto channel = weak->data().channelLoaded(
				peerToChannel(peerId));
			if (const auto strong = weak->account().mtsLinkSession()) {
				strong->users()->loadChatMembers(data.id);
				if (!cover.isNull()) {
					auto bytes = QByteArray();
					QBuffer buffer(&bytes);
					buffer.open(QIODevice::WriteOnly);
					cover.save(&buffer, "PNG");
					const auto chatId = data.id;
					strong->files()->uploadAvatar(
						u"Avatar.png"_q,
						bytes,
						u"image/png"_q,
						[=](const Api::UploadResult &uploaded) {
							const auto mts = weak
								? weak->account().mtsLinkSession()
								: nullptr;
							if (!mts) {
								return;
							}
							attachChatCover(
								weak.get(),
								chatId,
								true,
								uploaded.id);
						},
						[=](const QString &error) {
							LOG(("MtsLink Group: cover upload failed: %1"
								).arg(error));
						});
				}
			}
			done(channel);
		});
}

void searchOrganizationMembers(
		not_null<Main::Session*> session,
		const QString &query,
		Fn<void(std::vector<not_null<UserData*>>)> done) {
	const auto mts = session->account().mtsLinkSession();
	if (!mts || !mts->users()) {
		done({});
		return;
	}
	const auto weak = base::make_weak(session);
	mts->users()->searchMembers(
		query,
		0,
		20,
		[=](QList<Api::MemberProfile> profiles) {
			if (!weak) {
				return;
			}
			auto users = std::vector<not_null<UserData*>>();
			for (const auto &profile : profiles) {
				applyUserData(weak.get(), profile);
				users.push_back(weak->data().user(
					::UserId(uuidToBareId(profile.userId))));
			}
			LOG(("MtsLink Group: members search '%1' found %2"
				).arg(query).arg(users.size()));
			done(std::move(users));
		},
		{ mts->userId() });
}

void searchChannelNonMembers(
		not_null<Main::Session*> session,
		not_null<PeerData*> channel,
		const QString &query,
		Fn<void(std::vector<not_null<UserData*>>)> done) {
	const auto mts = session->account().mtsLinkSession();
	const auto chatId = peerIdToChatId(channel->id);
	if (!mts || !mts->channels() || chatId.isEmpty()) {
		done({});
		return;
	}
	const auto weak = base::make_weak(session);
	LOG(("MtsLink Invite: search '%1' in %2").arg(query, chatId));
	mts->channels()->searchNonMembers(
		chatId,
		isGroupChat(channel->id),
		query,
		0,
		50,
		[=](QList<Api::MemberProfile> profiles) {
			LOG(("MtsLink Invite: search '%1' found %2"
				).arg(query).arg(profiles.size()));
			if (!weak) {
				return;
			}
			auto users = std::vector<not_null<UserData*>>();
			users.reserve(profiles.size());
			for (const auto &profile : profiles) {
				applyUserData(weak.get(), profile);
				users.push_back(weak->data().user(
					::UserId(uuidToBareId(profile.userId))));
			}
			done(std::move(users));
		});
}

void removeChannelMember(
		not_null<Main::Session*> session,
		not_null<PeerData*> channel,
		not_null<UserData*> user) {
	const auto chatId = peerIdToChatId(channel->id);
	const auto userId = userBareIdToUuid(peerToUser(user->id).bare);
	channelAction(
		session,
		u"remove %1 from %2"_q.arg(userId, chatId),
		[=](not_null<Session*> mts, Api::Channels::Done done) {
			if (isGroupChat(channel->id)) {
				mts->channels()->removeGroupUsers(
					chatId,
					{ userId },
					mts->organizationId(),
					std::move(done));
			} else {
				mts->channels()->removeUsers(
					chatId,
					{ userId },
					mts->organizationId(),
					std::move(done));
			}
		});
}

void startCall(not_null<Main::Session*> session, not_null<PeerData*> peer) {
	if (session->windows().empty()) {
		return;
	}
	const auto weak = base::make_weak(session);
	const auto start = [=](CallBrowser browser) {
		if (const auto strong = weak.get()) {
			performStartCall(strong, peer, browser);
		}
	};
	// As in Telegram: an ongoing video chat is joined right away, a new one
	// is created after the confirmation.
	if (!activeCallJoinLink(peer->id).isEmpty()) {
		LOG(("MtsLink Call: joining the ongoing call without confirmation"));
		start(CallBrowser::Default);
		return;
	}
	auto text = isPersonalChat(peer->id)
		? tr::lng_mtslink_call_confirm_personal(
			tr::now,
			lt_user,
			tr::bold(peer->name()),
			tr::marked)
		: TextWithEntities{ peer->isBroadcast()
			? tr::lng_group_call_create_sure_channel(tr::now)
			: tr::lng_group_call_create_sure(tr::now) };
	session->windows().front()->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(text),
			st::boxLabel));
		box->addButton(tr::lng_create_group_create(), [=] {
			box->closeBox();
			start(CallBrowser::Default);
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		box->addLeftButton(tr::lng_mtslink_call_open_browser(), [=] {
			box->closeBox();
			start(CallBrowser::System);
		});
	}));
}

bool openIncomingCall(not_null<HistoryItem*> item) {
	if (!CallMessages.contains(item->fullId())) {
		return false;
	}
	const auto peer = item->history()->peer;
	const auto joinLink = activeCallJoinLink(peer->id);
	const auto author = item->from()->asUser();
	LOG(("MtsLink Call: notification click, broadcast=%1 author=%2 link=%3"
		).arg(peer->isBroadcast() ? 1 : 0
		).arg(author ? 1 : 0
		).arg(joinLink));
	if (joinLink.isEmpty()) {
		LOG(("MtsLink Call: notification of a finished call"));
		return false;
	} else if (peer->isBroadcast() || !author) {
		// Channels (posts are from the channel itself): the call window
		// with "join" and "open in browser".
		Core::App().calls().showMtsLinkJoinCall(peer);
		return true;
	}
	Core::App().calls().showMtsLinkIncomingCall(peer, author, joinLink);
	return true;
}

void startCallNow(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		bool systemBrowser) {
	performStartCall(
		session,
		peer,
		systemBrowser ? CallBrowser::System : CallBrowser::Default);
}

void setActiveCall(
		not_null<Main::Session*> session,
		PeerId peerId,
		const Api::CallMetadata &meta) {
	rememberCallEvent(meta.joinLink, meta.webinarEventId);
	setActiveCall(session, peerId, meta.joinLink);
}

void setActiveCall(
		not_null<Main::Session*> session,
		PeerId peerId,
		const QString &joinLink) {
	if (joinLink.isEmpty()) {
		if (ActiveCallLinks.remove(peerId)) {
			ActiveCallChanges.fire_copy(peerId);
			if (peerIsChannel(peerId)) {
				if (const auto channel = session->data().channelLoaded(
						peerToChannel(peerId))) {
					using Flag = ChannelDataFlag;
					channel->removeFlags(
						Flag::CallActive | Flag::CallNotEmpty);
					session->changes().peerUpdated(
						channel,
						Data::PeerUpdate::Flag::GroupCall);
				}
			}
		}
	} else {
		auto &existing = ActiveCallLinks[peerId];
		if (existing != joinLink) {
			existing = joinLink;
			ActiveCallChanges.fire_copy(peerId);
			if (peerIsChannel(peerId)) {
				const auto channel = session->data().channelLoaded(
						peerToChannel(peerId));
				if (channel) {
					using Flag = ChannelDataFlag;
					channel->addFlags(
						Flag::CallActive | Flag::CallNotEmpty);
					session->changes().peerUpdated(
						channel,
						Data::PeerUpdate::Flag::GroupCall);
				}
			}
		}
	}
}

QString activeCallJoinLink(PeerId peerId) {
	return ActiveCallLinks.value(peerId);
}

rpl::producer<Ui::GroupCallBarContent> activeCallBarContent(PeerId peerId) {
	return ActiveCallChanges.events_starting_with_copy(
		peerId
	) | rpl::filter([=](PeerId id) {
		return id == peerId;
	}) | rpl::map([=] {
		const auto hasCall = ActiveCallLinks.contains(peerId);
		return Ui::GroupCallBarContent{
			.count = hasCall ? 1 : 0,
			.shown = hasCall,
		};
	});
}

namespace {

struct ScheduledInfo {
	QString id;
	QString chatId;
	QString markdown;
	QStringList fileIds;
};
QHash<MsgId, ScheduledInfo> ScheduledById; // By the remote id.

[[nodiscard]] MsgId ScheduledRemoteId(const QString &uuid) {
	return MsgId(int64(uuidToBareId(uuid) & 0x3FFFFFFFULL) + 1);
}

[[nodiscard]] MTPMessage ScheduledToMTP(
		not_null<Main::Session*> session,
		PeerId peerId,
		MsgId remoteId,
		const QJsonObject &obj) {
	const auto date = TimeId(
		qint64(obj.value(u"scheduledAt"_q).toDouble()) / 1000);
	const auto text = parseMentionedText(
		obj.value(u"text"_q).toString(),
		obj.value(u"markdown"_q).toString(),
		{},
		session);
	auto media = MTPMessageMedia(MTP_messageMediaEmpty());
	const auto files = obj.value(u"files"_q).toArray();
	if (!files.isEmpty()) {
		media = buildFileMedia(
			session,
			Api::ParseFileData(files.first().toObject()),
			date);
	}
	using Flag = MTPDmessage::Flag;
	const auto flags = Flag::f_entities
		| Flag::f_from_id
		| Flag::f_out
		| Flag::f_from_scheduled
		| (files.isEmpty() ? Flag(0) : Flag::f_media);
	return MTP_message(
		MTP_flags(flags),
		MTP_int(remoteId.bare),
		peerToMTP(session->userPeerId()),
		MTPint(), // from_boosts_applied
		MTPstring(), // from_rank
		peerToMTP(peerId),
		MTPPeer(), // saved_peer_id
		MTPMessageFwdHeader(),
		MTPlong(), // via_bot_id
		MTPlong(), // via_business_bot_id
		MTPPeer(), // guestchat_via_from
		MTPMessageReplyHeader(),
		MTP_int(date),
		MTP_string(text.text),
		media,
		MTPReplyMarkup(),
		::Api::EntitiesToMTP(session, text.entities),
		MTPint(), // views
		MTPint(), // forwards
		MTPMessageReplies(),
		MTPint(), // edit_date
		MTPstring(), // post_author
		MTPlong(), // grouped_id
		MTPMessageReactions(),
		MTPVector<MTPRestrictionReason>(),
		MTPint(), // ttl_period
		MTPint(), // quick_reply_shortcut_id
		MTPlong(), // effect
		MTPFactCheck(),
		MTPint(), // report_delivery_until_date
		MTPlong(), // paid_message_stars
		MTPSuggestedPost(),
		MTPint(), // schedule_repeat_period
		MTPstring(), // summary_from_language
		MTPRichMessage());
}

} // namespace

// The chats where the scheduled messages can't be got (read only, not a
// member: ERR_USER_CANNOT_GET_SCHEDULED_MESSAGE_FROM_CHAT_OR_THREAD).
QSet<QString> NoScheduledChats;

void requestScheduledMessages(
		not_null<Main::Session*> session,
		PeerId peerId,
		Fn<void(QVector<MTPMessage>)> done) {
	const auto mts = session->account().mtsLinkSession();
	const auto chatId = peerIdToChatId(peerId);
	const auto peer = session->data().peerLoaded(peerId);
	if (!mts
		|| !mts->rpc()
		|| chatId.isEmpty()
		|| NoScheduledChats.contains(chatId)
		|| (peer && !Data::CanSendAnything(peer))) {
		done({});
		return;
	}
	const auto weak = base::make_weak(session);
	mts->rpc()->call(
		u"MessageScheduler.GetMessagesScheduledByFilter"_q,
		QJsonObject{
			{ u"type"_q, u"GetMessagesScheduledFilter"_q },
			{ u"value"_q, QJsonObject{
				{ u"chatId"_q, chatId },
				{ u"limit"_q, 100 },
			} },
		},
		[=](const QJsonObject &result) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			if (result.value(u"type"_q).toString() == u"BusinessError"_q) {
				NoScheduledChats.insert(chatId);
			}
			auto list = QVector<MTPMessage>();
			const auto messages = result.value(u"value"_q).toObject().value(
				u"messages"_q).toArray();
			for (const auto &value : messages) {
				const auto obj = value.toObject();
				const auto id = obj.value(u"id"_q).toString();
				if (id.isEmpty()
					|| obj.value(u"status"_q).toString(u"Scheduled"_q)
						!= u"Scheduled"_q) {
					continue;
				}
				const auto remoteId = ScheduledRemoteId(id);
				auto fileIds = QStringList();
				for (const auto &file : obj.value(u"files"_q).toArray()) {
					fileIds.push_back(
						file.toObject().value(u"id"_q).toString());
				}
				ScheduledById.insert(remoteId, ScheduledInfo{
					.id = id,
					.chatId = chatId,
					.markdown = obj.value(u"markdown"_q).toString(
						obj.value(u"text"_q).toString()),
					.fileIds = fileIds,
				});
				list.push_back(ScheduledToMTP(strong, peerId, remoteId, obj));
			}
			done(std::move(list));
		});
}

void createScheduledMessage(
		not_null<Main::Session*> session,
		PeerId peerId,
		const QString &markdown,
		const QStringList &fileIds,
		TimeId date) {
	const auto mts = session->account().mtsLinkSession();
	const auto chatId = peerIdToChatId(peerId);
	if (!mts || !mts->rpc() || chatId.isEmpty()) {
		return;
	}
	auto files = QJsonArray();
	for (const auto &id : fileIds) {
		files.push_back(id);
	}
	mts->rpc()->call(
		u"MessageScheduler.CreateMessageScheduled"_q,
		QJsonObject{
			{ u"chatId"_q, chatId },
			{ u"text"_q, markdown },
			{ u"isMarkdown"_q, true },
			{ u"fileIds"_q, files },
			{ u"scheduledAt"_q, double(qint64(date) * 1000) },
		},
		[](const QJsonObject &) {});
}

void deleteScheduledMessages(
		not_null<Main::Session*> session,
		const QVector<MsgId> &remoteIds) {
	const auto mts = session->account().mtsLinkSession();
	if (!mts || !mts->rpc()) {
		return;
	}
	for (const auto remoteId : remoteIds) {
		const auto info = ScheduledById.value(remoteId);
		if (info.id.isEmpty()) {
			continue;
		}
		mts->rpc()->call(
			u"MessageScheduler.DeleteMessageScheduled"_q,
			QJsonObject{ { u"id"_q, info.id } },
			[](const QJsonObject &) {});
	}
}

void sendScheduledMessagesNow(
		not_null<Main::Session*> session,
		const QVector<MsgId> &remoteIds) {
	const auto mts = session->account().mtsLinkSession();
	if (!mts || !mts->rpc()) {
		return;
	}
	for (const auto remoteId : remoteIds) {
		const auto info = ScheduledById.value(remoteId);
		if (info.id.isEmpty()) {
			continue;
		}
		mts->rpc()->call(
			u"MessageScheduler.SendMessageScheduledNow"_q,
			QJsonObject{ { u"id"_q, info.id } },
			[](const QJsonObject &) {});
	}
}

void rescheduleMessage(
		not_null<Main::Session*> session,
		MsgId remoteId,
		TimeId date,
		std::optional<QString> markdown) {
	const auto mts = session->account().mtsLinkSession();
	const auto info = ScheduledById.value(remoteId);
	if (!mts || !mts->rpc() || info.id.isEmpty()) {
		return;
	}
	const auto peerId = chatIdToPeerId(info.chatId);
	const auto text = markdown.value_or(info.markdown);
	const auto weak = base::make_weak(session);
	mts->rpc()->call(
		u"MessageScheduler.DeleteMessageScheduled"_q,
		QJsonObject{ { u"id"_q, info.id } },
		[=](const QJsonObject &) {
			if (const auto strong = weak.get()) {
				createScheduledMessage(
					strong,
					peerId,
					text,
					info.fileIds,
					date);
			}
		});
}

void handleSchedulerEvent(
		not_null<Main::Session*> session,
		const QJsonObject &param) {
	const auto type = param.value(u"type"_q).toString();
	const auto value = param.value(u"value"_q).toObject();
	const auto chatId = value.value(u"chatId"_q).toString(
		value.value(u"messageScheduled"_q).toObject().value(
			u"chatId"_q).toString());
	if (chatId.isEmpty()) {
		return;
	}
	const auto peerId = chatIdToPeerId(chatId);
	if (const auto history = session->data().historyLoaded(peerId)) {
		session->scheduledMessages().mtsLinkRefresh(history);
	}
}

namespace {

constexpr auto kMtsLinkSavedGifsTag = uint64(0xBC07'0000'0000'0001ULL);
constexpr auto kMtsLinkGifBytesTag = uint64(0xBC08'0000'0000'0000ULL);
bool RestoringSavedGifs = false;
QSet<QString> GifBytesKept;

[[nodiscard]] Storage::Cache::Key savedGifsCacheKey() {
	return { kMtsLinkSavedGifsTag, 0 };
}

// The content of a saved GIF in the session cache, by its MTS Link file:
// its message (and the file on the server with it) may be deleted.
[[nodiscard]] Storage::Cache::Key gifBytesCacheKey(const QString &fileId) {
	const auto hash = QCryptographicHash::hash(
		fileId.toUtf8(),
		QCryptographicHash::Md5);
	uint64 low = 0;
	memcpy(&low, hash.constData(), sizeof(low));
	return { kMtsLinkGifBytesTag, low };
}

[[nodiscard]] QString fileDownloadUrl(const QString &fileId) {
	return fileDownloadBase() + fileId + u"/download"_q;
}

// The content in the file cache of the document (as Telegram keeps the
// loaded files): it is shown from the cache, not loaded from the server.
void putToDocumentCache(
		not_null<DocumentData*> document,
		const QString &url,
		QByteArray bytes) {
	document->session().data().cache().put(
		Data::UrlCacheKey(url),
		Storage::Cache::Database::TaggedValue(
			std::move(bytes),
			document->cacheTag()));
}

// The content of a file: loaded already, or downloaded with the session.
void loadFileBytes(
		DocumentData *document,
		const QString &fileId,
		Fn<void(QByteArray)> done) {
	auto bytes = QByteArray();
	if (const auto media = document ? document->activeMediaView() : nullptr) {
		bytes = media->bytes();
	}
	if (bytes.isEmpty() && document) {
		const auto &location = document->location(true);
		if (!location.isEmpty() && location.accessEnable()) {
			auto f = QFile(location.name());
			if (f.open(QIODevice::ReadOnly)) {
				bytes = f.readAll();
			}
			location.accessDisable();
		}
	}
	if (!bytes.isEmpty()) {
		done(std::move(bytes));
		return;
	}
	static const auto manager = new QNetworkAccessManager();
	auto request = QNetworkRequest(QUrl(fileDownloadUrl(fileId)));
	auto cookies = QStringList();
	for (const auto &cookie : fileAuthCookies()) {
		cookies.push_back(QString::fromLatin1(cookie.name())
			+ '='
			+ QString::fromLatin1(cookie.value()));
	}
	request.setRawHeader("Cookie", cookies.join(u"; "_q).toLatin1());
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	const auto reply = manager->get(request);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		done((status == 200) ? reply->readAll() : QByteArray());
	});
}

void keepGifBytes(
		not_null<Main::Session*> session,
		not_null<DocumentData*> document,
		const QString &fileId) {
	if (GifBytesKept.contains(fileId)) {
		return;
	}
	GifBytesKept.insert(fileId);
	const auto weak = base::make_weak(session);
	loadFileBytes(document, fileId, [=](QByteArray bytes) {
		const auto strong = weak.get();
		if (!strong || bytes.isEmpty()) {
			GifBytesKept.remove(fileId);
			return;
		}
		LOG(("MtsLink Gifs: kept %1 bytes of %2"
			).arg(bytes.size()
			).arg(fileId));
		strong->data().cache().put(
			gifBytesCacheKey(fileId),
			std::move(bytes));
	});
}

void saveSavedGifs(not_null<Main::Session*> session) {
	auto files = QList<Api::FileData>();
	for (const auto document : session->data().stickers().savedGifs()) {
		const auto i = GifFiles.constFind(document->id);
		if (i != GifFiles.constEnd()) {
			files.push_back(i.value());
			keepGifBytes(session, document, i->id);
		}
	}
	// The removed GIFs: their contents are not kept anymore.
	for (const auto &id : base::duplicate(GifBytesKept)) {
		const auto used = ranges::any_of(files, [&](const Api::FileData &f) {
			return (f.id == id);
		});
		if (!used) {
			GifBytesKept.remove(id);
			session->data().cache().remove(gifBytesCacheKey(id));
		}
	}
	auto data = QByteArray();
	{
		QDataStream s(&data, QIODevice::WriteOnly);
		s.setVersion(QDataStream::Qt_5_1);
		s << qint32(1) << qint32(files.size());
		for (const auto &f : files) {
			s << f.id << f.name << f.size << f.mime
				<< qint32(f.width) << qint32(f.height);
		}
	}
	LOG(("MtsLink Gifs: saved %1 GIFs").arg(files.size()));
	session->data().cache().put(savedGifsCacheKey(), std::move(data));
}

} // namespace

namespace {

struct LocalSticker {
	QString key; // The MTS Link file id of the image.
	QString name;
	QString mime;
	int width = 0;
	int height = 0;
};
constexpr auto kMtsLinkStickersTag = uint64(0xBC09'0000'0000'0001ULL);
constexpr auto kMtsLinkStickerBytesTag = uint64(0xBC0A'0000'0000'0000ULL);
QHash<DocumentId, LocalSticker> LocalStickers;
bool RestoringLocalStickers = false;
// The imported sticker packs: local sets, by their ids.
QSet<uint64> LocalPackIds;
constexpr auto kMaxPackStickers = 120;
constexpr auto kPackStickerSide = 512;

// A static sticker as in Telegram: the longer side 512 at most (shown at
// ~256 anyway), PNG with the transparency, JPEG without, smaller ones kept.
// False if it is not an image.
[[nodiscard]] bool normalizeStickerImage(
		QByteArray &bytes,
		QString &name,
		QString &mime,
		int &width,
		int &height) {
	QBuffer buffer(&bytes);
	buffer.open(QIODevice::ReadOnly);
	QImageReader reader(&buffer);
	const auto format = reader.format().toLower();
	auto frame = reader.read();
	buffer.close();
	if (frame.isNull()) {
		return false;
	}
	const auto known = (format == "png")
		|| (format == "jpeg")
		|| (format == "jpg");
	const auto big = (frame.width() > kPackStickerSide)
		|| (frame.height() > kPackStickerSide);
	if (known && !big) {
		mime = (format == "png") ? u"image/png"_q : u"image/jpeg"_q;
		width = frame.width();
		height = frame.height();
		return true;
	}
	if (big) {
		frame = frame.scaled(
			kPackStickerSide,
			kPackStickerSide,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
	}
	const auto alpha = frame.hasAlphaChannel();
	auto result = QByteArray();
	QBuffer out(&result);
	out.open(QIODevice::WriteOnly);
	frame.save(&out, alpha ? "PNG" : "JPEG", alpha ? -1 : 90);
	out.close();
	LOG(("MtsLink Stickers: %1 %2 (%3 bytes) -> %4x%5 %6 (%7 bytes)"
		).arg(name
		).arg(QString::fromLatin1(format)
		).arg(bytes.size()
		).arg(frame.width()
		).arg(frame.height()
		).arg(alpha ? u"png"_q : u"jpeg"_q
		).arg(result.size()));
	bytes = result;
	mime = alpha ? u"image/png"_q : u"image/jpeg"_q;
	name = QFileInfo(name).completeBaseName()
		+ (alpha ? u".png"_q : u".jpg"_q);
	width = frame.width();
	height = frame.height();
	return true;
}

[[nodiscard]] Storage::Cache::Key stickersCacheKey() {
	return { kMtsLinkStickersTag, 0 };
}

[[nodiscard]] Storage::Cache::Key stickerBytesCacheKey(const QString &key) {
	const auto hash = QCryptographicHash::hash(
		key.toUtf8(),
		QCryptographicHash::Md5);
	uint64 low = 0;
	memcpy(&low, hash.constData(), sizeof(low));
	return { kMtsLinkStickerBytesTag, low };
}

[[nodiscard]] QString stickerUrl(const QString &key) {
	// Not loaded from anywhere: the content is in the cache.
	return u"mtslink-sticker://"_q + key;
}

[[nodiscard]] DocumentId stickerDocumentId(const QString &key) {
	return DocumentId(uuidToBareId(u"mtslink-sticker:"_q + key));
}

// A sticker document of a local image: shown in the stickers panel.
not_null<DocumentData*> makeLocalSticker(
		not_null<Main::Session*> session,
		const LocalSticker &sticker,
		int64 size) {
	const auto id = stickerDocumentId(sticker.key);
	LocalStickers.insert(id, sticker);
	const auto attrs = stickerAttributes(
		sticker.name,
		sticker.width,
		sticker.height);
	const auto document = session->data().document(
		id,
		uint64(0),
		QByteArray(),
		base::unixtime::now(),
		attrs,
		sticker.mime,
		InlineImageLocation(),
		ImageWithLocation(),
		ImageWithLocation(),
		false,
		session->mainDcId(),
		size);
	document->setContentUrl(stickerUrl(sticker.key));
	return document;
}

void writeLocalSticker(QDataStream &s, const LocalSticker &sticker) {
	s << sticker.key << sticker.name << sticker.mime
		<< qint32(sticker.width) << qint32(sticker.height);
}

[[nodiscard]] std::optional<LocalSticker> readLocalSticker(QDataStream &s) {
	auto sticker = LocalSticker();
	auto w = qint32();
	auto h = qint32();
	s >> sticker.key >> sticker.name >> sticker.mime >> w >> h;
	if (s.status() != QDataStream::Ok) {
		return std::nullopt;
	}
	sticker.width = w;
	sticker.height = h;
	return sticker;
}

[[nodiscard]] QList<LocalSticker> localStickersOf(
		const Data::StickersPack &pack) {
	auto result = QList<LocalSticker>();
	for (const auto document : pack) {
		const auto j = LocalStickers.constFind(document->id);
		if (j != LocalStickers.constEnd()) {
			result.push_back(j.value());
		}
	}
	return result;
}

void saveLocalStickers(not_null<Main::Session*> session) {
	using Flag = Data::StickersSetFlag;
	const auto &sets = session->data().stickers().sets();
	const auto i = sets.find(Data::Stickers::FavedSetId);
	const auto faved = (i != sets.end())
		? localStickersOf(i->second->stickers)
		: QList<LocalSticker>();

	// The installed packs, in the order of the panel.
	struct Pack {
		uint64 id = 0;
		QString title;
		QList<LocalSticker> stickers;
	};
	auto packs = std::vector<Pack>();
	for (const auto setId : session->data().stickers().setsOrder()) {
		if (!LocalPackIds.contains(setId)) {
			continue;
		}
		const auto j = sets.find(setId);
		if (j != sets.end() && (j->second->flags & Flag::Installed)) {
			packs.push_back({
				setId,
				j->second->title,
				localStickersOf(j->second->stickers),
			});
		}
	}
	for (auto j = LocalPackIds.begin(); j != LocalPackIds.end();) {
		const auto id = *j;
		const auto kept = ranges::any_of(packs, [&](const Pack &pack) {
			return (pack.id == id);
		});
		if (!kept) {
			LOG(("MtsLink Stickers: pack %1 removed").arg(id));
			j = LocalPackIds.erase(j);
		} else {
			++j;
		}
	}

	// The removed stickers: their contents are not kept anymore.
	auto used = QSet<QString>();
	for (const auto &sticker : faved) {
		used.insert(sticker.key);
	}
	for (const auto &pack : packs) {
		for (const auto &sticker : pack.stickers) {
			used.insert(sticker.key);
		}
	}
	for (auto j = LocalStickers.begin(); j != LocalStickers.end();) {
		if (!used.contains(j->key)) {
			session->data().cache().remove(stickerBytesCacheKey(j->key));
			j = LocalStickers.erase(j);
		} else {
			++j;
		}
	}
	auto data = QByteArray();
	{
		QDataStream s(&data, QIODevice::WriteOnly);
		s.setVersion(QDataStream::Qt_5_1);
		s << qint32(2) << qint32(faved.size());
		for (const auto &sticker : faved) {
			writeLocalSticker(s, sticker);
		}
		s << qint32(packs.size());
		for (const auto &pack : packs) {
			s << quint64(pack.id) << pack.title << qint32(pack.stickers.size());
			for (const auto &sticker : pack.stickers) {
				writeLocalSticker(s, sticker);
			}
		}
	}
	LOG(("MtsLink Stickers: saved %1 faved, %2 packs"
		).arg(faved.size()
		).arg(packs.size()));
	session->data().cache().put(stickersCacheKey(), std::move(data));
}

// The content of a local sticker from the cache: shown from it.
void showLocalStickerFromCache(
		not_null<Main::Session*> session,
		not_null<DocumentData*> document,
		const QString &key) {
	const auto weak = base::make_weak(session);
	session->data().cache().get(stickerBytesCacheKey(key), [=](
			QByteArray &&bytes) {
		crl::on_main(weak, [=, bytes = std::move(bytes)] {
			if (weak.get() && !bytes.isEmpty()) {
				putToDocumentCache(document, stickerUrl(key), bytes);
			}
		});
	});
}

// An imported pack: an installed set of the panel (no set on the server).
void applyLocalPack(
		not_null<Main::Session*> session,
		uint64 setId,
		const QString &title,
		const Data::StickersPack &stickers,
		bool toFront) {
	using Flag = Data::StickersSetFlag;
	auto &sets = session->data().stickers().setsRef();
	const auto now = base::unixtime::now();
	auto i = sets.find(setId);
	if (i == sets.end()) {
		i = sets.emplace(setId, std::make_unique<Data::StickersSet>(
			&session->data(),
			setId,
			uint64(0), // No access hash: not requested from the server.
			uint64(0),
			title,
			QString(),
			int(stickers.size()),
			Flag::Installed,
			now)).first;
	}
	const auto set = i->second.get();
	set->title = title;
	set->flags = Flag::Installed;
	set->installDate = now;
	set->count = int(stickers.size());
	set->stickers = stickers;
	set->dates = std::vector<TimeId>(stickers.size(), now);
	set->emoji.clear();
	LocalPackIds.insert(setId);
	auto &order = session->data().stickers().setsOrderRef();
	if (toFront) {
		order.removeAll(setId);
		order.push_front(setId);
	} else if (!order.contains(setId)) {
		order.push_back(setId);
	}
}

} // namespace

// Diagnostics: the format and the transparency of an image.
[[nodiscard]] QString describeImage(const QByteArray &bytes) {
	auto copy = bytes;
	QBuffer buffer(&copy);
	buffer.open(QIODevice::ReadOnly);
	QImageReader reader(&buffer);
	const auto format = QString::fromLatin1(reader.format());
	const auto image = reader.read();
	return u"format=%1 size=%2x%3 alpha=%4"_q
		.arg(format.isEmpty() ? u"?"_q : format)
		.arg(image.width())
		.arg(image.height())
		.arg(image.hasAlphaChannel() ? 1 : 0);
}

// The content of an image by its file: the urls of buildPhotoMedia.
void putToImageCache(
		not_null<Main::Session*> session,
		const QString &fileId,
		const QByteArray &bytes) {
	for (const auto &url : {
		privateCdnThumbBase() + fileId + u"_s.jpg"_q,
		privateCdnThumbBase() + fileId + u".jpg"_q,
	}) {
		LOG(("MtsLink Stickers: image cache %1 (%2 bytes)"
			).arg(url
			).arg(bytes.size()));
		session->data().cache().put(
			Data::UrlCacheKey(url),
			Storage::Cache::Database::TaggedValue(
				QByteArray(bytes),
				Data::kImageCacheTag));
	}
}

bool toggleFavedSticker(not_null<DocumentData*> document, bool faved) {
	auto &stickers = document->owner().stickers();
	if (!faved || LocalStickers.contains(document->id)) {
		stickers.mtsLinkSetFaved(document, faved);
		return true;
	}
	const auto i = StickerFiles.constFind(document->id);
	if (i == StickerFiles.constEnd()) {
		LOG(("MtsLink Stickers: not a local sticker %1").arg(document->id));
		return false;
	}
	// The sticker of a message: kept with its content, as the images.
	const auto file = i.value();
	const auto session = &document->session();
	const auto weak = base::make_weak(session);
	loadFileBytes(document, file.id, [=](QByteArray bytes) {
		const auto strong = weak.get();
		if (!strong || bytes.isEmpty()) {
			LOG(("MtsLink Stickers: no content of %1").arg(file.id));
			return;
		}
		const auto sticker = LocalSticker{
			.key = file.id,
			.name = file.name,
			.mime = file.mime,
			.width = (file.width > 0) ? file.width : kAnimatedStickerSide,
			.height = (file.height > 0) ? file.height : kAnimatedStickerSide,
		};
		const auto local = makeLocalSticker(strong, sticker, bytes.size());
		strong->data().cache().put(
			stickerBytesCacheKey(file.id),
			QByteArray(bytes));
		putToDocumentCache(local, stickerUrl(file.id), bytes);
		LOG(("MtsLink Stickers: added sticker %1 (%2 bytes)"
			).arg(file.id
			).arg(bytes.size()));
		strong->data().stickers().mtsLinkSetFaved(local, true);
	});
	return true;
}

namespace {

[[nodiscard]] bool nameMatches(const QString &name, const QString &query) {
	// "my_cat-01.png" is found by "cat" and by "my cat".
	auto words = QFileInfo(name).completeBaseName().toLower();
	words.replace('_', ' ').replace('-', ' ');
	for (const auto &part : query.toLower().split(' ', Qt::SkipEmptyParts)) {
		if (!words.contains(part)) {
			return false;
		}
	}
	return true;
}

} // namespace

LocalStickersSearch searchLocalStickers(
		not_null<Main::Session*> session,
		const QString &query) {
	auto result = LocalStickersSearch();
	const auto &sets = session->data().stickers().sets();
	auto inFoundSet = base::flat_set<DocumentId>();
	for (const auto setId : session->data().stickers().setsOrder()) {
		if (!LocalPackIds.contains(setId)) {
			continue;
		}
		const auto i = sets.find(setId);
		if (i != sets.end() && nameMatches(i->second->title, query)) {
			result.sets.push_back(setId);
			for (const auto document : i->second->stickers) {
				inFoundSet.emplace(document->id);
			}
		}
	}
	// The stickers by their names: the favorites first, then the packs.
	const auto add = [&](const Data::StickersPack &pack) {
		for (const auto document : pack) {
			const auto j = LocalStickers.constFind(document->id);
			if (j != LocalStickers.constEnd()
				&& !inFoundSet.contains(document->id)
				&& !ranges::contains(result.stickers, document->id)
				&& nameMatches(j->name, query)) {
				result.stickers.push_back(document->id);
			}
		}
	};
	if (const auto i = sets.find(Data::Stickers::FavedSetId)
		; i != sets.end()) {
		add(i->second->stickers);
	}
	for (const auto setId : session->data().stickers().setsOrder()) {
		if (LocalPackIds.contains(setId)) {
			if (const auto i = sets.find(setId); i != sets.end()) {
				add(i->second->stickers);
			}
		}
	}
	LOG(("MtsLink Stickers: search '%1', %2 packs, %3 stickers"
		).arg(query
		).arg(result.sets.size()
		).arg(result.stickers.size()));
	return result;
}

QString panelItemName(not_null<DocumentData*> document) {
	const auto i = LocalStickers.constFind(document->id);
	auto name = (i != LocalStickers.constEnd())
		? i->name
		: document->filename();
	if (name.endsWith(QLatin1String(kWebmStickerSuffix), Qt::CaseInsensitive)) {
		name.chop(int(strlen(kWebmStickerSuffix)));
	}
	return QFileInfo(name).completeBaseName();
}

bool savedGifMatches(
		not_null<DocumentData*> document,
		const QString &query) {
	return nameMatches(document->filename(), query);
}

bool canAddPhotoToStickers(not_null<PhotoData*> photo) {
	const auto i = PhotoFiles.constFind(photo->id);
	// Bigger images are scaled down to the sticker size when added.
	constexpr auto kMaxSourceSize = 10 * 1024 * 1024;
	return (i != PhotoFiles.constEnd())
		&& (i->size <= kMaxSourceSize)
		&& !LocalStickers.contains(stickerDocumentId(i->id));
}

void addPhotoToStickers(not_null<PhotoData*> photo) {
	const auto i = PhotoFiles.constFind(photo->id);
	if (i == PhotoFiles.constEnd()) {
		return;
	}
	const auto file = i.value();
	const auto session = &photo->session();
	const auto weak = base::make_weak(session);
	loadFileBytes(nullptr, file.id, [=](QByteArray bytes) {
		const auto strong = weak.get();
		if (!strong || bytes.isEmpty()) {
			LOG(("MtsLink Stickers: no content of %1").arg(file.id));
			return;
		}
		auto content = bytes;
		auto sticker = LocalSticker{
			.key = file.id,
			.name = file.name,
			.mime = file.mime,
			.width = file.width,
			.height = file.height,
		};
		if (!normalizeStickerImage(
				content,
				sticker.name,
				sticker.mime,
				sticker.width,
				sticker.height)) {
			LOG(("MtsLink Stickers: not an image %1").arg(file.id));
			return;
		}
		const auto document = makeLocalSticker(
			strong,
			sticker,
			content.size());
		strong->data().cache().put(stickerBytesCacheKey(file.id), QByteArray(content));
		putToDocumentCache(document, stickerUrl(file.id), content);
		LOG(("MtsLink Stickers: added %1 (%2 bytes, mime %3, %4)"
			).arg(file.id
			).arg(content.size()
			).arg(sticker.mime
			).arg(describeImage(content)));
		strong->data().stickers().mtsLinkSetFaved(document, true);
	});
}

void restoreLocalStickers(not_null<Main::Session*> session) {
	const auto weak = base::make_weak(session);
	RestoringLocalStickers = true;
	session->data().stickers().updated(
		Data::StickersType::Stickers
	) | rpl::on_next([=] {
		if (const auto strong = weak.get()
			; strong && !RestoringLocalStickers) {
			saveLocalStickers(strong);
		}
	}, session->lifetime());

	session->data().cache().get(stickersCacheKey(), [=](QByteArray &&data) {
		crl::on_main(weak, [=, data = std::move(data)] {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			const auto finish = gsl::finally([] {
				RestoringLocalStickers = false;
			});
			if (data.isEmpty()) {
				return;
			}
			QDataStream s(data);
			s.setVersion(QDataStream::Qt_5_1);
			auto version = qint32();
			auto count = qint32();
			s >> version >> count;
			if (s.status() != QDataStream::Ok
				|| (version != 1 && version != 2)
				|| count < 0) {
				return;
			}
			const auto readStickers = [&](int count) {
				auto result = Data::StickersPack();
				for (auto i = 0; i != count; ++i) {
					const auto sticker = readLocalSticker(s);
					if (!sticker) {
						return std::optional<Data::StickersPack>();
					}
					const auto document = makeLocalSticker(
						strong,
						*sticker,
						0);
					showLocalStickerFromCache(strong, document, sticker->key);
					result.push_back(document);
				}
				return std::make_optional(result);
			};
			const auto faved = readStickers(count);
			if (!faved) {
				return;
			}
			auto documents = std::vector<not_null<DocumentData*>>();
			for (const auto document : *faved) {
				documents.push_back(document);
			}
			auto packs = qint32();
			if (version == 2) {
				s >> packs;
				for (auto i = 0; i < packs; ++i) {
					auto setId = quint64();
					auto title = QString();
					auto size = qint32();
					s >> setId >> title >> size;
					if (s.status() != QDataStream::Ok || size < 0) {
						break;
					}
					const auto stickers = readStickers(size);
					if (!stickers) {
						break;
					}
					applyLocalPack(strong, setId, title, *stickers, false);
				}
				if (packs > 0) {
					strong->data().stickers().notifyUpdated(
						Data::StickersType::Stickers);
				}
			}
			// The first one is the newest: pushed to the front the last.
			for (const auto document : ranges::views::reverse(documents)) {
				strong->data().stickers().mtsLinkSetFaved(document, true);
			}
			LOG(("MtsLink Stickers: restored %1 faved, %2 packs"
				).arg(documents.size()
				).arg(packs));
		});
	});
}

namespace {

struct ImportedImage {
	QString name;
	QByteArray bytes;
};

[[nodiscard]] bool IsPackImageName(const QString &name) {
	const auto lower = name.toLower();
	return !lower.startsWith(u"__macosx/"_q)
		&& !QFileInfo(lower).fileName().startsWith('.')
		&& (lower.endsWith(u".png"_q)
			|| lower.endsWith(u".tgs"_q)
			|| lower.endsWith(u".webm"_q)
			|| lower.endsWith(u".webp"_q)
			|| lower.endsWith(u".jpg"_q)
			|| lower.endsWith(u".jpeg"_q));
}

[[nodiscard]] std::vector<ImportedImage> ReadPackZip(const QString &path) {
	constexpr auto kMaxZipSize = 100 * 1024 * 1024;
	constexpr auto kMaxImageSize = 10 * 1024 * 1024;
	auto result = std::vector<ImportedImage>();
	auto f = QFile(path);
	if (f.size() > kMaxZipSize || !f.open(QIODevice::ReadOnly)) {
		LOG(("MtsLink Stickers: can't read %1").arg(path));
		return result;
	}
	auto zip = zlib::FileToRead(f.readAll());
	if (zip.goToFirstFile() != UNZ_OK) {
		LOG(("MtsLink Stickers: not a zip %1").arg(path));
		return result;
	}
	do {
		const auto name = zip.getCurrentFileName();
		if (IsPackImageName(name)) {
			auto bytes = zip.readCurrentFileContent(kMaxImageSize);
			if (!bytes.isEmpty() && zip.error() == UNZ_OK) {
				result.push_back({ QFileInfo(name).fileName(), bytes });
			}
		}
	} while (zip.goToNextFile() == UNZ_OK
		&& int(result.size()) < kMaxPackStickers);
	return result;
}

// The sticker image: PNG or JPEG of a limited size (as sent to the chats),
// other formats and the big images are converted.
[[nodiscard]] std::optional<LocalSticker> PreparePackImage(
		ImportedImage &image) {
	constexpr auto kMaxAnimatedSize = 5 * 1024 * 1024;
	const auto lower = image.name.toLower();
	const auto tgs = lower.endsWith(u".tgs"_q);
	if (tgs || lower.endsWith(u".webm"_q)) {
		// Animated: as is, sent as a file with the sticker name.
		if (image.bytes.size() > kMaxAnimatedSize) {
			LOG(("MtsLink Stickers: too big %1").arg(image.name));
			return std::nullopt;
		}
		const auto base = QFileInfo(image.name).completeBaseName();
		return LocalSticker{
			.key = QUuid::createUuid().toString(QUuid::WithoutBraces),
			.name = tgs
				? image.name
				: isWebmStickerFile(image.name)
				? image.name
				: (base + QLatin1String(kWebmStickerSuffix)),
			.mime = tgs ? QString(kTgsMime) : u"video/webm"_q,
			.width = kAnimatedStickerSide,
			.height = kAnimatedStickerSide,
		};
	}
	auto name = image.name;
	auto mime = QString();
	auto width = 0;
	auto height = 0;
	if (!normalizeStickerImage(image.bytes, name, mime, width, height)) {
		LOG(("MtsLink Stickers: not an image %1").arg(image.name));
		return std::nullopt;
	}
	return LocalSticker{
		.key = QUuid::createUuid().toString(QUuid::WithoutBraces),
		.name = name,
		.mime = mime,
		.width = width,
		.height = height,
	};
}

} // namespace

void importStickerPack(
		not_null<Main::Session*> session,
		QPointer<QWidget> parent,
		Fn<void(QString)> showToast) {
	const auto weak = base::make_weak(session);
	FileDialog::GetOpenPaths(
		parent,
		tr::lng_mtslink_import_stickers_title(tr::now),
		u"Sticker packs (*.zip *.png *.webp *.jpg *.jpeg *.tgs *.webm);;"_q
			+ FileDialog::AllFilesFilter(),
		[=](FileDialog::OpenResult &&result) {
		const auto strong = weak.get();
		if (!strong || result.paths.isEmpty()) {
			return;
		}
		const auto &paths = result.paths;
		auto images = std::vector<ImportedImage>();
		auto title = QString();
		if (paths.size() == 1
			&& paths.front().endsWith(u".zip"_q, Qt::CaseInsensitive)) {
			images = ReadPackZip(paths.front());
			title = QFileInfo(paths.front()).completeBaseName();
		} else {
			for (const auto &path : paths) {
				if (!IsPackImageName(path)
					|| int(images.size()) >= kMaxPackStickers) {
					continue;
				}
				auto f = QFile(path);
				if (f.open(QIODevice::ReadOnly)) {
					images.push_back({ QFileInfo(path).fileName(), f.readAll() });
				}
			}
			title = QFileInfo(paths.front()).dir().dirName();
		}
		auto collator = QCollator();
		collator.setNumericMode(true);
		collator.setCaseSensitivity(Qt::CaseInsensitive);
		ranges::sort(images, [&](const ImportedImage &a, const ImportedImage &b) {
			return collator.compare(a.name, b.name) < 0;
		});
		auto pack = Data::StickersPack();
		for (auto &image : images) {
			const auto sticker = PreparePackImage(image);
			if (!sticker) {
				continue;
			}
			const auto document = makeLocalSticker(
				strong,
				*sticker,
				image.bytes.size());
			strong->data().cache().put(
				stickerBytesCacheKey(sticker->key),
				QByteArray(image.bytes));
			putToDocumentCache(document, stickerUrl(sticker->key), image.bytes);
			pack.push_back(document);
		}
		LOG(("MtsLink Stickers: import '%1' from %2 files: %3 stickers"
			).arg(title
			).arg(paths.size()
			).arg(pack.size()));
		if (pack.isEmpty()) {
			showToast(tr::lng_mtslink_import_stickers_empty(tr::now));
			return;
		}
		auto setId = uint64();
		do {
			setId = (base::RandomValue<uint64>() & 0x3FFF'FFFF'FFFF'FFFFULL)
				| 1ULL;
		} while (strong->data().stickers().sets().contains(setId));
		applyLocalPack(strong, setId, title, pack, true);
		strong->data().stickers().notifyUpdated(Data::StickersType::Stickers);
		showToast(tr::lng_mtslink_import_stickers_done(
			tr::now,
			lt_pack,
			title));
	});
}

namespace {

[[nodiscard]] Storage::Cache::Key imageFilesCacheKey() {
	return { kMtsLinkImageFilesTag, 0 };
}

} // namespace

void rememberImageAsFile(
		not_null<Main::Session*> session,
		const QString &fileId) {
	if (fileId.isEmpty() || ImageAsFileIds.contains(fileId)) {
		return;
	}
	ImageAsFileIds.insert(fileId);
	auto data = QByteArray();
	{
		QDataStream s(&data, QIODevice::WriteOnly);
		s.setVersion(QDataStream::Qt_5_1);
		s << qint32(1) << qint32(ImageAsFileIds.size());
		for (const auto &id : std::as_const(ImageAsFileIds)) {
			s << id;
		}
	}
	LOG(("MtsLink Files: image %1 sent as a file, %2 kept"
		).arg(fileId
		).arg(ImageAsFileIds.size()));
	session->data().cache().put(imageFilesCacheKey(), std::move(data));
}

void restoreImagesAsFiles(not_null<Main::Session*> session) {
	const auto weak = base::make_weak(session);
	session->data().cache().get(imageFilesCacheKey(), [=](
			QByteArray &&data) {
		crl::on_main(weak, [=, data = std::move(data)] {
			if (data.isEmpty()) {
				return;
			}
			QDataStream s(data);
			s.setVersion(QDataStream::Qt_5_1);
			auto version = qint32();
			auto count = qint32();
			s >> version >> count;
			if (s.status() != QDataStream::Ok || version != 1 || count < 0) {
				return;
			}
			for (auto i = 0; i != count; ++i) {
				auto id = QString();
				s >> id;
				if (s.status() != QDataStream::Ok) {
					break;
				}
				ImageAsFileIds.insert(id);
			}
			LOG(("MtsLink Files: %1 images sent as files restored"
				).arg(ImageAsFileIds.size()));
		});
	});
}

void rememberUploadedContent(
		not_null<Main::Session*> session,
		DocumentId localDocumentId,
		const QString &fileId,
		const QByteArray &content) {
	const auto document = session->data().document(localDocumentId);
	if (content.isEmpty() || !document->location(true).isEmpty()) {
		return; // Shown from its file on the disk.
	}
	putToDocumentCache(document, fileDownloadUrl(fileId), content);
}

void restoreSavedGifs(not_null<Main::Session*> session) {
	const auto weak = base::make_weak(session);
	// Not saved till restored: Telegram notifies about its own (empty)
	// list at the start, it overwrote the kept one.
	RestoringSavedGifs = true;
	session->data().stickers().savedGifsUpdated(
	) | rpl::on_next([=] {
		if (const auto strong = weak.get(); strong && !RestoringSavedGifs) {
			saveSavedGifs(strong);
		}
	}, session->lifetime());

	session->data().cache().get(savedGifsCacheKey(), [=](QByteArray &&data) {
		crl::on_main(weak, [=, data = std::move(data)] {
			const auto strong = weak.get();
			if (!strong) {
				return;
			} else if (data.isEmpty()) {
				RestoringSavedGifs = false;
				return;
			}
			const auto finish = gsl::finally([] {
				RestoringSavedGifs = false;
			});
			QDataStream s(data);
			s.setVersion(QDataStream::Qt_5_1);
			auto version = qint32();
			auto count = qint32();
			s >> version >> count;
			if (s.status() != QDataStream::Ok || version != 1 || count < 0) {
				return;
			}
			auto documents = QVector<DocumentData*>();
			for (auto i = 0; i != count; ++i) {
				auto f = Api::FileData();
				auto w = qint32();
				auto h = qint32();
				s >> f.id >> f.name >> f.size >> f.mime >> w >> h;
				if (s.status() != QDataStream::Ok) {
					return;
				}
				f.width = w;
				f.height = h;
				(void)buildFileMedia(strong, f, base::unixtime::now());
				const auto document = strong->data().document(
					DocumentId(uuidToBareId(f.id)));
				if (!document->isGifv()) {
					continue;
				}
				documents.push_back(document);
				GifBytesKept.insert(f.id);
				// Shown from the kept content, not from the server.
				const auto fileId = f.id;
				strong->data().cache().get(gifBytesCacheKey(fileId), [=](
						QByteArray &&bytes) {
					crl::on_main(weak, [=, bytes = std::move(bytes)] {
						if (weak.get() && !bytes.isEmpty()) {
							putToDocumentCache(
								document,
								fileDownloadUrl(fileId),
								bytes);
						}
					});
				});
			}
			LOG(("MtsLink Gifs: restored %1 GIFs").arg(documents.size()));
			auto &saved = strong->data().stickers().savedGifsRef();
			saved.clear();
			for (const auto document : documents) {
				saved.push_back(document);
			}
			strong->data().stickers().notifySavedGifsUpdated();
			// Telegram keeps the list as well (without the file urls).
			strong->local().writeSavedGifs();
		});
	});
}

bool sendSavedGif(
		not_null<Main::Session*> session,
		not_null<History*> history,
		not_null<DocumentData*> document,
		MsgId replyToId,
		MsgId topicRootId) {
	const auto i = GifFiles.constFind(document->id);
	const auto sticker = LocalStickers.constFind(document->id);
	const auto mts = session->account().mtsLinkSession();
	const auto peerId = history->peer->id;
	const auto chatId = peerIdToChatId(peerId);
	const auto isSticker = (sticker != LocalStickers.constEnd());
	if ((i == GifFiles.constEnd() && !isSticker) || !mts || chatId.isEmpty()) {
		LOG(("MtsLink Gifs: no file for the GIF %1").arg(document->id));
		return false;
	}
	const auto replyMtsId = replyToId
		? msgIdToMtsLinkId(peerId, replyToId)
		: QString();
	const auto parentMtsId = topicRootId
		? msgIdToMtsLinkId(peerId, topicRootId)
		: QString();
	const auto file = isSticker
		? Api::FileData{
			.id = sticker->key,
			.name = isImageMime(sticker->mime)
				? imageStickerFileName(sticker->name)
				: sticker->name,
			.mime = sticker->mime,
			.width = sticker->width,
			.height = sticker->height,
		}
		: i.value();
	const auto bytesKey = isSticker
		? stickerBytesCacheKey(sticker->key)
		: gifBytesCacheKey(file.id);
	const auto weak = base::make_weak(session);

	// The message with the content: a local one right away, as for a sent
	// file, the content is uploaded again (FILE_ALREADY_USED otherwise).
	const auto sendBytes = [=](QByteArray bytes) {
		const auto strong = weak.get();
		const auto mts = strong
			? strong->account().mtsLinkSession()
			: nullptr;
		if (!mts) {
			return;
		} else if (bytes.isEmpty()) {
			LOG(("MtsLink Gifs: no content of the file %1").arg(file.id));
			return;
		}
		const auto tempId = QUuid::createUuid().toString(
			QUuid::WithoutBraces);
		const auto isThreadSend = !parentMtsId.isEmpty();
		const auto clientId = isThreadSend
			? QUuid::createUuid().toString(QUuid::WithoutBraces)
			: QString();
		auto msg = Api::MessageData();
		msg.id = tempId;
		msg.chatId = chatId;
		msg.authorId = mts->userId();
		msg.createdAt = QDateTime::currentMSecsSinceEpoch();
		msg.type = MessageType::Text;
		if (replyMtsId != parentMtsId) {
			msg.repliedMessageId = replyMtsId;
		}
		msg.parentId = parentMtsId;
		auto local = file;
		local.id = tempId;
		local.size = bytes.size();
		msg.files.push_back(local);
		// The content is in the cache of the local document before it is
		// shown: not loaded by its temporary (not existing) url.
		const auto tempDocument = strong->data().document(
			DocumentId(uuidToBareId(tempId)));
		addMessage(strong, msg, isThreadSend);
		markLocalSending(
			strong,
			peerId,
			MsgId(uuidToBareId(tempId) & 0x7FFFFFFFLL));
		putToDocumentCache(tempDocument, fileDownloadUrl(tempId), bytes);
		// The upload progress is shown, as for a sent file.
		tempDocument->uploadingData = std::make_unique<Data::UploadState>(
			bytes.size());
		if (isThreadSend) {
			addPendingThreadSend(clientId);
		} else {
			setPendingTempMessage(
				peerId,
				MsgId(uuidToBareId(tempId) & 0x7FFFFFFFLL));
		}
		if (const auto history = strong->data().historyLoaded(peerId)) {
			strong->data().sendHistoryChangeNotifications();
			strong->changes().historyUpdated(
				history,
				Data::HistoryUpdate::Flag::MessageSent);
		}
		LOG(("MtsLink Gifs: uploading '%1' (%2 bytes, mime %3, %4) to %5"
			).arg(file.name
			).arg(bytes.size()
			).arg(file.mime
			).arg(describeImage(bytes)
			).arg(chatId));
		const auto localId = MsgId(uuidToBareId(tempId) & 0x7FFFFFFFLL);
		const auto upload = std::make_shared<Fn<void()>>();
		*upload = [=, self = std::weak_ptr<Fn<void()>>(upload)] {
		// Kept by the callbacks of the upload, may be run again.
		const auto keep = self.lock();
		const auto strong = weak.get();
		const auto mts = strong
			? strong->account().mtsLinkSession()
			: nullptr;
		if (!mts) {
			return;
		} else if (!mts->rpc() || !mts->rpc()->isConnected()) {
			LOG(("MtsLink Gifs: upload waits for the connection"));
			if (const auto again = self.lock()) {
				addPendingUpload(peerId, localId, [again] { (*again)(); });
			}
			return;
		}
		mts->files()->uploadFile(
			file.name.isEmpty() ? u"animation.gif"_q : file.name,
			bytes,
			file.mime,
			[=](const Api::UploadResult &result) {
				const auto strong = weak.get();
				const auto mts = strong
					? strong->account().mtsLinkSession()
					: nullptr;
				if (!mts) {
					return;
				}
				// The upload is over: no progress, the message is "sending"
				// (the clock) till the server sends it back.
				tempDocument->uploadingData = nullptr;
				tempDocument->owner().requestDocumentViewRepaint(tempDocument);
				// The real file is in the cache as well (the local message
				// gets its url).
				putToDocumentCache(
					tempDocument,
					fileDownloadUrl(result.id),
					bytes);
				trackSend(
					strong,
					peerId,
					MsgId(uuidToBareId(tempId) & 0x7FFFFFFFLL),
					chatId,
					QString(),
					QJsonArray(),
					QJsonArray(),
					(replyMtsId == parentMtsId) ? QString() : replyMtsId,
					QStringList{ result.id },
					parentMtsId,
					clientId);
			},
			[=](const QString &error) {
				LOG(("MtsLink Gifs: upload failed: %1").arg(error));
				if (error == u"disconnected"_q) {
					// Lost with the connection: uploaded after a reconnect.
					if (const auto again = keep) {
						addPendingUpload(
							peerId,
							localId,
							[again] { (*again)(); });
					}
					return;
				}
				if (const auto strong = weak.get()) {
					markLocalFailed(
						strong,
						peerId,
						MsgId(uuidToBareId(tempId) & 0x7FFFFFFFLL));
				}
				tempDocument->uploadingData = nullptr;
				tempDocument->owner().requestDocumentViewRepaint(
					tempDocument);
			},
			[=](qint64 sent, qint64 total) {
				if (const auto uploading = tempDocument->uploadingData.get()) {
					uploading->offset = sent;
					tempDocument->owner().requestDocumentViewRepaint(
						tempDocument);
				}
			});
		};
		(*upload)();
	};
	session->data().cache().get(bytesKey, [=](QByteArray &&bytes) {
		crl::on_main(weak, [=, bytes = std::move(bytes)]() mutable {
			if (!bytes.isEmpty()) {
				sendBytes(std::move(bytes));
			} else if (!isSticker) {
				loadFileBytes(document, file.id, sendBytes);
			} else {
				LOG(("MtsLink Stickers: no content of %1").arg(file.id));
			}
		});
	});
	return true;
}

} // namespace MtsLink

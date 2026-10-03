/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#include "mtslink/data_adapters.h"
#include "mtslink/session.h"
#include "mtslink/env_config.h"

#include "main/main_session.h"
#include "main/main_account.h"
#include "storage/storage_account.h"
#include "data/data_session.h"
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
#include "dialogs/dialogs_pinned_list.h"
#include "ui/text/format_values.h"
#include "ui/boxes/confirm_box.h"
#include "dialogs/dialogs_key.h"
#include "data/data_lastseen_status.h"
#include "data/data_types.h"
#include "data/notify/data_notify_settings.h"
#include "data/notify/data_peer_notify_settings.h"
#include "core/application.h"
#include "window/notifications_manager.h"
#include "base/unixtime.h"
#include "base/random.h"
#include "ui/image/image_location.h"
#include "ui/text/text_entity.h"
#include "ui/chat/group_call_bar.h"
#include "ui/chat/group_call_userpics.h"
#include "storage/cache/storage_cache_database.h"
#include "storage/storage_shared_media.h"

#include "core/click_handler_types.h"
#include "core/file_utilities.h"
#include "window/window_session_controller.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDataStream>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtGui/QGuiApplication>
#include <QtGui/QClipboard>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkCookie>

namespace MtsLink {

constexpr auto kMtsLinkMsgCacheTag = uint64(0xBC01'0000'0000'0000ULL);

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
QSet<QString> PinnedMessagesLoadedChats;
QSet<QString> ChatInfoLoadedChats;
QSet<QString> PendingThreadClientIds;
QHash<quint64, MsgId> ThreadRootMap;
QString FileAuthTokenValue;
QString FileRefreshTokenValue;
QList<QNetworkCookie> FileAuthCookies;
std::function<void()> TokenRefreshCallback;
bool TokenRefreshInProgress = false;
QHash<QString, QString> EmojiToIdMap;
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
}
PeerId FavoritesPeerIdValue = PeerId(0);

struct PendingChatEvent {
	QString dst;
	QJsonObject param;
};
QHash<QString, QList<PendingChatEvent>> PendingChatEvents;
QHash<QString, QList<PendingChatEvent>> PendingUserEvents;
QSet<QString> ChatInfoRequested;
QSet<QString> UserProfileRequested;
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
QSet<PeerId> PresenceKnownUsers;
QSet<PeerId> PresenceRequestedUsers;

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

MTPMessageMedia buildPhotoMedia(
		not_null<Main::Session*> session,
		const Api::FileData &file,
		TimeId date) {
	const auto thumbUrl = privateCdnThumbBase() + file.id + u"_s.jpg"_q;
	const auto fullUrl = privateCdnThumbBase() + file.id + u".jpg"_q;

	const auto photoId = PhotoId(uuidToBareId(file.id));

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

	if (isImageMime(file.mime) && file.width > 0 && file.height > 0) {
		return buildPhotoMedia(session, file, date);
	}

	const auto fileUrl = fileDownloadBase() + file.id + u"/download"_q;

	const auto isVideo = file.mime.startsWith(u"video/"_q);

	QVector<MTPDocumentAttribute> attrs;
	attrs.push_back(
		MTP_documentAttributeFilename(MTP_string(file.name)));
	if (isVideo) {
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

void applyUserpic(not_null<PeerData*> peer, const QString &fileId) {
	if (fileId.isEmpty()) {
		return;
	}
	const auto photoId = PhotoId(uuidToBareId(fileId));
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
				entities.push_back(EntityInText(
					EntityType::Pre,
					preStart,
					result.size() - preStart,
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

} // namespace

void handleNotificationEvent(
	not_null<Main::Session*> session,
	const QJsonObject &param);

void applyThreadsList(
		not_null<Main::Session*> session,
		const QList<Api::ThreadData> &threads) {
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

		const auto rootId = MsgId(
			uuidToBareId(thread.id) & 0x7FFFFFFFLL);

		const auto msgText = thread.message.value("text").toString();
		if (!msgText.isEmpty()) {
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
			history->addNewLocalMessage(
				HistoryItemCommonFields{
					.id = rootId,
					.flags = msgFlags,
					.from = fromId,
					.date = date,
				},
				parseMentionedText(
					msgText, markdown, mentions, session),
				MTP_messageMediaEmpty());
		}
		const auto parentPeerId = chatIdToPeerId(thread.chatId);
		ThreadTopicMap.insert(rootId, { thread.chatId, thread.id });
		ThreadPeerInfoMap.insert(peerId, { parentPeerId, rootId });
		ThreadReverseMap.insert({ parentPeerId, rootId }, peerId);

		const auto authorUuidForMap =
			thread.message.value("authorId").toString();
		if (!authorUuidForMap.isEmpty()) {
			const auto authorBare = uuidToBareId(authorUuidForMap);
			ThreadAuthorMap.insert(peerId, PeerId(::UserId(authorBare)));
		}

		session->data().refreshChatListEntry(
			Dialogs::Key(history));
	}

	LOG(("MtsLink: applied %1 threads as chats")
		.arg(threads.size()));
}

QList<Api::ChannelData> PendingChannelsList;
QList<Api::ChannelData> LoadedDialogsList;
bool DialogsApplied = false;
bool ActiveChatRestored = false;

void connectToSession(
		not_null<Main::Session*> mainSession,
		not_null<Session*> mtsSession) {
	ProfileDataPath = mainSession->account().local().basePath();
	loadCachedUserId();
	PendingChannelsList.clear();
	LoadedDialogsList.clear();
	DialogsApplied = false;
	ActiveChatRestored = false;
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
		[mtsSession] {
			mtsSession->messages()->retryFailedLoads();
			saveCachedUserId(mtsSession->userId());
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
				mtsSession->messages()->load(ch.id, {}, 1);
			}
		}
	};
	QObject::connect(
		mtsSession->channels(),
		&Api::Channels::channelsLoaded,
		[mainSession, refreshLastMessages](const QList<Api::ChannelData> &list) {
			if (DialogsApplied) {
				applyChatList(mainSession, list);
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
			refreshLastMessages(list);
			if (!PendingChannelsList.isEmpty()) {
				applyChatList(mainSession, PendingChannelsList);
				refreshLastMessages(PendingChannelsList);
				auto combined = list + PendingChannelsList;
				saveChatListToCache(mainSession, combined);
				PendingChannelsList.clear();
			}
		});
	QObject::connect(
		mtsSession->channels(),
		&Api::Channels::chatInfoLoaded,
		[mainSession, mtsSession](const Api::ChannelData &ch) {
			if (ch.type == ChatType::Dialog
				|| ch.type == ChatType::Favorites) {
				applyDialogData(mainSession, ch);
			} else {
				applyChannelData(mainSession, ch);
				if (!ch.isReadOnly) {
					mtsSession->users()->loadChatMembers(ch.id);
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
		&Api::Messages::messagesLoaded,
		[mainSession, mtsSession](
				const ChatId &chatId,
				const QList<Api::MessageData> &messages,
				const QList<Api::MemberProfile> &profiles,
				const QString &rawLastId,
				int rawCount) {
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
						setActiveCall(mainSession, peerId, cm.callMeta->joinLink);
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
				history->applyDialogTopMessage(
					newerItems.back()->id);
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
			} else if (name == "TypingEvent") {
				const auto tv = param.value("value").toObject();
				const auto tChatId = tv.value("chatId").toString();
				const auto tUserId = tv.value("userId").toString();
				if (!tChatId.isEmpty() && !tUserId.isEmpty()
					&& tUserId != mtsSession->userId()) {
					const auto peerId = chatIdToPeerId(tChatId);
					if (hasChatId(peerId)) {
						const auto h = mainSession->data()
							.historyLoaded(peerId);
						if (h) {
							const auto bId = uuidToBareId(tUserId);
							const auto u = mainSession->data()
								.user(::UserId(bId));
							mainSession->data()
								.sendActionManager().registerFor(
									h,
									MsgId(0),
									u,
									MTP_sendMessageTypingAction(),
									base::unixtime::now());
						}
					}
				}
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
						LOG(("[Presence] event %1 user=%2").arg(type).arg(userId));
						PresenceKnownUsers.insert(user->id);
						if (user->updateLastseen(status)) {
							mainSession->data().session().changes().peerUpdated(
								user,
								Data::PeerUpdate::Flag::OnlineStatus);
						}
					}
				} else if (type == "MemberInCallChanged") {
					const auto userId = value.value("userId").toString();
					if (!userId.isEmpty()) {
						const auto user = mainSession->data().user(
							::UserId(uuidToBareId(userId)));
						const auto inCall = value.value("inCall").toBool();
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
			LOG(("[Presence] GetMember user=%1 '%2' presence=%3 inCall=%4")
				.arg(userId)
				.arg(user->name())
				.arg(int(presence))
				.arg(inCall ? 1 : 0));
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
			const auto peerId = chatIdToPeerId(chatId);
			auto &stored = ChatMembersMap[peerId];
			stored.clear();
			stored.reserve(members.size());
			auto presenceRequests = 0;
			for (const auto &m : members) {
				applyUserData(mainSession, m);
				const auto bareId = uuidToBareId(m.userId);
				stored.push_back(bareId);
				const auto userPeerId = PeerId(::UserId(bareId));
				if (!PresenceKnownUsers.contains(userPeerId)
					&& !PresenceRequestedUsers.contains(userPeerId)) {
					PresenceRequestedUsers.insert(userPeerId);
					mtsSession->users()->loadPresence(
						m.userId,
						mtsSession->organizationId());
					++presenceRequests;
				}
			}
			LOG(("[Presence] chat=%1 members=%2 presenceRequests=%3")
				.arg(chatId)
				.arg(members.size())
				.arg(presenceRequests));
			const auto chatType = chatTypeForPeer(peerId);
			if (chatType == ChatType::Dialog) {
				const auto selfId = mtsSession->userId();
				for (const auto &m : members) {
					if (m.userId != selfId
							&& !m.avatarFileId.isEmpty()) {
						const auto peer = mainSession->data()
							.peerLoaded(peerId);
						if (peer && !peer->userpicPhotoId()) {
							applyUserpic(peer, m.avatarFileId);
						}
						break;
					}
				}
			}
			if (const auto channel = mainSession->data().channelLoaded(
					peerToChannel(peerId))) {
				if (const auto mega = channel->asMegagroup()) {
					if (mega->mgInfo) {
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
		[mainSession](const QList<Api::ThreadData> &threads) {
			applyThreadsList(mainSession, threads);
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

	mainSession->data().reactions().populateMtsLinkReactions({
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
	});

	mtsSession->users()->loadOrganizationMembers();
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

	user->setName(src.name, {}, {}, {});
	user->setIsContact(true);
	applyUserpic(user, src.avatarFileId);

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

void applyChannelData(
		not_null<Main::Session*> session,
		const Api::ChannelData &src) {
	const auto peerId = chatIdToPeerId(src.id, src.type);
	const auto channelId = peerToChannel(peerId);
	const auto channel = session->data().channel(channelId);

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
	{
		auto adminRights = ChatAdminRights(0);
		if (src.type == ChatType::Channel && !src.isReadOnly) {
			adminRights |= ChatAdminRight::PostMessages;
		}
		const auto isAdmin =
			src.memberRole.contains("Admin")
			|| src.memberRole.contains("Owner");
		if (isAdmin) {
			adminRights |= ChatAdminRight::EditMessages;
			adminRights |= ChatAdminRight::PinMessages;
			adminRights |= ChatAdminRight::DeleteMessages;
		}
		channel->setAdminRights(adminRights);
	}
	channel->setName(src.name, {});
	applyUserpic(channel, src.avatarFileId);
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
}

void applyUserData(
		not_null<Main::Session*> session,
		const Api::MemberProfile &src) {
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
	user->setName(
		first.isEmpty() ? display : first,
		last,
		{},
		display);
	user->setLoadedStatus(PeerData::LoadedStatus::Normal);
	user->removeFlags(UserDataFlag::Scam | UserDataFlag::Fake);
	user->setIsContact(true);
	applyUserpic(user, src.avatarFileId);

	if (!src.displayName.isEmpty()) {
		user->setUsername(src.displayName);
	}

	if (!src.phone.isEmpty()) {
		auto phone = src.phone;
		if (phone.startsWith('+')) {
			phone = phone.mid(1);
		}
		user->setPhone(phone);
	}

	QStringList aboutParts;
	if (!src.email.isEmpty()) {
		aboutParts << src.email;
	}
	if (!src.position.isEmpty()) {
		aboutParts << src.position;
	}
	if (!src.department.isEmpty()) {
		aboutParts << src.department;
	}
	if (!aboutParts.isEmpty()) {
		user->setAbout(aboutParts.join(QChar('\n')));
	}

	if (src.presence != MemberPresence::Unknown) {
		const auto status = (src.presence == MemberPresence::Online)
			? Data::LastseenStatus::OnlineTill(
				base::unixtime::now() + kMtsLinkOnlineHorizon)
			: Data::LastseenStatus::Recently();
		const auto wasOnline = (user->lastseen().onlineTill() > 0);
		const auto nowOnline = (src.presence == MemberPresence::Online);
		if (wasOnline != nowOnline) {
			LOG(("[Presence] applyUserData user=%1 %2 -> %3")
				.arg(src.userId)
				.arg(wasOnline ? "online" : "offline")
				.arg(nowOnline ? "online" : "offline"));
		}
		PresenceKnownUsers.insert(user->id);
		user->updateLastseen(status);
	}
	if (src.inCall >= 0) {
		if (src.inCall) {
			InCallUsers.insert(user->id);
		} else {
			InCallUsers.remove(user->id);
		}
	}

	auto flags = Data::PeerUpdate::Flag::Name
		| Data::PeerUpdate::Flag::Photo
		| Data::PeerUpdate::Flag::Username
		| Data::PeerUpdate::Flag::OnlineStatus;
	if (!src.phone.isEmpty()) {
		flags |= Data::PeerUpdate::Flag::PhoneNumber;
	}
	if (!aboutParts.isEmpty()) {
		flags |= Data::PeerUpdate::Flag::About;
	}
	session->changes().peerUpdated(user, flags);
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

	auto &members = ChatMembersMap[chatPeerId];
	if (std::find(members.begin(), members.end(), fromBareId) == members.end()) {
		members.push_back(fromBareId);
	}

	auto flags = (src.type == MessageType::Call)
		? MessageFlags(0)
		: MessageFlags(MessageFlag::HasFromId);
	const auto mts = session->account().mtsLinkSession();
	const auto myUserId = (mts && !mts->userId().isEmpty())
		? mts->userId()
		: CachedMyUserId;
	if (!myUserId.isEmpty() && src.authorId == myUserId) {
		flags |= MessageFlag::Outgoing;
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
		const auto parentBareId = uuidToBareId(src.parentId);
		const auto parentMsgId = MsgId(parentBareId & 0x7FFFFFFFLL);
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

	registerMessageId(chatPeerId, msgId, src.id);

	const auto existing = session->data().message(chatPeerId, msgId);
	if (existing) {
		if (!src.mentions.isEmpty()) {
			existing->setText(text);
			session->data().requestItemTextRefresh(existing);
			existing->invalidateChatListEntry();
		}
		if (!src.parentId.isEmpty()) {
			const auto parentBareId = uuidToBareId(src.parentId);
			const auto parentMsgId = MsgId(parentBareId & 0x7FFFFFFFLL);
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
		}
		applyThreadChildrenCount(existing, chatPeerId, msgId, src);
		if (src.type == MessageType::Call && src.callMeta) {
			existing->updateServiceText(buildCallServiceText(
				*src.callMeta,
				existing->out(),
				isPersonalChat(chatPeerId)));
			if (src.callMeta->status == u"Ended"_q) {
				existing->clearOngoingCallLink();
			}
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
		auto serviceText = buildCallServiceText(
			*src.callMeta,
			bool(fields.flags & MessageFlag::Outgoing),
			isPersonalChat(chatPeerId));
		const auto item = (threadOnly || batchItems)
			? history->makeMessage(
				std::move(fields),
				std::move(serviceText))
			: history->addNewExternalServiceMessage(
				std::move(fields),
				std::move(serviceText));
		if (item && batchItems) {
			batchItems->push_back(item);
		}
		if (item && src.callMeta->status == "Started"
			&& !src.callMeta->joinLink.isEmpty()) {
			const auto joinUrl = src.callMeta->joinLink;
			item->setOngoingCallLink(
				std::make_shared<LambdaClickHandler>([joinUrl] {
					File::OpenUrl(joinUrl);
				}));
			setActiveCall(session, chatPeerId, src.callMeta->joinLink);
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

		auto flags = (src.type == MessageType::Call)
			? MessageFlags(0)
			: MessageFlags(MessageFlag::HasFromId);
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
			auto serviceText = buildCallServiceText(
				*src.callMeta,
				bool(fields.flags & MessageFlag::Outgoing),
				isPersonalChat(chatPeerId));
			const auto callItem = history->makeMessage(
				std::move(fields),
				std::move(serviceText));
			if (callItem && src.callMeta->status == "Started"
				&& !src.callMeta->joinLink.isEmpty()) {
				const auto joinUrl = src.callMeta->joinLink;
				callItem->setOngoingCallLink(
					std::make_shared<LambdaClickHandler>([joinUrl] {
						File::OpenUrl(joinUrl);
					}));
			}
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
				setActiveCall(session, chatPeerId, cm.callMeta->joinLink);
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
				const auto threadBareId = uuidToBareId(threadId);
				const auto rootMsgId = MsgId(threadBareId & 0x7FFFFFFFLL);
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

void handleChatEvent(
		not_null<Main::Session*> session,
		const QString &dst,
		const QJsonObject &param) {
	const auto type = param.value("type").toString();
	const auto value = param.value("value").toObject();
	const auto isUserLevel = dst.startsWith(u"chat-user-"_q);
	const auto chatId = isUserLevel
		? value.value("chatId").toString()
		: extractChatIdFromDst(dst);
	LOG(("MtsLink Event: type=%1 chatId=%2 dst=%3")
		.arg(type).arg(chatId).arg(dst));

	if (chatId.isEmpty()) {
		return;
	}

	const auto chatKnown = ChatToPeerMap.contains(chatId);
	if (!chatKnown && type == "NewMessageV2Event") {
		PendingChatEvents[chatId].append({ dst, param });
		if (!ChatInfoRequested.contains(chatId)) {
			ChatInfoRequested.insert(chatId);
			const auto mts = session->account().mtsLinkSession();
			if (mts) {
				LOG(("MtsLink: unknown chatId %1, requesting info").arg(chatId));
				mts->channels()->loadChatInfo(chatId);
			}
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
		const auto authorId = m.value("authorId").toString();
		if (!authorId.isEmpty()) {
			const auto authorBareId = uuidToBareId(authorId);
			const auto authorPeerId = PeerId(::UserId(authorBareId));
			const auto user = session->data().userLoaded(
				peerToUser(authorPeerId));
			if (!user || user->name().isEmpty()) {
				PendingUserEvents[authorId].append({ dst, param });
				if (!UserProfileRequested.contains(authorId)) {
					UserProfileRequested.insert(authorId);
					const auto mts = session->account().mtsLinkSession();
					if (mts) {
						mts->users()->loadMember(
							authorId, mts->organizationId());
					}
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
			const auto meta = fo.value("meta").toObject()
				.value("value").toObject();
			msg.files.push_back(Api::FileData{
				.id = fo.value("id").toString(),
				.name = fo.value("name").toString(),
				.url = fo.value("url").toString(),
				.size = qint64(fo.value("size").toDouble()),
				.mime = fo.value("mime").toString(),
				.width = meta.value("width").toInt(),
				.height = meta.value("height").toInt(),
			});
		}
		if (msg.type == MessageType::Call) {
			const auto meta = m.value("metadata").toObject();
			if (meta.value("type").toString() == "CallMetadata") {
				const auto v = meta.value("value").toObject();
				msg.callMeta = Api::CallMetadata{
					.status = v.value("status").toString(),
					.joinLink = v.value("joinLink").toString(),
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
		LOG(("MtsLink NewMsg: id=%1 msgId=%2 isThread=%3 isThreadReply=%4 "
			"parentId=%5 text=%6")
			.arg(msg.id).arg(qint64(msgIdVal.bare))
			.arg(isThread).arg(isThreadReply)
			.arg(msg.parentId).arg(msg.text.left(50)));
		HistoryItem *newItem = nullptr;
		if (isThreadReply) {
			LOG(("MtsLink NewMsg: treating as thread reply"));
			replacePendingWithReal(session, chatPeerId, msg);
		} else if (!replacePendingWithReal(session, chatPeerId, msg)) {
			newItem = addMessage(session, msg, isThread);
			LOG(("MtsLink NewMsg: addMessage result=%1")
				.arg(newItem ? "ok" : "null"));
		} else {
			LOG(("MtsLink NewMsg: replaced pending message"));
		}
		{
			const auto mts = session->account().mtsLinkSession();
			const auto isOutgoing =
				mts && (msg.authorId == mts->userId());
			if (!isOutgoing) {
				const auto history =
					session->data().history(chatPeerId);
				const auto channel = session->data().channelLoaded(
					peerToChannel(chatPeerId));
				const auto skipUnread = isThread
					&& channel && channel->isBroadcast();
				if (!skipUnread && history->unreadCountKnown()) {
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
			}
		}
		if (isThread) {
			const auto parentBareId = uuidToBareId(msg.parentId);
			const auto parentMsgId = MsgId(parentBareId & 0x7FFFFFFFLL);
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
			if (threadIsOpen) {
				// Thread view is open — no scroll to parent needed.
			}
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
		const auto history = session->data().historyLoaded(peerId);
		if (!history) {
			return;
		}
		const auto count = value.value("unreadMessageCount").toInt();
		const auto localCount = history->unreadCount();
		const auto wasReadRequest = consumeReadRequestSent(eventChatId);
		if (count < localCount && !wasReadRequest) {
			return;
		}
		history->setUnreadCount(count);
		if (wasReadRequest && count == 0) {
			history->destroyUnreadBar();
			history->clearFirstUnreadMessage();
		}
		if (const auto last = history->lastMessage()) {
			last->invalidateChatListEntry();
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
			session->data().requestItemViewRefresh(item);
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
				setActiveCall(session, chatPeerId, joinLink);
			}
			if (item) {
				item->updateServiceText(buildCallServiceText(
					callMeta,
					item->out(),
					isPersonalChat(chatPeerId)));
				if (status == "Ended") {
					item->clearOngoingCallLink();
				} else if (status == "Started"
					&& !joinLink.isEmpty()) {
					item->setOngoingCallLink(
						std::make_shared<LambdaClickHandler>(
							[joinLink] { File::OpenUrl(joinLink); }));
				}
				session->data().requestItemViewRefresh(item);
			}
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
			.duration = int(meta.value("duration").toDouble() / 1000),
			.statusReason = meta.value("statusReasonV2").toString(
				meta.value("statusReason").toString()),
		};
		const auto &status = callMeta.status;
		const auto &joinLink = callMeta.joinLink;
		const auto chatPeerId = chatIdToPeerId(chatId);
		if (status == "Ended") {
			setActiveCall(session, chatPeerId, QString());
		} else if (status == "Started" && !joinLink.isEmpty()) {
			setActiveCall(session, chatPeerId, joinLink);
		}
		const auto csMapped = MtsLinkIdToMsgMap.constFind(messageId);
		const auto msgId = (csMapped != MtsLinkIdToMsgMap.constEnd())
			? csMapped.value().second
			: MsgId(uuidToBareId(messageId) & 0x7FFFFFFFLL);
		const auto item = session->data().message(chatPeerId, msgId);
		if (item) {
			item->updateServiceText(buildCallServiceText(
				callMeta,
				item->out(),
				isPersonalChat(chatPeerId)));
			if (status == "Ended") {
				item->clearOngoingCallLink();
			} else if (status == "Started" && !joinLink.isEmpty()) {
				item->setOngoingCallLink(
					std::make_shared<LambdaClickHandler>(
						[joinLink] { File::OpenUrl(joinLink); }));
			}
			session->data().requestItemViewRefresh(item);
		}
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
	const auto item = session->data().message(chatPeerId, msgId);
	if (item) {
		item->destroy();
		const auto hash = QCryptographicHash::hash(
			chatId.toUtf8(), QCryptographicHash::Md5);
		uint64 low = 0;
		memcpy(&low, hash.constData(), sizeof(low));
		session->data().cache().remove(
			Storage::Cache::Key{ kMtsLinkMsgCacheTag, low });
	}
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
	LOG(("MtsLink Pending: SET peerId=%1 tempMsgId=%2")
		.arg(peerId.value).arg(msgId.bare));
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

bool replacePendingWithReal(
		not_null<Main::Session*> session,
		PeerId peerId,
		const Api::MessageData &realMsg) {
	const auto it = PendingTempMessages.find(peerId);
	if (it == PendingTempMessages.end() || it->isEmpty()) {
		LOG(("MtsLink Pending: NOT FOUND for peerId=%1 realId='%2'")
			.arg(peerId.value).arg(realMsg.id));
		return false;
	}
	const auto tempMsgId = it->takeFirst();
	LOG(("MtsLink Pending: FOUND peerId=%1 tempMsgId=%2 realId='%3'")
		.arg(peerId.value).arg(tempMsgId.bare).arg(realMsg.id));
	if (it->isEmpty()) {
		PendingTempMessages.erase(it);
	}
	const auto item = session->data().message(peerId, tempMsgId);
	if (!item) {
		return false;
	}
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
				doc->uploadingData = nullptr;
				doc->setContentUrl(
					fileDownloadBase() + f.id + u"/download"_q);
			}
		}
	}
	item->setText(parseMentionedText(
		realMsg.text, realMsg.markdown, realMsg.mentions, session));
	registerMessageId(peerId, tempMsgId, realMsg.id);
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

static QHash<PeerId, MsgId> CurrentOpenThreads;

void setCurrentOpenThread(PeerId peerId, MsgId rootId) {
	CurrentOpenThreads[peerId] = rootId;
}

void clearCurrentOpenThread(PeerId peerId) {
	CurrentOpenThreads.remove(peerId);
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

	s << qint32(3); // format version
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
				<< qint32(f.width) << qint32(f.height);
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
	// version 2 adds callMeta, version 3 adds callMeta statusReason
	if (version < 1 || version > 3) {
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
			LOG(("MtsLink Cache: loaded %1 messages for chatId=%2")
				.arg(cached->messages.size())
				.arg(chatId));
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
					{ { f.title } },
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
	auto splits = splitSet.values();
	std::sort(splits.begin(), splits.end());

	QJsonArray blocks;
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
		value[u"text"_q] = segText;
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
	return EmojiToIdMap.value(emoji);
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
					[weak, peerId, msgId, chatId, conn](
							const ChatId &cid,
							const MessageId &,
							const QList<Api::MessageData> &messages,
							const QList<Api::MemberProfile> &profiles) {
						QObject::disconnect(*conn);
						if (cid != chatId) {
							return;
						}
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

void handleMtsLinkUrl(
		const QString &url,
		const QVariant &context) {
	LOG(("MtsLink Navigate: handleMtsLinkUrl url='%1'").arg(url));
	const auto parsed = QUrl(url);
	const auto path = parsed.path();
	if (tryNavigateDirectUrl(parsed, context)) {
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
				if (!tryNavigateDirectUrl(originUrl, context)) {
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
		not_null<PeerData*> peer) {
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
		File::OpenUrl(active);
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
			File::OpenUrl(joinLink);
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

void startCall(not_null<Main::Session*> session, not_null<PeerData*> peer) {
	if (session->windows().empty()) {
		return;
	}
	const auto active = !activeCallJoinLink(peer->id).isEmpty();
	const auto name = tr::bold(peer->name());
	auto text = active
		? tr::lng_mtslink_call_join_confirm(
			tr::now,
			lt_chat,
			name,
			tr::marked)
		: isPersonalChat(peer->id)
		? tr::lng_mtslink_call_confirm_personal(
			tr::now,
			lt_user,
			name,
			tr::marked)
		: tr::lng_mtslink_call_confirm_group(
			tr::now,
			lt_chat,
			name,
			tr::marked);
	const auto weak = base::make_weak(session);
	session->windows().front()->show(Ui::MakeConfirmBox({
		.text = std::move(text),
		.confirmed = [=](Fn<void()> close) {
			close();
			if (const auto strong = weak.get()) {
				performStartCall(strong, peer);
			}
		},
		.confirmText = (active
			? tr::lng_mtslink_call_join()
			: tr::lng_mtslink_call_start()),
	}));
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

} // namespace MtsLink

/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#include "mtslink/data_adapters.h"
#include "mtslink/session.h"
#include "mtslink/env_config.h"

#include "main/main_session.h"
#include "main/main_account.h"
#include "data/data_session.h"
#include "data/data_channel.h"
#include "data/data_chat_participant_status.h"
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
#include "history/history_item_reply_markup.h"
#include "history/view/history_view_send_action.h"
#include "dialogs/dialogs_main_list.h"
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
QList<QNetworkCookie> FileAuthCookies;
QHash<QString, QString> EmojiToIdMap;
QHash<QString, QString> IdToEmojiMap;
bool EmojiMapsInitialized = false;

QString emojiMapFilePath() {
	return cWorkingDir() + u"tdata/mtslink_emoji_map.json"_q;
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
QMap<QPair<PeerId, MsgId>, int> PendingThreadUnread;

[[nodiscard]] QString storageThumbBase() {
	return EnvConfig::instance().baseMediaUrl() + u"/thumb/"_q;
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

	const auto thumbUrl = storageThumbBase() + file.id + u"/S"_q;
	const auto fullUrl = storageThumbBase() + file.id + u"/XL"_q;

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
	const auto thumbUrl = storageThumbBase() + file.id + u"/S"_q;
	const auto fullUrl = storageThumbBase() + file.id + u"/XL"_q;

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
		const auto thumbUrl = storageThumbBase() + file.id + u"/S"_q;
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
		|| source.contains('[');
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

		result += unescaped[pos];
		++pos;
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

} // namespace

void handleNotificationEvent(
	not_null<Main::Session*> session,
	const QJsonObject &param);

void connectToSession(
		not_null<Main::Session*> mainSession,
		not_null<Session*> mtsSession) {
	loadChatListFromCache(mainSession);
	QObject::connect(
		mtsSession,
		&Session::initialized,
		mtsSession->messages(),
		[mtsSession] {
			mtsSession->messages()->retryFailedLoads();
		});
	QObject::connect(
		mtsSession->channels(),
		&Api::Channels::channelsLoaded,
		[mainSession](const QList<Api::ChannelData> &list) {
			applyChatList(mainSession, list);
			saveChatListToCache(mainSession, list);
		});
	QObject::connect(
		mtsSession->channels(),
		&Api::Channels::dialogsLoaded,
		[mainSession](const QList<Api::ChannelData> &list) {
			applyChatList(mainSession, list);
			saveChatListToCache(mainSession, list);
		});
	QObject::connect(
		mtsSession->channels(),
		&Api::Channels::chatInfoLoaded,
		[mainSession](const Api::ChannelData &ch) {
			LOG(("MtsLink: chatInfoLoaded '%1' type=%2 id=%3")
				.arg(ch.name)
				.arg(int(ch.type))
				.arg(ch.id));
			if (ch.type == ChatType::Dialog
				|| ch.type == ChatType::Favorites) {
				applyDialogData(mainSession, ch);
			} else {
				applyChannelData(mainSession, ch);
			}
			const auto pending = PendingChatEvents.take(ch.id);
			ChatInfoRequested.remove(ch.id);
			for (const auto &ev : pending) {
				handleChatEvent(mainSession, ev.dst, ev.param);
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
			for (const auto &p : profiles) {
				applyUserData(mainSession, p);
			}
			const auto perfStart = crl::now();
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
			int addedCount = 0;
			int skippedOlder = 0;
			for (int i = messages.size() - 1; i >= 0; --i) {
				const auto msgDate = TimeId(
					messages[i].createdAt / 1000);
				if (newestExistingDate > 0
					&& msgDate < newestExistingDate) {
					++skippedOlder;
					continue;
				}
				auto *item = addMessage(
					mainSession,
					messages[i],
					false,
					hasCachedMessages ? &newerItems : nullptr);
				if (item) {
					++addedCount;
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
			const auto perfEnd = crl::now();
			LOG(("MtsLink Paging: messagesLoaded chatId=%1 "
				"filtered=%2 added=%3 skippedOlder=%4 "
				"batchedNewer=%5 rawCount=%6 rawLastId=%7 "
				"elapsed=%8ms")
				.arg(chatId)
				.arg(messages.size())
				.arg(addedCount)
				.arg(skippedOlder)
				.arg(newerItems.size())
				.arg(rawCount)
				.arg(rawLastId)
				.arg(perfEnd - perfStart));
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
			LOG(("MtsLink WS event: name=%1 dst=%2")
				.arg(name).arg(dst));
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
								base::unixtime::now() + 300)
							: Data::LastseenStatus::Recently();
						if (user->updateLastseen(status)) {
							mainSession->data().session().changes().peerUpdated(
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
			for (const auto &m : members) {
				applyUserData(mainSession, m);
				const auto bareId = uuidToBareId(m.userId);
				stored.push_back(bareId);
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
						for (const auto bareId : stored) {
							if (const auto user = mainSession->data()
									.userLoaded(::UserId(bareId))) {
								mega->mgInfo->lastParticipants.push_back(
									user);
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
		session->data().setChatPinned(history, FilterId(), true);
		session->data().setChatPinned(history, FilterId(2), true);
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

	const auto status = (src.presence == MemberPresence::Online)
		? Data::LastseenStatus::OnlineTill(base::unixtime::now() + 300)
		: Data::LastseenStatus::Recently();
	user->updateLastseen(status);

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

HistoryItem *addMessage(
		not_null<Main::Session*> session,
		const Api::MessageData &src,
		bool threadOnly,
		std::vector<not_null<HistoryItem*>> *batchItems) {
	if (src.isDeleted) {
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

	auto flags = MessageFlags(MessageFlag::HasFromId);
	const auto mts = session->account().mtsLinkSession();
	if (mts && src.authorId == mts->userId()) {
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
		LOG(("MtsLink addMessage: EXISTING id=%1 msgId=%2 date=%3")
			.arg(src.id).arg(msgId.bare).arg(date));
	} else {
		LOG(("MtsLink addMessage: NEW id=%1 msgId=%2 date=%3")
			.arg(src.id).arg(msgId.bare).arg(date));
	}
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
		if (!existing->mainView() && !threadOnly) {
			if (batchItems) {
				batchItems->push_back(existing);
			} else {
				history->reattachToBlock(existing);
			}
		}
		return existing;
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
	if (item && src.threadChildrenCount > 0) {
		auto repliesData = HistoryMessageRepliesData();
		repliesData.isNull = false;
		repliesData.repliesCount = src.threadChildrenCount;
		repliesData.maxId = MsgId(src.threadChildrenCount);
		const auto key = qMakePair(chatPeerId, msgId);
		const auto pendingIt = PendingThreadUnread.find(key);
		if (pendingIt != PendingThreadUnread.end()) {
			repliesData.readMaxId = MsgId(
				src.threadChildrenCount - pendingIt.value());
			PendingThreadUnread.erase(pendingIt);
		} else {
			repliesData.readMaxId = MsgId(src.threadChildrenCount);
		}
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

	const auto olderStart = crl::now();
	LOG(("MtsLink Paging: addOlderMessages count=%1 rawCount=%2")
		.arg(messages.size())
		.arg(rawCount));

	if (rawCount == 0) {
		LOG(("MtsLink Paging: server returned empty, markLoadedAtTop"));
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
		if (mts && src.authorId == mts->userId()) {
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
	}

	LOG(("MtsLink Paging: newItems=%1, duplicates=%2 elapsed=%3ms")
		.arg(items.size())
		.arg(duplicates)
		.arg(crl::now() - olderStart));

	if (items.empty()) {
		if (duplicates > 0) {
			LOG(("MtsLink Paging: all duplicates, already loaded"));
			return true;
		}
		LOG(("MtsLink Paging: no visible items, cursor updated, need retry"));
		return false;
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
		const auto clientId = m.value("clientId").toString();
		const auto isThreadReply = takePendingThreadSend(clientId);
		const auto chatPeerId = chatIdToPeerId(chatId);
		const auto isThread = !msg.parentId.isEmpty();
		HistoryItem *newItem = nullptr;
		if (isThreadReply) {
			replacePendingWithReal(session, chatPeerId, msg);
		} else if (!replacePendingWithReal(session, chatPeerId, msg)) {
			newItem = addMessage(session, msg, isThread);
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
			if (parent) {
				if (const auto views = parent->Get<HistoryMessageViews>()) {
					auto data = HistoryMessageRepliesData();
					data.isNull = false;
					data.repliesCount = views->replies.count + 1;
					data.maxId = MsgId(data.repliesCount);
					parent->setReplies(std::move(data));
				}
				session->data().requestItemViewRefresh(parent);
			} else {
				const auto key = qMakePair(chatPeerId, parentMsgId);
				PendingThreadUnread[key]++;
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
		const auto msgBareId = uuidToBareId(messageId);
		const auto msgId = MsgId(msgBareId & 0x7FFFFFFFLL);
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
		const auto msgBareId = uuidToBareId(messageId);
		const auto msgId = MsgId(msgBareId & 0x7FFFFFFFLL);
		const auto item = session->data().message(chatPeerId, msgId);
		if (!item) {
			return;
		}
		QList<Api::ReactionData> reactions;
		const auto arr = value.value("reactions").toArray();
		for (const auto &r : arr) {
			const auto ro = r.toObject();
			const auto eid = ro.value("emojiId").toString();
			const auto emj = ro.value("emoji").toString();
			const auto cnt = ro.value("count").toInt();
			const auto sel = ro.value("selected").toBool();
			reactions.push_back({
				.emojiId = eid,
				.emoji = emj,
				.count = cnt,
				.selected = sel,
			});
		}
		session->data().reactions().clearMtsLinkSending(item);
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
		if (!emoji.isEmpty() && !emojiId.isEmpty()) {
			const auto isNew = !IdToEmojiMap.contains(emojiId)
				|| IdToEmojiMap[emojiId] != emoji;
			IdToEmojiMap[emojiId] = emoji;
			EmojiToIdMap[emoji] = emojiId;
			if (isNew) {
				saveEmojiMaps();
			}
		}
	}
}

void deleteMessage(
		not_null<Main::Session*> session,
		const ChatId &chatId,
		const MessageId &messageId) {
	const auto chatPeerId = chatIdToPeerId(chatId);
	const auto msgBareId = uuidToBareId(messageId);
	const auto msgId = MsgId(msgBareId & 0x7FFFFFFFLL);
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
	const auto msgBareId = uuidToBareId(src.id);
	const auto msgId = MsgId(msgBareId & 0x7FFFFFFFLL);
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
				const auto thumbUrl = storageThumbBase()
					+ f.id + u"/S"_q;
				const auto fullUrl = storageThumbBase()
					+ f.id + u"/XL"_q;
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

	s << qint32(1); // format version
	s << qint32(messages.size());
	for (const auto &m : messages) {
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
	if (version != 1) {
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
		p.presence = MemberPresence(presInt);
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

void setFileAuthToken(const QString &token) {
	FileAuthTokenValue = token;
}

QString fileAuthToken() {
	return FileAuthTokenValue;
}

void setFileAuthCookies(const QList<QNetworkCookie> &cookies) {
	FileAuthCookies = cookies;
	LOG(("MtsLink: stored %1 auth cookies").arg(cookies.size()));
}

QList<QNetworkCookie> fileAuthCookies() {
	return FileAuthCookies;
}

QSet<ChatId> MessageCacheLoadedChats;

void applyChatList(
		not_null<Main::Session*> session,
		const QList<Api::ChannelData> &channels) {
	LOG(("MtsLink Data: applyChatList count=%1").arg(channels.size()));
	for (const auto &ch : channels) {
		LOG(("MtsLink Data: chat '%1' type=%2 id=%3")
			.arg(ch.name)
			.arg(int(ch.type))
			.arg(ch.id));
		if (ch.type == ChatType::Dialog
			|| ch.type == ChatType::Favorites) {
			applyDialogData(session, ch);
		} else {
			applyChannelData(session, ch);
		}
	}
	session->data().chatsList()->setLoaded();
	for (const auto &ch : channels) {
		if (!MessageCacheLoadedChats.contains(ch.id)) {
			MessageCacheLoadedChats.insert(ch.id);
			loadMessagesFromCache(session, ch.id);
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

	if (tags.isEmpty()) {
		MtsLinkMessageContent plain;
		plain.text = text;
		return plain;
	}

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
		{u"```"_q, u"```"_q},
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
		} else {
			if (styleMap.contains(tag.id)) {
				styleRanges.push_back({
					tag.offset, tag.length, styleMap[tag.id]});
			}
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

	if (hits.isEmpty() && styleRanges.isEmpty()
		&& allMdMarkers.isEmpty()) {
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
	const auto peerId = chatIdToPeerId(chatId);
	if (!peerId) {
		return;
	}
	const auto my = context.value<ClickHandlerContext>();
	const auto controller = my.sessionWindow.get();
	if (!controller) {
		return;
	}

	if (!threadId.isEmpty()) {
		const auto rootBareId = uuidToBareId(threadId);
		const auto rootMsgId = MsgId(rootBareId & 0x7FFFFFFFLL);
		MsgId commentId = 0;
		if (!messageId.isEmpty()) {
			const auto msgBareId = uuidToBareId(messageId);
			commentId = MsgId(msgBareId & 0x7FFFFFFFLL);
		}
		const auto history = controller->session().data().history(peerId);
		controller->showRepliesForMessage(history, rootMsgId, commentId);
	} else if (!messageId.isEmpty()) {
		const auto bareId = uuidToBareId(messageId);
		const auto msgId = MsgId(bareId & 0x7FFFFFFFLL);
		const auto item = controller->session().data().message(
			peerId, msgId);
		if (item) {
			controller->showPeerHistory(
				peerId,
				Window::SectionShow::Way::Forward,
				msgId);
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
						const auto loaded = ctrl->session().data().message(
							peerId, msgId);
						if (loaded) {
							ctrl->showPeerHistory(
								peerId,
								Window::SectionShow::Way::Forward,
								msgId);
						} else {
							ctrl->showPeerHistory(
								peerId,
								Window::SectionShow::Way::Forward);
						}
					});
				mts->messages()->loadAround(chatId, messageId, 50);
			} else {
				controller->showPeerHistory(
					peerId,
					Window::SectionShow::Way::Forward);
			}
		}
	} else {
		controller->showPeerHistory(
			peerId,
			Window::SectionShow::Way::Forward);
	}
}

bool tryNavigateDirectUrl(
		const QUrl &parsed,
		const QVariant &context) {
	const auto path = parsed.path();
	static const auto re = QRegularExpression(
		u"^/chats/(?:channel|group|dialog)/([0-9a-f-]+)"
		"(?:/thread/([0-9a-f-]+))?"
		"(?:/message/([0-9a-f-]+))?$"_q);
	const auto match = re.match(path);
	if (!match.hasMatch()) {
		return false;
	}
	navigateToChat(
		match.captured(1), match.captured(2), match.captured(3), context);
	return true;
}

} // namespace

void handleMtsLinkUrl(
		const QString &url,
		const QVariant &context) {
	const auto parsed = QUrl(url);
	const auto path = parsed.path();
	if (tryNavigateDirectUrl(parsed, context)) {
		return;
	}
	if (!path.startsWith(u"/r/"_q)) {
		File::OpenUrl(url);
		return;
	}
	auto *nam = new QNetworkAccessManager();
	auto request = QNetworkRequest(parsed);
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::ManualRedirectPolicy);
	auto *reply = nam->head(request);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		const auto location = reply->header(
			QNetworkRequest::LocationHeader).toUrl();
		if (location.isValid()) {
			if (!tryNavigateDirectUrl(location, context)) {
				File::OpenUrl(location.toString());
			}
		} else {
			File::OpenUrl(url);
		}
		reply->deleteLater();
		nam->deleteLater();
	});
}

} // namespace MtsLink

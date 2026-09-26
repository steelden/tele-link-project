/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#include "mtslink/data_adapters.h"
#include "mtslink/session.h"

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
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_reply_markup.h"
#include "history/view/history_view_send_action.h"
#include "dialogs/dialogs_main_list.h"
#include "data/data_lastseen_status.h"
#include "data/data_types.h"
#include "base/unixtime.h"
#include "base/random.h"
#include "ui/image/image_location.h"
#include "ui/text/text_entity.h"
#include "storage/cache/storage_cache_database.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QRegularExpression>
#include <QtNetwork/QNetworkCookie>

namespace MtsLink {
namespace {

QHash<PeerId, QString> PeerToChatMap;
QHash<QString, PeerId> ChatToPeerMap;
QHash<PeerId, ChatType> PeerToChatTypeMap;
QHash<quint64, QString> MsgIdToMtsLinkIdMap;
QHash<PeerId, QString> OldestLoadedMsgMap;
QHash<uint64, QString> UserBareIdToUuidMap;
QHash<PeerId, std::vector<uint64>> ChatMembersMap;
QHash<PeerId, MsgId> PendingTempMessages;
QHash<QString, QPair<PeerId, MsgId>> MtsLinkIdToMsgMap;
QSet<QString> PinnedMessagesLoadedChats;
QString FileAuthTokenValue;
QList<QNetworkCookie> FileAuthCookies;
QHash<QString, QString> EmojiToIdMap;
QHash<QString, QString> IdToEmojiMap;
PeerId FavoritesPeerIdValue = PeerId(0);

const auto kStorageThumbBase =
	u"https://prod-storage-chat.mts-link.ru/thumb/"_q;
const auto kAvatarCdnBase =
	u"https://prod-cdn-thumb-public-chat.mts-link.ru/thumb_"_q;

quint64 makeMsgKey(PeerId peerId, MsgId msgId) {
	return (quint64(peerId.value) ^ (quint64(msgId.bare) << 32));
}

bool isImageMime(const QString &mime) {
	return mime.startsWith(u"image/"_q);
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

	const auto thumbUrl = kStorageThumbBase + file.id + u"/S"_q;
	const auto fullUrl = kStorageThumbBase + file.id + u"/XL"_q;

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
	const auto thumbUrl = kStorageThumbBase + file.id + u"/S"_q;
	const auto fullUrl = kStorageThumbBase + file.id + u"/XL"_q;

	const auto photoId = base::RandomValue<PhotoId>();

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

	session->data().cache().remove(Data::UrlCacheKey(thumbUrl));
	session->data().cache().remove(Data::UrlCacheKey(fullUrl));

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

	const auto fileUrl = u"https://prod-storage-chat.mts-link.ru/file/"_q
		+ file.id + u"/download"_q;

	QVector<MTPDocumentAttribute> attrs;
	attrs.push_back(
		MTP_documentAttributeFilename(MTP_string(file.name)));
	if (file.width > 0 && file.height > 0) {
		attrs.push_back(MTP_documentAttributeImageSize(
			MTP_int(file.width), MTP_int(file.height)));
	}

	const auto thumbUrl = kStorageThumbBase + file.id + u"/S"_q;
	auto thumbnail = ImageWithLocation{};
	if (!file.id.isEmpty()) {
		thumbnail = ImageWithLocation{
			.location = ImageLocation(
				DownloadLocation{ PlainUrlLocation{ thumbUrl } },
				320, 320),
		};
	}

	const auto docId = base::RandomValue<DocumentId>();
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
	const auto url = kAvatarCdnBase + fileId + u"_s.jpg"_q;
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
		const QList<Api::MentionInfo> &mentions,
		not_null<Main::Session*> session) {
	if (text.isEmpty() || !text.contains(u"<@u:"_q)) {
		auto result = TextWithEntities{ text };
		TextUtilities::ParseEntities(result, TextParseLinks);
		return result;
	}

	QHash<QString, QString> nameMap;
	for (const auto &m : mentions) {
		if (!m.userId.isEmpty() && !m.name.isEmpty()) {
			nameMap[m.userId] = m.name;
		}
	}

	static const auto re = QRegularExpression(
		QStringLiteral("<@u:([0-9a-f\\-]{36})>"));

	QString result;
	EntitiesInText entities;
	int pos = 0;
	auto it = re.globalMatch(text);
	while (it.hasNext()) {
		const auto match = it.next();
		result += text.mid(pos, match.capturedStart() - pos);

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
	result += text.mid(pos);
	auto parsed = TextWithEntities{ result, entities };
	TextUtilities::ParseEntities(parsed, TextParseLinks);
	return parsed;
}

} // namespace

void connectToSession(
		not_null<Main::Session*> mainSession,
		not_null<Session*> mtsSession) {
	QObject::connect(
		mtsSession->channels(),
		&Api::Channels::channelsLoaded,
		[mainSession](const QList<Api::ChannelData> &list) {
			applyChatList(mainSession, list);
		});
	QObject::connect(
		mtsSession->channels(),
		&Api::Channels::dialogsLoaded,
		[mainSession](const QList<Api::ChannelData> &list) {
			applyChatList(mainSession, list);
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
			int addedCount = 0;
			for (int i = messages.size() - 1; i >= 0; --i) {
				if (addMessage(mainSession, messages[i])) {
					++addedCount;
				}
			}
			LOG(("MtsLink Paging: messagesLoaded chatId=%1 "
				"filtered=%2 added=%3 rawCount=%4 rawLastId=%5")
				.arg(chatId)
				.arg(messages.size())
				.arg(addedCount)
				.arg(rawCount)
				.arg(rawLastId));
			const auto peerId = chatIdToPeerId(chatId);
			if (!rawLastId.isEmpty()
				&& oldestLoadedMessageId(peerId).isEmpty()) {
				setOldestLoadedMessageId(peerId, rawLastId);
			}
			if (!PinnedMessagesLoadedChats.contains(chatId)) {
				PinnedMessagesLoadedChats.insert(chatId);
				mtsSession->messages()->loadPinned(chatId, 5);
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
		mtsSession->messages(),
		&Api::Messages::pinnedMessagesLoaded,
		[mainSession](
				const ChatId &chatId,
				const QList<Api::MessageData> &messages,
				const QList<Api::MemberProfile> &profiles,
				int) {
			for (const auto &p : profiles) {
				applyUserData(mainSession, p);
			}
			for (const auto &src : messages) {
				const auto item = addMessage(mainSession, src);
				if (item) {
					item->setIsPinned(true);
				}
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
								base::unixtime::now() + 300)
							: Data::LastseenStatus::Recently();
						if (user->updateLastseen(status)) {
							mainSession->data().session().changes().peerUpdated(
								user,
								Data::PeerUpdate::Flag::OnlineStatus);
						}
					}
				}
			}
		});

	QObject::connect(
		mtsSession->users(),
		&Api::Users::memberLoaded,
		[mainSession](const Api::MemberProfile &profile) {
			applyUserData(mainSession, profile);
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
			for (const auto &src : messages) {
				const auto item = addMessage(mainSession, src);
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
			}
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

ChatType chatTypeForPeer(PeerId peerId) {
	return PeerToChatTypeMap.value(peerId, ChatType::Channel);
}

PeerId favoritesPeerId() {
	return FavoritesPeerIdValue;
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
	const auto date = src.lastMessageTimestamp
		? TimeId(src.lastMessageTimestamp / 1000)
		: TimeId(1);

	history->setChatListTimeId(date);
	history->setUnreadCount(src.unreadCount);

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

	if (src.type == ChatType::Channel) {
		flags |= ChannelDataFlag::Megagroup;
		flags &= ~ChannelDataFlag::Broadcast;
	} else {
		flags |= ChannelDataFlag::Megagroup;
		flags &= ~ChannelDataFlag::Broadcast;
	}
	channel->setFlags(flags);
	{
		auto adminRights = ChatAdminRights(0);
		if (src.type == ChatType::Channel) {
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
		if (adminRights) {
			channel->setAdminRights(adminRights);
		}
	}
	channel->setName(src.name, {});
	applyUserpic(channel, src.avatarFileId);

	const auto history = session->data().history(channel->id);
	if (!history->folderKnown()) {
		history->clearFolder();
	}

	const auto date = src.lastMessageTimestamp
		? TimeId(src.lastMessageTimestamp / 1000)
		: TimeId(1);

	history->setChatListTimeId(date);
	history->setUnreadCount(src.unreadCount);
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
	if (isSelf) {
		LOG(("MtsLink: applying SELF user data: '%1 %2' display='%3' avatar='%4'")
			.arg(first, last, display, src.avatarFileId));
	}
	user->setName(
		first.isEmpty() ? display : first,
		last,
		{},
		display);
	user->setLoadedStatus(PeerData::LoadedStatus::Normal);
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

	auto flags = Data::PeerUpdate::Flag::Name
		| Data::PeerUpdate::Flag::Photo
		| Data::PeerUpdate::Flag::Username;
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
		const Api::MessageData &src) {
	if (src.isDeleted) {
		return nullptr;
	}
	const auto chatPeerId = chatIdToPeerId(src.chatId);

	const auto history = session->data().history(chatPeerId);

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

	auto text = parseMentionedText(src.text, src.mentions, session);

	registerMessageId(chatPeerId, msgId, src.id);

	const auto existing = session->data().message(chatPeerId, msgId);
	if (existing) {
		if (!src.mentions.isEmpty()) {
			existing->setText(text);
			session->data().requestItemTextRefresh(existing);
			existing->invalidateChatListEntry();
		}
		return existing;
	}

	const auto media = (!src.files.isEmpty())
		? buildFileMedia(session, src.files.first(), date)
		: MTP_messageMediaEmpty();

	const auto item = history->addNewExternalMessage(
		std::move(fields),
		std::move(text),
		media);
	if (item && !src.files.isEmpty()) {
		reapplyPhotoUrls(item, src.files.first());
	}
	if (item && src.threadChildrenCount > 0) {
		auto repliesData = HistoryMessageRepliesData();
		repliesData.isNull = false;
		repliesData.repliesCount = src.threadChildrenCount;
		item->setReplies(std::move(repliesData));
	}
	if (item && src.updatedAt > 0 && src.updatedAt != src.createdAt) {
		item->setEditDate(TimeId(src.updatedAt / 1000));
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

		auto text = parseMentionedText(src.text, src.mentions, session);

		registerMessageId(chatPeerId, msgId, src.id);

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
	}

	LOG(("MtsLink Paging: newItems=%1, duplicates=%2")
		.arg(items.size())
		.arg(duplicates));

	if (items.empty()) {
		if (duplicates > 0) {
			LOG(("MtsLink Paging: all duplicates, already loaded"));
			return true;
		}
		LOG(("MtsLink Paging: no visible items, cursor updated, need retry"));
		return false;
	}

	history->addCreatedOlderSlice(items);

	session->data().notifyHistoryChangeDelayed(history);
	session->data().sendHistoryChangeNotifications();

	return true;
}

void handleChatEvent(
		not_null<Main::Session*> session,
		const QString &dst,
		const QJsonObject &param) {
	const auto type = param.value("type").toString();
	const auto value = param.value("value").toObject();
	const auto chatId = extractChatIdFromDst(dst);

	if (chatId.isEmpty()) {
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
		Api::MessageData msg;
		msg.id = m.value("id").toString();
		msg.chatId = chatId;
		msg.authorId = m.value("authorId").toString();
		msg.text = m.value("text").toString();
		msg.markdown = m.value("markdown").toString();
		msg.createdAt = parseTimestamp(m, "createdAtMs", "createdAt");
		msg.updatedAt = parseTimestamp(m, "updatedAtMs", "updatedAt");
		const auto repliedMsg = m.value("repliedMessage").toObject();
		msg.repliedMessageId = repliedMsg.value("id").toString();
		msg.type = MessageType::Text;
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
		const auto chatPeerId = chatIdToPeerId(chatId);
		if (!replacePendingWithReal(session, chatPeerId, msg)) {
			addMessage(session, msg);
		}
	} else if (type == "MessageDeletedEvent") {
		const auto messageId = value.value("messageId").toString();
		deleteMessage(session, chatId, messageId);
	} else if (type == "MessageUpdatedV2Event") {
		const auto messageId = value.value("messageId").toString();
		const auto newText = value.value("text").toString();
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
				newText, mentions, session));
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
			mts->messages()->loadPinned(chatId);
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
		history->setUnreadCount(count);
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
		item->setText(parseMentionedText(src.text, src.mentions, session));
		session->data().requestItemTextRefresh(item);
	}
}

void registerMessageId(PeerId peerId, MsgId msgId, const QString &mtsLinkId) {
	MsgIdToMtsLinkIdMap.insert(makeMsgKey(peerId, msgId), mtsLinkId);
	MtsLinkIdToMsgMap.insert(mtsLinkId, {peerId, msgId});
}

void setPendingTempMessage(PeerId peerId, MsgId msgId) {
	PendingTempMessages[peerId] = msgId;
}

void clearPendingTempMessage(
		not_null<Main::Session*> session,
		PeerId peerId) {
	const auto it = PendingTempMessages.find(peerId);
	if (it == PendingTempMessages.end()) {
		return;
	}
	const auto tempMsgId = it.value();
	PendingTempMessages.erase(it);
	if (const auto item = session->data().message(peerId, tempMsgId)) {
		item->destroy();
	}
}

bool replacePendingWithReal(
		not_null<Main::Session*> session,
		PeerId peerId,
		const Api::MessageData &realMsg) {
	const auto it = PendingTempMessages.find(peerId);
	if (it == PendingTempMessages.end()) {
		return false;
	}
	const auto tempMsgId = it.value();
	PendingTempMessages.erase(it);
	const auto item = session->data().message(peerId, tempMsgId);
	if (!item) {
		return false;
	}
	if (const auto media = item->media()) {
		if (const auto doc = media->document()) {
			doc->uploadingData = nullptr;
			if (!realMsg.files.isEmpty()) {
				const auto &f = realMsg.files.first();
				doc->setContentUrl(
					u"https://prod-storage-chat.mts-link.ru/file/"_q
					+ f.id + u"/download"_q);
			}
		}
	}
	item->setText(parseMentionedText(
		realMsg.text, realMsg.mentions, session));
	registerMessageId(peerId, tempMsgId, realMsg.id);
	session->data().requestItemTextRefresh(item);
	item->invalidateChatListEntry();
	return true;
}

QString msgIdToMtsLinkId(PeerId peerId, MsgId msgId) {
	return MsgIdToMtsLinkIdMap.value(makeMsgKey(peerId, msgId));
}

void setOldestLoadedMessageId(PeerId peerId, const QString &mtsLinkId) {
	OldestLoadedMsgMap[peerId] = mtsLinkId;
}

QString oldestLoadedMessageId(PeerId peerId) {
	return OldestLoadedMsgMap.value(peerId);
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
}

QString userBareIdToUuid(uint64 bareId) {
	return UserBareIdToUuidMap.value(bareId);
}

MtsLinkMessageContent convertMentionsForSending(
		const TextWithTags &textWithTags,
		not_null<Main::Session*> session) {
	const auto &text = textWithTags.text;
	const auto &tags = textWithTags.tags;

	struct MentionHit {
		int offset = 0;
		int length = 0;
		QString uuid;
		QString displayName;
	};
	QList<MentionHit> hits;
	for (const auto &tag : tags) {
		if (!TextUtilities::IsMentionLink(tag.id)) {
			continue;
		}
		const auto data = TextUtilities::MentionEntityData(tag.id);
		if (data.isEmpty()) {
			continue;
		}
		const auto fields = TextUtilities::MentionNameDataToFields(data);
		const auto mentionUuid = userBareIdToUuid(fields.userId);
		if (mentionUuid.isEmpty()) {
			continue;
		}
		MentionHit hit;
		hit.offset = tag.offset;
		hit.length = tag.length;
		hit.uuid = mentionUuid;
		hit.displayName = text.mid(tag.offset, tag.length);
		hits.push_back(std::move(hit));
	}

	if (hits.isEmpty()) {
		MtsLinkMessageContent plain;
		plain.text = text;
		return plain;
	}

	QString mtsText;
	QJsonArray blocks;
	QJsonArray mentionsMeta;
	QSet<QString> addedMentions;
	int pos = 0;

	const auto mts = session->account().mtsLinkSession();
	const auto orgId = mts ? mts->organizationId() : QString();

	for (const auto &hit : hits) {
		const auto before = text.mid(pos, hit.offset - pos);
		if (!before.isEmpty()) {
			blocks.append(QJsonObject{
				{QStringLiteral("type"), QStringLiteral("TextElement")},
				{QStringLiteral("value"), QJsonObject{{QStringLiteral("text"), before}}},
			});
		}
		mtsText += before;
		mtsText += QStringLiteral("<@u:") + hit.uuid + QStringLiteral(">");

		blocks.append(QJsonObject{
			{QStringLiteral("type"), QStringLiteral("MentionElement")},
			{QStringLiteral("value"), QJsonObject{
				{QStringLiteral("id"), hit.uuid},
				{QStringLiteral("type"), QStringLiteral("User")},
				{QStringLiteral("organizationId"), orgId},
			}},
		});

		if (!addedMentions.contains(hit.uuid)) {
			addedMentions.insert(hit.uuid);
			mentionsMeta.append(QJsonObject{
				{QStringLiteral("id"), hit.uuid},
				{QStringLiteral("type"), QStringLiteral("User")},
				{QStringLiteral("name"), hit.displayName},
			});
		}
		pos = hit.offset + hit.length;
	}
	const auto tail = text.mid(pos);
	if (!tail.isEmpty()) {
		blocks.append(QJsonObject{
			{QStringLiteral("type"), QStringLiteral("TextElement")},
			{QStringLiteral("value"), QJsonObject{{QStringLiteral("text"), tail}}},
		});
	}
	mtsText += tail;

	MtsLinkMessageContent result;
	result.text = mtsText;
	result.blocks = blocks;
	result.mentionsMeta = mentionsMeta;
	return result;
}

void setEmojiMapping(const QHash<QString, QString> &emojiToId) {
	EmojiToIdMap = emojiToId;
	IdToEmojiMap.clear();
	for (auto it = emojiToId.constBegin(); it != emojiToId.constEnd(); ++it) {
		IdToEmojiMap[it.value()] = it.key();
	}
}

QString emojiToId(const QString &emoji) {
	return EmojiToIdMap.value(emoji);
}

QString idToEmoji(const QString &emojiId) {
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

} // namespace MtsLink

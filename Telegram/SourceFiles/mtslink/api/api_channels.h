/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#pragma once

#include "mtslink/types.h"

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
	int pinnedMessageCount = 0;
	QString memberRole;
	QString interlocutorId;
};

class Channels final : public QObject {
	Q_OBJECT

public:
	explicit Channels(Rpc *rpc, QObject *parent = nullptr);

	void loadMyChannels();
	void loadMyDialogsAndGroupChats();
	void loadChatInfo(const ChatId &chatId);

Q_SIGNALS:
	void channelsLoaded(const QList<ChannelData> &channels);
	void dialogsLoaded(const QList<ChannelData> &dialogs);
	void chatInfoLoaded(const ChannelData &chat);

private:
	[[nodiscard]] ChannelData parseChat(const QJsonObject &obj) const;
	[[nodiscard]] ChatType parseChatType(const QString &type) const;

	Rpc *_rpc = nullptr;
};

} // namespace MtsLink::Api

/*
This file is part of MTS Link Desktop,
the unofficial desktop client for MTS Link Chats,
based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/nicegram/nicegram-desktop/blob/master/LEGAL
*/
#pragma once

#include <QString>
#include <QUuid>

namespace MtsLink {

using ChatId = QString;
using MessageId = QString;
using UserId = QString;
using OrganizationId = QString;
using TeamId = QString;
using FileId = QString;
using EmojiId = QString;
using ClientId = QString;

enum class ChatType {
	Dialog,
	Channel,
	GroupChat,
	Discussion,
	Favorites,
	Team,
};

enum class MessageType {
	Text,
	Call,
	System,
	Forward,
};

enum class MemberPresence {
	Online,
	Offline,
	Away,
};

enum class MemberRole {
	Owner,
	Admin,
	Member,
	Guest,
};

} // namespace MtsLink

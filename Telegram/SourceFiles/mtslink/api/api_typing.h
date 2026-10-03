/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#pragma once

#include "mtslink/types.h"

#include <QObject>

namespace MtsLink {
class Rpc;
} // namespace MtsLink

namespace MtsLink::Api {

class Typing final : public QObject {
	Q_OBJECT

public:
	explicit Typing(Rpc *rpc, QObject *parent = nullptr);

	// threadId is empty for the chat itself.
	void sendTyping(const ChatId &chatId, const MessageId &threadId = {});
	// Events come in "typing-chat-<id>" / "typing-thread-<id>" streams.
	void subscribeToChat(const ChatId &chatId);
	void subscribeToThread(const ChatId &chatId, const MessageId &threadId);

Q_SIGNALS:
	void userTyping(
		const ChatId &chatId,
		const UserId &userId);

private:
	Rpc *_rpc = nullptr;
};

} // namespace MtsLink::Api

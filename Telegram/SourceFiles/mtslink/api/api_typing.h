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

	void sendTyping(const ChatId &chatId);
	void subscribeToChat(const ChatId &chatId);

Q_SIGNALS:
	void userTyping(
		const ChatId &chatId,
		const UserId &userId);

private:
	Rpc *_rpc = nullptr;
};

} // namespace MtsLink::Api

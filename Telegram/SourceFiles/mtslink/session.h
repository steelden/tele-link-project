/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#pragma once

#include "mtslink/rpc.h"
#include "mtslink/types.h"
#include "mtslink/api/api_auth.h"
#include "mtslink/api/api_channels.h"
#include "mtslink/api/api_messages.h"
#include "mtslink/api/api_users.h"
#include "mtslink/api/api_sending.h"
#include "mtslink/api/api_typing.h"
#include "mtslink/api/api_files.h"
#include "mtslink/api/api_threads.h"

#include <QObject>
#include <QTimer>

namespace MtsLink {

class Session final : public QObject {
	Q_OBJECT

public:
	explicit Session(QObject *parent = nullptr);
	~Session();

	void start(const QString &token, bool needsRefresh = false);
	void stop();

	[[nodiscard]] Rpc *rpc();
	[[nodiscard]] Api::Auth *auth();
	[[nodiscard]] Api::Channels *channels();
	[[nodiscard]] Api::Messages *messages();
	[[nodiscard]] Api::Users *users();
	[[nodiscard]] Api::Sending *sending();
	[[nodiscard]] Api::Typing *typing();
	[[nodiscard]] Api::Files *files();
	[[nodiscard]] Api::Threads *threads();

	[[nodiscard]] UserId userId() const;
	[[nodiscard]] OrganizationId organizationId() const;
	[[nodiscard]] bool isActive() const;
	[[nodiscard]] QString token() const;

Q_SIGNALS:
	void started();
	void stopped();
	void initialized();
	void chatListReady();
	void authExpired();

private:
	void onConnected();
	void onAuthError(const QString &errorText);
	void runInitSequence();
	void subscribeToEvents();
	void loadChatLists();

	Rpc _rpc;
	Api::Auth _auth;
	std::unique_ptr<Api::Channels> _channels;
	std::unique_ptr<Api::Messages> _messages;
	std::unique_ptr<Api::Users> _users;
	std::unique_ptr<Api::Sending> _sending;
	std::unique_ptr<Api::Typing> _typing;
	std::unique_ptr<Api::Files> _files;
	std::unique_ptr<Api::Threads> _threads;

	void scheduleReconnect();
	void doReconnect();

	UserId _userId;
	OrganizationId _organizationId;
	QString _token;
	bool _active = false;
	bool _manualStop = false;
	int _initPendingCalls = 0;
	QTimer _reconnectTimer;
	int _reconnectDelay = 0;
};

} // namespace MtsLink

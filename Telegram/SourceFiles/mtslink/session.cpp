/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#include "mtslink/session.h"
#include "mtslink/data_adapters.h"
#include "mtslink/env_config.h"

namespace MtsLink {

namespace {
constexpr auto kReconnectInitialMs = 1000;
constexpr auto kReconnectMaxMs = 30000;
} // namespace

Session::Session(QObject *parent)
: QObject(parent) {
	_reconnectTimer.setSingleShot(true);
	QObject::connect(
		&_reconnectTimer,
		&QTimer::timeout,
		this,
		&Session::doReconnect);
	QObject::connect(
		&_rpc,
		&Rpc::connected,
		this,
		&Session::onConnected);
	QObject::connect(
		&_rpc,
		&Rpc::disconnected,
		this,
		[this] {
			_active = false;
			Q_EMIT stopped();
			if (!_manualStop) {
				scheduleReconnect();
			}
		});
	QObject::connect(
		&_rpc,
		&Rpc::authError,
		this,
		&Session::onAuthError);
	QObject::connect(
		&_auth,
		&Api::Auth::tokenRefreshed,
		this,
		[this](const QString &newToken) {
			LOG(("MtsLink Session: token refreshed, reconnecting"));
			_token = newToken;
			MtsLink::setFileAuthToken(newToken);
			_rpc.connectAndAuth(newToken);
		});
	QObject::connect(
		&_auth,
		&Api::Auth::authFailed,
		this,
		[this](const QString &error) {
			LOG(("MtsLink Session: token refresh failed: %1, "
				"logging out").arg(error));
			Q_EMIT authExpired();
		});
}

Session::~Session() {
	stop();
}

void Session::start(const QString &token, bool needsRefresh) {
	_token = token;
	MtsLink::setFileAuthToken(token);
	_manualStop = false;
	_reconnectDelay = 0;
	_channels = std::make_unique<Api::Channels>(&_rpc);
	_messages = std::make_unique<Api::Messages>(&_rpc);
	_users = std::make_unique<Api::Users>(&_rpc);
	_sending = std::make_unique<Api::Sending>(&_rpc);
	_typing = std::make_unique<Api::Typing>(&_rpc);
	_files = std::make_unique<Api::Files>(&_rpc);
	_threads = std::make_unique<Api::Threads>(&_rpc);

	auto &env = EnvConfig::instance();
	const auto doConnect = [this, needsRefresh] {
		if (needsRefresh) {
			LOG(("MtsLink Session: refreshing token before connect"));
			_auth.refreshTokens();
		} else {
			LOG(("MtsLink Session: fresh login, connecting directly"));
			_rpc.connectAndAuth(_token);
		}
	};
	if (env.isLoaded()) {
		doConnect();
	} else {
		QObject::connect(&env, &EnvConfig::loaded, this, [this, doConnect] {
			if (!_manualStop) {
				doConnect();
			}
		}, Qt::SingleShotConnection);
		QObject::connect(&env, &EnvConfig::loadFailed, this, [this, doConnect] {
			LOG(("MtsLink Session: env-config failed, connecting with defaults"));
			if (!_manualStop) {
				doConnect();
			}
		}, Qt::SingleShotConnection);
		env.load(u"https://my.mts-link.ru/chats/env-config.js"_q);
	}
}

void Session::stop() {
	_manualStop = true;
	_reconnectTimer.stop();
	_active = false;
	// Pending RPC fail handlers capture the API objects, cancel them first.
	_rpc.disconnect();
	_channels.reset();
	_messages.reset();
	_users.reset();
	_sending.reset();
	_typing.reset();
	_files.reset();
	_threads.reset();
}

Rpc *Session::rpc() { return &_rpc; }
Api::Auth *Session::auth() { return &_auth; }
Api::Channels *Session::channels() { return _channels.get(); }
Api::Messages *Session::messages() { return _messages.get(); }
Api::Users *Session::users() { return _users.get(); }
Api::Sending *Session::sending() { return _sending.get(); }
Api::Typing *Session::typing() { return _typing.get(); }
Api::Files *Session::files() { return _files.get(); }
Api::Threads *Session::threads() { return _threads.get(); }

UserId Session::userId() const { return _userId; }
OrganizationId Session::organizationId() const { return _organizationId; }
bool Session::isActive() const { return _active; }
QString Session::token() const { return _token; }

void Session::onAuthError(const QString &errorText) {
	LOG(("MtsLink Session: auth error: %1, trying token refresh")
		.arg(errorText));
	_active = false;
	_rpc.disconnect();
	_auth.refreshTokens();
}

void Session::onConnected() {
	LOG(("MtsLink Session: WS connected, running init sequence"));
	_reconnectDelay = 0;
	_active = true;
	Q_EMIT started();
	runInitSequence();
}

void Session::runInitSequence() {
	// Step 1: Get organizations to find userId and orgId.
	_rpc.call(
		"Organization.GetOrganizationsByUserV2",
		QJsonObject{},
		[this](const QJsonObject &result) {
			const auto value = result.value("value").toObject();
			const auto items = value.value("items").toArray();
			if (!items.isEmpty()) {
				const auto first = items.first().toObject();
				_organizationId = first.value("organizationId").toString();
				const auto member = first.value("currentMember").toObject();
				_userId = member.value("userId").toString();
				LOG(("MtsLink Session: orgId='%1' userId='%2'")
					.arg(_organizationId, _userId));
			}

			if (_users && !_userId.isEmpty()) {
				_users->loadMember(_userId, _organizationId);
			}

			subscribeToEvents();
		});
}

void Session::subscribeToEvents() {
	_initPendingCalls = 5;
	auto onDone = [this](const QJsonObject &) {
		if (--_initPendingCalls <= 0) {
			LOG(("MtsLink Session: fully initialized"));
			Q_EMIT initialized();
			loadChatLists();
		}
	};

	const auto orgParam = QJsonObject{
		{"organizationId", _organizationId}};

	_rpc.call("Organization.Subscribe", orgParam, onDone);
	_rpc.call("Counters.GetCounters", QJsonObject{}, onDone);
	_rpc.call("Featurer.GetFeatures", QJsonObject{}, onDone);
	_rpc.call("Chat.Subscribe", orgParam, onDone);
	_rpc.call("Notification.Subscribe", QJsonObject{}, onDone);

	_rpc.call(
		"Chat.GetMyFrequentlyUsedEmojis",
		orgParam,
		[](const QJsonObject &result) {
			const auto value = result.value("value").toObject();
			const auto items = value.value("items").toArray();
			QHash<QString, QString> mapping;
			for (const auto &item : items) {
				const auto obj = item.toObject();
				const auto emoji = obj.value("emoji").toString();
				const auto emojiId = obj.value("emojiId").toString();
				if (!emoji.isEmpty() && !emojiId.isEmpty()) {
					mapping[emoji] = emojiId;
				}
			}
			if (!mapping.isEmpty()) {
				MtsLink::setEmojiMapping(mapping);
			}
		});
}

void Session::loadChatLists() {
	int pending = 4;
	auto onListDone = [this, pending]() mutable {
		if (--pending <= 0) {
			Q_EMIT chatListReady();
		}
	};

	QObject::connect(
		_channels.get(),
		&Api::Channels::channelsLoaded,
		this,
		[onListDone](const QList<Api::ChannelData> &) mutable {
			onListDone();
		});
	QObject::connect(
		_channels.get(),
		&Api::Channels::dialogsLoaded,
		this,
		[onListDone](const QList<Api::ChannelData> &) mutable {
			onListDone();
		});
	QObject::connect(
		_threads.get(),
		&Api::Threads::threadsLoaded,
		this,
		[onListDone](const QList<Api::ThreadData> &) mutable {
			onListDone();
		});

	QObject::connect(
		_threads.get(),
		&Api::Threads::unreadCounterLoaded,
		this,
		[onListDone](int) mutable {
			onListDone();
		});

	_channels->loadMyChannels();
	_channels->loadMyDialogsAndGroupChats();
	_threads->loadMyThreads();
	_threads->getUnreadCounter(_organizationId);
}

void Session::scheduleReconnect() {
	if (_token.isEmpty()) {
		return;
	}
	_reconnectDelay = _reconnectDelay
		? qMin(_reconnectDelay * 2, kReconnectMaxMs)
		: kReconnectInitialMs;
	LOG(("MtsLink Session: reconnecting in %1 ms").arg(_reconnectDelay));
	_reconnectTimer.start(_reconnectDelay);
}

void Session::doReconnect() {
	if (_manualStop || _token.isEmpty()) {
		return;
	}
	LOG(("MtsLink Session: attempting reconnect"));
	_rpc.connectAndAuth(_token);
}

} // namespace MtsLink

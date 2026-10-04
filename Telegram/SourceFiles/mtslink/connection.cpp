/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#include "mtslink/connection.h"
#include "mtslink/env_config.h"

#include <QJsonDocument>
#include <QUrl>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkProxyFactory>
#include <QtNetwork/QNetworkProxyQuery>

namespace MtsLink {
namespace {

constexpr auto kPlatform = "desktop";
constexpr auto kVersion = "1.0.0";
constexpr auto kDefaultPingInterval = 10000;
constexpr auto kWsPort = 443;

[[nodiscard]] QByteArray GenerateWebSocketKey() {
	QByteArray key(16, Qt::Uninitialized);
	for (int i = 0; i < 16; ++i) {
		key[i] = char(QRandomGenerator::global()->bounded(256));
	}
	return key.toBase64();
}

[[nodiscard]] QByteArray MaskPayload(
		const QByteArray &data,
		const QByteArray &mask) {
	QByteArray result = data;
	for (int i = 0; i < result.size(); ++i) {
		result[i] = result[i] ^ mask[i % 4];
	}
	return result;
}

[[nodiscard]] QByteArray BuildWebSocketFrame(const QByteArray &payload) {
	QByteArray frame;
	// FIN + text opcode
	frame.append(char(0x81));

	// Masked, payload length
	const auto len = payload.size();
	if (len < 126) {
		frame.append(char(0x80 | len));
	} else if (len < 65536) {
		frame.append(char(0x80 | 126));
		frame.append(char((len >> 8) & 0xFF));
		frame.append(char(len & 0xFF));
	} else {
		frame.append(char(0x80 | 127));
		for (int i = 7; i >= 0; --i) {
			frame.append(char((len >> (8 * i)) & 0xFF));
		}
	}

	QByteArray mask(4, Qt::Uninitialized);
	for (int i = 0; i < 4; ++i) {
		mask[i] = char(QRandomGenerator::global()->bounded(256));
	}
	frame.append(mask);
	frame.append(MaskPayload(payload, mask));
	return frame;
}

} // namespace

Connection::Connection(QObject *parent)
: QObject(parent) {
	_sendQueueTimer.setSingleShot(true);
	QObject::connect(
		&_sendQueueTimer,
		&QTimer::timeout,
		this,
		&Connection::flushSendQueue);
	QObject::connect(
		&_socket,
		&QSslSocket::encrypted,
		this,
		&Connection::onSocketConnected);
	QObject::connect(
		&_socket,
		&QSslSocket::readyRead,
		this,
		&Connection::onSocketReadyRead);
	QObject::connect(
		&_socket,
		&QSslSocket::disconnected,
		this,
		&Connection::onSocketDisconnected);
	QObject::connect(
		&_pingTimer,
		&QTimer::timeout,
		this,
		&Connection::sendPing);
}

Connection::~Connection() {
	disconnect();
}

void Connection::connectToServer(const QString &token) {
	_token = token;
	_sendSeq = 0;
	_recvSeq = 0;
	_ackSeq = 0;
	_authenticated = false;
	_wsHandshakeDone = false;
	_readBuffer.clear();

	const auto url = QUrl(QString("%1?p=%2&v=%3")
		.arg(EnvConfig::instance().wssServerUrl())
		.arg(kPlatform)
		.arg(kVersion));
	_host = url.host();
	_path = url.path();
	if (url.hasQuery()) {
		_path += '?' + url.query();
	}

	LOG(("MtsLink WS: connecting to %1:%2%3").arg(_host).arg(kWsPort).arg(_path));

	{
		const auto appProxy = QNetworkProxy::applicationProxy();
		LOG(("MtsLink WS: app proxy type=%1 host=%2:%3")
			.arg(int(appProxy.type()))
			.arg(appProxy.hostName())
			.arg(appProxy.port()));
		const auto proxies = QNetworkProxyFactory::systemProxyForQuery(
			QNetworkProxyQuery(url));
		for (const auto &p : proxies) {
			LOG(("MtsLink WS: system proxy type=%1 host=%2:%3")
				.arg(int(p.type()))
				.arg(p.hostName())
				.arg(p.port()));
		}
	}

	if (!_errorSignalConnected) {
		_errorSignalConnected = true;
		QObject::connect(&_socket, &QSslSocket::errorOccurred,
			this, [this](QAbstractSocket::SocketError err) {
				LOG(("MtsLink WS: socket error %1: %2").arg(int(err)).arg(_socket.errorString()));
			});
	}

	_socket.connectToHostEncrypted(_host, kWsPort);
}

void Connection::disconnect() {
	_pingTimer.stop();
	_authenticated = false;
	_wsHandshakeDone = false;
	if (_socket.state() != QAbstractSocket::UnconnectedState) {
		_socket.close();
	}
}

void Connection::sendControl(const QString &name, const QJsonObject &param) {
	QJsonObject frame;
	QJsonObject control;
	control["name"] = name;
	if (!param.isEmpty()) {
		control["param"] = param;
	}
	frame["control"] = control;
	frame["seq"] = _sendSeq++;
	frame["ack"] = _recvSeq;

	sendRawText(QString::fromUtf8(
		QJsonDocument(frame).toJson(QJsonDocument::Compact)));
}

namespace {

// "Chat.ReadMessage" for commands, the kind / type for the rest.
[[nodiscard]] QStringList MessageTypes(const QJsonArray &messages) {
	auto result = QStringList();
	for (const auto &value : messages) {
		const auto message = value.toObject();
		auto type = message.value("method").toString();
		if (type.isEmpty()) {
			type = message.value("kind").toString();
		}
		if (type.isEmpty()) {
			type = message.value("type").toString();
		}
		result.push_back(type.isEmpty() ? u"?"_q : type);
	}
	return result;
}

[[nodiscard]] QString CountTypes(const QHash<QString, int> &counts) {
	auto list = std::vector<std::pair<int, QString>>();
	for (auto i = counts.begin(); i != counts.end(); ++i) {
		list.push_back({ i.value(), i.key() });
	}
	ranges::sort(list, ranges::greater());
	auto result = QStringList();
	for (const auto &[count, type] : list) {
		result.push_back(type + ':' + QString::number(count));
	}
	return result.join(u", "_q);
}

} // namespace

void Connection::countSent(const QStringList &types) {
	for (const auto &type : types) {
		++_sentStats[type];
		++_sentStatsTotal;
	}
	constexpr auto kLogEach = crl::time(10 * 60 * 1000);
	const auto now = crl::now();
	if (!_sentStatsLogged) {
		_sentStatsLogged = now;
	} else if (now - _sentStatsLogged >= kLogEach) {
		_sentStatsLogged = now;
		logSentStats();
	}
}

void Connection::logSentStats() {
	auto top = QHash<QString, int>();
	auto list = std::vector<std::pair<int, QString>>();
	for (auto i = _sentStats.begin(); i != _sentStats.end(); ++i) {
		list.push_back({ i.value(), i.key() });
	}
	ranges::sort(list, ranges::greater());
	constexpr auto kTop = 15;
	for (auto i = 0; i != int(list.size()) && i != kTop; ++i) {
		top.insert(list[i].second, list[i].first);
	}
	LOG(("MtsLink WS: sent %1 messages of %2 types since connect, top: %3"
		).arg(_sentStatsTotal
		).arg(_sentStats.size()
		).arg(CountTypes(top)));
}

void Connection::sendMessages(const QJsonArray &messages) {
	_sendQueue.push_back(messages);
	flushSendQueue();
}

void Connection::flushSendQueue() {
	// The server answers "rateLimitIsReached" and drops requests above
	// ~50 messages per second (seen on the first login: 84 sent, 34 lost).
	constexpr auto kMaxPerSecond = 40;
	constexpr auto kWindow = crl::time(1000);
	while (!_sendQueue.empty()) {
		const auto now = crl::now();
		while (!_sentTimes.empty()
			&& now - _sentTimes.front().when >= kWindow) {
			_sentTimes.pop_front();
		}
		if (int(_sentTimes.size()) >= kMaxPerSecond) {
			if (!_sendQueueTimer.isActive()) {
				const auto wait = kWindow
					- (now - _sentTimes.front().when)
					+ 1;
				_sendQueueTimer.start(int(std::max(wait, crl::time(1))));
			}
			return;
		}
		const auto messages = _sendQueue.front();
		_sendQueue.pop_front();
		const auto types = MessageTypes(messages);
		_sentTimes.push_back({ now, types });
		countSent(types);
		sendMessagesNow(messages);
	}
}

void Connection::sendMessagesNow(const QJsonArray &messages) {
	QJsonObject frame;
	frame["messages"] = messages;
	frame["seq"] = _sendSeq++;
	frame["ack"] = _recvSeq;

	sendRawText(QString::fromUtf8(
		QJsonDocument(frame).toJson(QJsonDocument::Compact)));
}

bool Connection::isConnected() const {
	return _authenticated;
}

QString Connection::sid() const {
	return _sid;
}

void Connection::onSocketConnected() {
	LOG(("MtsLink WS: TLS connected, sending handshake"));
	sendWebSocketHandshake();
}

void Connection::sendWebSocketHandshake() {
	const auto key = GenerateWebSocketKey();
	const auto request = QByteArray(
		"GET " + _path.toUtf8() + " HTTP/1.1\r\n"
		"Host: " + _host.toUtf8() + "\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: " + key + "\r\n"
		"Sec-WebSocket-Version: 13\r\n"
		"\r\n");
	_socket.write(request);
}

void Connection::onSocketReadyRead() {
	_readBuffer.append(_socket.readAll());

	if (!_wsHandshakeDone) {
		const auto end = _readBuffer.indexOf("\r\n\r\n");
		if (end < 0) {
			return;
		}
		const auto response = _readBuffer.left(end);
		_readBuffer = _readBuffer.mid(end + 4);
		if (response.contains("101") && response.toLower().contains("upgrade")) {
			LOG(("MtsLink WS: handshake OK"));
			_wsHandshakeDone = true;
		} else {
			LOG(("MtsLink WS: handshake FAILED: %1").arg(QString::fromUtf8(response)));
			Q_EMIT error("WebSocket handshake failed");
			_socket.close();
			return;
		}
	}

	processIncomingData();
}

void Connection::processIncomingData() {
	while (_readBuffer.size() >= 2) {
		const auto byte0 = quint8(_readBuffer[0]);
		const auto byte1 = quint8(_readBuffer[1]);
		const auto opcode = byte0 & 0x0F;
		const auto masked = (byte1 & 0x80) != 0;
		quint64 payloadLen = byte1 & 0x7F;
		int headerSize = 2;

		if (payloadLen == 126) {
			if (_readBuffer.size() < 4) return;
			payloadLen = (quint8(_readBuffer[2]) << 8)
				| quint8(_readBuffer[3]);
			headerSize = 4;
		} else if (payloadLen == 127) {
			if (_readBuffer.size() < 10) return;
			payloadLen = 0;
			for (int i = 0; i < 8; ++i) {
				payloadLen = (payloadLen << 8)
					| quint8(_readBuffer[2 + i]);
			}
			headerSize = 10;
		}

		if (masked) {
			headerSize += 4;
		}

		const auto totalSize = qint64(headerSize) + qint64(payloadLen);
		if (_readBuffer.size() < totalSize) {
			return;
		}

		auto payload = _readBuffer.mid(headerSize, int(payloadLen));
		if (masked) {
			const auto maskOffset = headerSize - 4;
			const auto mask = _readBuffer.mid(maskOffset, 4);
			payload = MaskPayload(payload, mask);
		}
		_readBuffer = _readBuffer.mid(int(totalSize));

		if (opcode == 0x1) {
			handleFrame(QString::fromUtf8(payload));
		} else if (opcode == 0x8) {
			_socket.close();
			return;
		} else if (opcode == 0x9) {
			// Pong response
			QByteArray pong;
			pong.append(char(0x8A));
			pong.append(char(0x80 | payload.size()));
			QByteArray mask(4, Qt::Uninitialized);
			for (int i = 0; i < 4; ++i) {
				mask[i] = char(QRandomGenerator::global()->bounded(256));
			}
			pong.append(mask);
			pong.append(MaskPayload(payload, mask));
			_socket.write(pong);
		}
	}
}

void Connection::sendRawText(const QString &text) {
	if (!_wsHandshakeDone) {
		return;
	}
	_socket.write(BuildWebSocketFrame(text.toUtf8()));
}

void Connection::handleFrame(const QString &text) {
	const auto doc = QJsonDocument::fromJson(text.toUtf8());
	if (doc.isNull()) {
		return;
	}
	const auto root = doc.object();
	const auto seq = root.value("seq").toInt(-1);

	if (seq >= 0) {
		_recvSeq = seq;
	}

	if (root.contains("control")) {
		handleControl(root.value("control").toObject(), seq);
	}
	if (root.contains("messages")) {
		handleMessages(root.value("messages").toArray(), seq);
	}
}

void Connection::handleControl(const QJsonObject &control, int seq) {
	const auto name = control.value("name").toString();
	const auto param = control.value("param").toObject();

	if (name != u"pong"_q) { // Every few seconds.
		LOG(("MtsLink WS: control '%1'").arg(name));
	}
	if (name == "open") {
		_sid = param.value("sid").toString();
		const auto pingInterval = param.value("pingInterval").toInt(
			kDefaultPingInterval);
		_pingTimer.start(pingInterval);
		LOG(("MtsLink WS: got sid=%1, sending token").arg(_sid));
		sendControl("token", QJsonObject{{"token", _token}});
	} else if (name == "setTokenResp") {
		const auto status = param.value("status").toString();
		if (status == "ok") {
			LOG(("MtsLink WS: setTokenResp status=ok"));
			_authenticated = true;
			Q_EMIT connected(_sid);
		} else {
			const auto detail = QString::fromUtf8(
				QJsonDocument(param).toJson(QJsonDocument::Compact));
			LOG(("MtsLink WS: setTokenResp FAILED: %1").arg(detail));
			Q_EMIT error("Authentication failed");
		}
	} else if (name == "pong") {
		// keepalive acknowledged
	} else if (name == "rateLimitIsReached") {
		auto sent = QHash<QString, int>();
		const auto now = crl::now();
		for (const auto &entry : _sentTimes) {
			if (now - entry.when < 1000) {
				for (const auto &type : entry.types) {
					++sent[type];
				}
			}
		}
		auto queued = QHash<QString, int>();
		for (const auto &messages : _sendQueue) {
			for (const auto &type : MessageTypes(messages)) {
				++queued[type];
			}
		}
		LOG(("MtsLink WS: rate limit reached, sent in the last second: %1 "
			"[%2], queued: %3 [%4]")
			.arg(_sentTimes.size())
			.arg(CountTypes(sent))
			.arg(_sendQueue.size())
			.arg(CountTypes(queued)));
		logSentStats();
	}

	Q_EMIT controlReceived(name, param);
}

void Connection::handleMessages(const QJsonArray &messages, int seq) {
	for (const auto &val : messages) {
		Q_EMIT messageReceived(val.toObject());
	}
}

void Connection::sendPing() {
	sendControl("ping");
}

void Connection::onSocketDisconnected() {
	_pingTimer.stop();
	_sendQueueTimer.stop();
	_sendQueue.clear();
	_sentTimes.clear();
	if (_sentStatsTotal) {
		logSentStats();
	}
	_sentStats.clear();
	_sentStatsTotal = 0;
	_sentStatsLogged = 0;
	_authenticated = false;
	_wsHandshakeDone = false;
	_readBuffer.clear();
	LOG(("MtsLink WS: disconnected"));
	Q_EMIT disconnected();
}

} // namespace MtsLink

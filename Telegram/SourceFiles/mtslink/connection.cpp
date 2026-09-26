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

void Connection::sendMessages(const QJsonArray &messages) {
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

	LOG(("MtsLink WS: control '%1'").arg(name));
	if (name == "open") {
		_sid = param.value("sid").toString();
		const auto pingInterval = param.value("pingInterval").toInt(
			kDefaultPingInterval);
		_pingTimer.start(pingInterval);
		LOG(("MtsLink WS: got sid=%1, sending token").arg(_sid));
		sendControl("token", QJsonObject{{"token", _token}});
	} else if (name == "setTokenResp") {
		const auto status = param.value("status").toString();
		LOG(("MtsLink WS: setTokenResp status=%1").arg(status));
		if (status == "ok") {
			_authenticated = true;
			Q_EMIT connected(_sid);
		} else {
			Q_EMIT error("Authentication failed");
		}
	} else if (name == "pong") {
		// keepalive acknowledged
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
	_authenticated = false;
	_wsHandshakeDone = false;
	_readBuffer.clear();
	LOG(("MtsLink WS: disconnected"));
	Q_EMIT disconnected();
}

} // namespace MtsLink

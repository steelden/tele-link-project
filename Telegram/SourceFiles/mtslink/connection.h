/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#pragma once

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QtNetwork/QSslSocket>

namespace MtsLink {

class Connection final : public QObject {
	Q_OBJECT

public:
	explicit Connection(QObject *parent = nullptr);
	~Connection();

	void connectToServer(const QString &token);
	void disconnect();
	void sendControl(const QString &name, const QJsonObject &param = {});
	void sendMessages(const QJsonArray &messages);

	[[nodiscard]] bool isConnected() const;
	[[nodiscard]] QString sid() const;

Q_SIGNALS:
	void connected(const QString &sid);
	void disconnected();
	void controlReceived(const QString &name, const QJsonObject &param);
	void messageReceived(const QJsonObject &message);
	void error(const QString &errorText);

private:
	void handleFrame(const QString &text);
	void handleControl(const QJsonObject &control, int seq);
	void handleMessages(const QJsonArray &messages, int seq);
	void sendPing();
	void sendRawText(const QString &text);

	// Raw WebSocket over QSslSocket (Qt WebSockets module not available).
	void onSocketConnected();
	void onSocketReadyRead();
	void sendWebSocketHandshake();
	void processIncomingData();

	void onSocketDisconnected();

	QSslSocket _socket;
	QTimer _pingTimer;
	QString _token;
	QString _sid;
	QString _host;
	QString _path;

	QByteArray _readBuffer;
	bool _wsHandshakeDone = false;
	bool _errorSignalConnected = false;

	int _sendSeq = 0;
	int _recvSeq = 0;
	int _ackSeq = 0;

	bool _authenticated = false;
};

} // namespace MtsLink

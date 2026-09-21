/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#pragma once

#include "mtslink/types.h"

#include <QObject>
#include <QByteArray>
#include <functional>

QT_BEGIN_NAMESPACE
class QNetworkAccessManager;
class QNetworkReply;
QT_END_NAMESPACE

namespace MtsLink {
class Rpc;
} // namespace MtsLink

namespace MtsLink::Api {

struct UploadResult {
	FileId id;
	QString url;
	QString name;
	qint64 size = 0;
	QString mime;
};

class Files final : public QObject {
	Q_OBJECT

public:
	using DoneHandler = std::function<void(const UploadResult &result)>;
	using FailHandler = std::function<void(const QString &error)>;
	using ProgressHandler = std::function<void(qint64 sent, qint64 total)>;

	explicit Files(Rpc *rpc, QObject *parent = nullptr);
	~Files();

	void uploadFile(
		const QString &filename,
		const QByteArray &content,
		const QString &mime,
		DoneHandler done,
		FailHandler fail = nullptr,
		ProgressHandler progress = nullptr);

private:
	void doHttpUpload(
		const QString &url,
		const QString &method,
		const QStringList &headers,
		const QByteArray &content,
		const UploadResult &result,
		DoneHandler done,
		FailHandler fail,
		ProgressHandler progress);

	Rpc *_rpc = nullptr;
	QNetworkAccessManager *_network = nullptr;
};

} // namespace MtsLink::Api

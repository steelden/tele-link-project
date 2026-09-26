/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.
*/
#include "mtslink/api/api_files.h"
#include "mtslink/rpc.h"
#include "mtslink/data_adapters.h"
#include "mtslink/env_config.h"

#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>
#include <QtCore/QJsonArray>
#include <QtCore/QFile>

namespace MtsLink::Api {

Files::Files(Rpc *rpc, QObject *parent)
: QObject(parent)
, _rpc(rpc)
, _network(new QNetworkAccessManager(this)) {
}

Files::~Files() = default;

void Files::uploadFile(
		const QString &filename,
		const QByteArray &content,
		const QString &mime,
		DoneHandler done,
		FailHandler fail,
		ProgressHandler progress) {
	QJsonObject param;
	param["filename"] = filename;

	_rpc->call(
		"Mediacontent.CreatePrivateFile",
		param,
		[=](const QJsonObject &result) {
			const auto value = result.value("value").isObject()
				? result.value("value").toObject()
				: result;
			const auto id = value.value("id").toString();
			const auto url = value.value("url").toString();
			const auto method = value.value("method").toString();
			const auto headersArr = value.value("headers").toArray();

			QStringList headers;
			for (const auto &h : headersArr) {
				headers.push_back(h.toString());
			}

			if (id.isEmpty() || url.isEmpty()) {
				LOG(("MtsLink Files: CreatePrivateFile returned empty id or url"));
				if (fail) {
					fail("CreatePrivateFile returned empty response");
				}
				return;
			}

			LOG(("MtsLink Files: got upload params id='%1' method='%2' url='%3'")
				.arg(id, method, url));

			UploadResult uploadResult{
				.id = id,
				.url = EnvConfig::instance().baseMediaUrl() + u"/file/"_q + id + u"/download"_q,
				.name = filename,
				.size = content.size(),
				.mime = mime,
			};

			doHttpUpload(url, method, headers, content, uploadResult, done, fail, progress);
		},
		[fail](const QString &error) {
			LOG(("MtsLink Files: CreatePrivateFile error: %1").arg(error));
			if (fail) {
				fail(error);
			}
		});
}

void Files::doHttpUpload(
		const QString &url,
		const QString &method,
		const QStringList &headers,
		const QByteArray &content,
		const UploadResult &result,
		DoneHandler done,
		FailHandler fail,
		ProgressHandler progress) {
	const auto qurl = QUrl(url);
	QNetworkRequest request(qurl);

	for (const auto &header : headers) {
		const auto colonIdx = header.indexOf(':');
		if (colonIdx > 0) {
			const auto name = header.left(colonIdx).trimmed().toUtf8();
			const auto value = header.mid(colonIdx + 1).trimmed().toUtf8();
			request.setRawHeader(name, value);
		}
	}

	const auto token = MtsLink::fileAuthToken();
	if (!token.isEmpty()) {
		request.setRawHeader("Authorization", ("Bearer " + token).toUtf8());
	}

	QNetworkReply *reply = nullptr;
	if (method.compare("PUT", Qt::CaseInsensitive) == 0) {
		reply = _network->put(request, content);
	} else {
		reply = _network->post(request, content);
	}

	if (progress) {
		QObject::connect(reply, &QNetworkReply::uploadProgress, this,
			[=](qint64 sent, qint64 total) {
				progress(sent, total);
			});
	}

	QObject::connect(reply, &QNetworkReply::finished, this,
		[=]() {
			reply->deleteLater();
			if (reply->error() != QNetworkReply::NoError) {
				LOG(("MtsLink Files: HTTP upload failed: %1")
					.arg(reply->errorString()));
				if (fail) {
					fail(reply->errorString());
				}
				return;
			}
			LOG(("MtsLink Files: upload complete, fileId='%1'")
				.arg(result.id));
			if (done) {
				done(result);
			}
		});
}

} // namespace MtsLink::Api

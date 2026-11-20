/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.
*/
#include "mtslink/api/api_tasks.h"

#include "mtslink/data_adapters.h"
#include "mtslink/env_config.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTimeZone>
#include <QtCore/QRegularExpression>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace MtsLink::Api {
namespace {

[[nodiscard]] QString BaseUrl() {
	return EnvConfig::instance().webinarHost() + u"/tasks/api"_q;
}

[[nodiscard]] QDateTime ParseTime(const QJsonValue &value) {
	const auto text = value.toString();
	if (text.isEmpty()) {
		return QDateTime();
	}
	auto result = QDateTime::fromString(text, Qt::ISODateWithMs);
	if (!result.isValid()) {
		result = QDateTime::fromString(text, Qt::ISODate);
	}
	if (!result.isValid()) {
		// "2026-11-20T22:30:00.000000Z": the microseconds.
		auto trimmed = text;
		const auto dot = trimmed.indexOf('.');
		if (dot > 0) {
			const auto zone = trimmed.indexOf(QRegularExpression(u"[Z+-]"_q), dot);
			trimmed = trimmed.left(dot + 4)
				+ ((zone > 0) ? trimmed.mid(zone) : QString());
		}
		result = QDateTime::fromString(trimmed, Qt::ISODateWithMs);
	}
	return result;
}

[[nodiscard]] QString FormatTime(const QDateTime &time) {
	return time.toUTC().toString(u"yyyy-MM-ddTHH:mm:ss.zzzZ"_q);
}

[[nodiscard]] Task ParseTask(const QJsonObject &object) {
	return {
		.id = object.value(u"id"_q).toString(),
		.title = object.value(u"title"_q).toString(),
		.description = object.value(u"description"_q).toString(),
		.assigneeId = object.value(u"assignee_id"_q).toString(),
		.authorId = object.value(u"user_id"_q).toString(),
		.updatedBy = object.value(u"updated_by"_q).toString(),
		.messageUrl = object.value(u"message_url"_q).toString(),
		.sourceType = object.value(u"source_type"_q).toString(),
		.deadline = ParseTime(object.value(u"deadline"_q)),
		.createdAt = ParseTime(object.value(u"created_at"_q)),
		.updatedAt = ParseTime(object.value(u"updated_at"_q)),
		.resolvedAt = ParseTime(object.value(u"resolved_at"_q)),
		.important = (object.value(u"priority"_q).toString()
			== u"important"_q),
	};
}

[[nodiscard]] QByteArray SerializeFields(
		const TaskFields &fields,
		bool withMessage) {
	auto object = QJsonObject{
		{ u"title"_q, fields.title },
		{ u"description"_q, fields.description },
		{ u"deadline"_q, fields.deadline.isValid()
			? QJsonValue(FormatTime(fields.deadline))
			: QJsonValue(QJsonValue::Null) },
		{ u"priority"_q, fields.important
			? QJsonValue(u"important"_q)
			: QJsonValue(QJsonValue::Null) },
		{ u"assignee_id"_q, fields.assigneeId.isEmpty()
			? QJsonValue(QJsonValue::Null)
			: QJsonValue(fields.assigneeId) },
	};
	if (withMessage && !fields.messageUrl.isEmpty()) {
		object.insert(u"message_url"_q, fields.messageUrl);
	}
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

} // namespace

TaskFilter FilterForSection(TaskSection section) {
	auto result = TaskFilter();
	switch (section) {
	case TaskSection::My:
		result.resolved = false;
		result.assignee = u"me"_q;
		result.author = u"all"_q;
		break;
	case TaskSection::Important:
		result.resolved = false;
		result.important = true;
		result.assignee = u"all"_q;
		result.author = u"all"_q;
		break;
	case TaskSection::AssignedByMe:
		result.resolved = false;
		result.assignee = u"not_me"_q;
		result.author = u"me"_q;
		break;
	case TaskSection::Someday:
		result.resolved = false;
		result.noDeadline = true;
		result.assignee = u"me"_q;
		result.author = u"all"_q;
		break;
	case TaskSection::Completed:
		result.resolved = true;
		result.assignee = u"all"_q;
		result.author = u"all"_q;
		break;
	case TaskSection::All:
		result.assignee = u"all"_q;
		result.author = u"all"_q;
		break;
	}
	return result;
}

TaskFields FieldsOf(const Task &task) {
	return {
		.title = task.title,
		.description = task.description,
		.deadline = task.deadline,
		.assigneeId = task.assigneeId,
		.important = task.important,
		.messageUrl = task.messageUrl,
	};
}

struct Tasks::Private {
	QNetworkAccessManager network;
	rpl::variable<int> attention = 0;
	rpl::event_stream<> changes;
	bool attentionLoading = false;
};

Tasks &Tasks::Instance() {
	static Tasks result;
	return result;
}

Tasks::Tasks() : _private(std::make_unique<Private>()) {
}

Tasks::~Tasks() = default;

void Tasks::request(
		const QByteArray &method,
		const QString &path,
		const QList<QPair<QString, QString>> &query,
		const QByteArray &body,
		Fn<void(QByteArray)> done,
		TasksFail fail) {
	auto url = QUrl(BaseUrl() + path);
	if (!query.isEmpty()) {
		// As the web client: "filter[resolved]=false&...".
		auto parts = QStringList();
		for (const auto &[key, value] : query) {
			parts.push_back(key + '=' + QString::fromLatin1(
				QUrl::toPercentEncoding(value)));
		}
		url.setQuery(parts.join('&'), QUrl::TolerantMode);
	}
	auto request = QNetworkRequest(url);
	request.setHeader(
		QNetworkRequest::ContentTypeHeader,
		u"application/json"_q);
	const auto token = MtsLink::fileAuthToken();
	if (!token.isEmpty()) {
		request.setRawHeader("Authorization", ("Bearer " + token).toUtf8());
	}
	const auto reply = body.isEmpty()
		? _private->network.sendCustomRequest(request, method)
		: _private->network.sendCustomRequest(request, method, body);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto data = reply->readAll();
		if (status < 200 || status >= 300) {
			const auto error = u"%1 %2"_q
				.arg(status)
				.arg(QString::fromUtf8(data.left(300)));
			LOG(("MtsLink Tasks: %1 %2 failed: %3 (%4)"
				).arg(QString::fromLatin1(method)
				).arg(path
				).arg(error
				).arg(reply->errorString()));
			if (fail) {
				fail(error);
			}
			return;
		}
		if (done) {
			done(data);
		}
	});
}

void Tasks::list(
		const TaskFilter &filter,
		int offset,
		int limit,
		TasksDone done,
		TasksFail fail) {
	auto query = QList<QPair<QString, QString>>{
		{ u"query"_q, filter.query },
		{ u"sort_by"_q, filter.sortBy },
		{ u"filter[author]"_q, filter.author },
		{ u"filter[assignee]"_q, filter.assignee },
	};
	if (filter.resolved) {
		query.push_back({
			u"filter[resolved]"_q,
			*filter.resolved ? u"true"_q : u"false"_q,
		});
	}
	if (filter.important) {
		query.push_back({ u"filter[priority]"_q, u"important"_q });
	}
	if (filter.noDeadline) {
		query.push_back({ u"filter[no_deadline]"_q, u"true"_q });
	}
	query.push_back({ u"offset"_q, QString::number(offset) });
	query.push_back({ u"limit"_q, QString::number(limit) });
	query.push_back({
		u"timezone"_q,
		QString::fromUtf8(QTimeZone::systemTimeZoneId()),
	});
	query.push_back({ u"sort_order"_q, filter.sortOrder });
	request("GET", u"/tasks"_q, query, {}, [=](QByteArray data) {
		const auto object = QJsonDocument::fromJson(data).object();
		auto tasks = std::vector<Task>();
		for (const auto &value : object.value(u"tasks"_q).toArray()) {
			tasks.push_back(ParseTask(value.toObject()));
		}
		const auto counters = object.value(u"counters"_q).toObject();
		const auto result = TaskCounters{
			.total = counters.value(u"total"_q).toInt(),
			.resolved = counters.value(u"resolved"_q).toInt(),
			.notResolved = counters.value(u"not_resolved"_q).toInt(),
			.attention = counters.value(u"attention"_q).toInt(),
			.filteredTotal = counters.value(u"filtered_total"_q).toInt(),
		};
		_private->attention = result.attention;
		done(std::move(tasks), result);
	}, fail);
}

void Tasks::get(const QString &id, TaskDone done, TasksFail fail) {
	request("GET", u"/tasks/"_q + id, {}, {}, [=](QByteArray data) {
		done(ParseTask(QJsonDocument::fromJson(data).object()));
	}, fail);
}

void Tasks::create(
		const TaskFields &fields,
		TaskDone done,
		TasksFail fail) {
	request(
		"POST",
		u"/v2/tasks"_q,
		{},
		SerializeFields(fields, true),
		[=](QByteArray data) {
			const auto task = ParseTask(QJsonDocument::fromJson(data).object());
			LOG(("MtsLink Tasks: created %1").arg(task.id));
			changed();
			if (done) {
				done(task);
			}
		},
		fail);
}

void Tasks::update(
		const QString &id,
		const TaskFields &fields,
		TaskDone done,
		TasksFail fail) {
	request(
		"PUT",
		u"/tasks/"_q + id,
		{},
		SerializeFields(fields, false),
		[=](QByteArray data) {
			changed();
			if (done) {
				done(ParseTask(QJsonDocument::fromJson(data).object()));
			}
		},
		fail);
}

void Tasks::remove(const QString &id, Fn<void()> done, TasksFail fail) {
	request("DELETE", u"/tasks/"_q + id, {}, {}, [=](QByteArray) {
		LOG(("MtsLink Tasks: removed %1").arg(id));
		changed();
		if (done) {
			done();
		}
	}, fail);
}

void Tasks::setResolved(
		const QString &id,
		bool resolved,
		TaskDone done,
		TasksFail fail) {
	request(
		"POST",
		u"/tasks/"_q + id + (resolved ? u"/resolve"_q : u"/unresolve"_q),
		{},
		{},
		[=](QByteArray data) {
			changed();
			if (done) {
				done(ParseTask(QJsonDocument::fromJson(data).object()));
			}
		},
		fail);
}

void Tasks::refreshAttention() {
	if (_private->attentionLoading) {
		return;
	}
	_private->attentionLoading = true;
	request(
		"GET",
		u"/tasks/counters"_q,
		{ { u"timezone"_q, QString::fromUtf8(QTimeZone::systemTimeZoneId()) } },
		{},
		[=](QByteArray data) {
			_private->attentionLoading = false;
			_private->attention = QJsonDocument::fromJson(data).object().value(
				u"attention"_q).toInt();
		},
		[=](QString) {
			_private->attentionLoading = false;
		});
}

rpl::producer<int> Tasks::attentionValue() const {
	return _private->attention.value();
}

rpl::producer<> Tasks::changes() const {
	return _private->changes.events();
}

void Tasks::changed() {
	_private->changes.fire({});
	refreshAttention();
}

} // namespace MtsLink::Api

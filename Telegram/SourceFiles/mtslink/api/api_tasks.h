/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.
*/
#pragma once

#include <QtCore/QDateTime>

namespace MtsLink::Api {

// The task tracker of MTS Link: a REST API apart from the chat socket,
// https://my.mts-link.ru/tasks/api with the token of the chat socket.
// See arch/24-tasks-api.md.

struct Task {
	QString id;
	QString title;
	QString description;
	QString assigneeId;
	QString authorId;
	QString updatedBy;
	QString messageUrl;
	QString sourceType;
	QDateTime deadline;
	QDateTime createdAt;
	QDateTime updatedAt;
	QDateTime resolvedAt;
	bool important = false;

	[[nodiscard]] bool resolved() const {
		return resolvedAt.isValid();
	}
};

struct TaskCounters {
	int total = 0;
	int resolved = 0;
	int notResolved = 0;
	int attention = 0;
	int filteredTotal = 0;
};

// The sections of the web client: the ready filters.
enum class TaskSection {
	My,
	Important,
	AssignedByMe,
	Someday,
	Completed,
	All,
};

struct TaskFilter {
	std::optional<bool> resolved;
	QString assignee = u"me"_q; // me / not_me / all
	QString author = u"all"_q; // me / all
	bool important = false;
	bool noDeadline = false;
	QString query;
	QString sortBy = u"deadline"_q;
	QString sortOrder = u"asc"_q;
};
[[nodiscard]] TaskFilter FilterForSection(TaskSection section);

// The fields of a new / an edited task (PUT sends all of them).
struct TaskFields {
	QString title;
	QString description;
	QDateTime deadline; // Invalid - without a deadline.
	QString assigneeId;
	bool important = false;
	QString messageUrl; // Only for a new one, from a message.
};
[[nodiscard]] TaskFields FieldsOf(const Task &task);

using TaskDone = Fn<void(Task)>;
using TasksDone = Fn<void(std::vector<Task>, TaskCounters)>;
using TasksFail = Fn<void(QString)>;

class Tasks final {
public:
	static Tasks &Instance();

	void list(
		const TaskFilter &filter,
		int offset,
		int limit,
		TasksDone done,
		TasksFail fail = nullptr);
	void get(const QString &id, TaskDone done, TasksFail fail = nullptr);
	void create(
		const TaskFields &fields,
		TaskDone done,
		TasksFail fail = nullptr);
	void update(
		const QString &id,
		const TaskFields &fields,
		TaskDone done,
		TasksFail fail = nullptr);
	void remove(const QString &id, Fn<void()> done, TasksFail fail = nullptr);
	void setResolved(
		const QString &id,
		bool resolved,
		TaskDone done,
		TasksFail fail = nullptr);

	// The counter of the tasks needing attention (the badge).
	void refreshAttention();
	[[nodiscard]] rpl::producer<int> attentionValue() const;

	// Fired after each change made here (the lists are reloaded).
	[[nodiscard]] rpl::producer<> changes() const;

	~Tasks();

private:
	Tasks();

	void request(
		const QByteArray &method,
		const QString &path,
		const QList<QPair<QString, QString>> &query,
		const QByteArray &body,
		Fn<void(QByteArray)> done,
		TasksFail fail);
	void changed();

	struct Private;
	const std::unique_ptr<Private> _private;

};

} // namespace MtsLink::Api

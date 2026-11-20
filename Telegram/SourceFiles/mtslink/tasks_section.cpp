/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.
*/
#include "mtslink/tasks_section.h"

#include "mtslink/data_adapters.h"
#include "mtslink/tasks_peers.h"
#include "mtslink/task_edit_box.h"
#include "mtslink/session.h"
#include "mtslink/call_window.h"
#include "data/data_channel.h"
#include "data/data_chat_filters.h"
#include "dialogs/dialogs_key.h"
#include "history/history.h"
#include "history/history_item.h"
#include "base/timer.h"
#include "ui/empty_userpic.h"
#include "ui/image/image_location.h"
#include <QtSvg/QSvgRenderer>
#include <QtCore/QBuffer>
#include "base/event_filter.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "history/view/history_view_service_message.h"
#include "ui/chat/chat_style.h"
#include "storage/cache/storage_cache_database.h"
#include "ui/chat/chat_theme.h"
#include "ui/text/text_utilities.h"
#include "styles/style_chat.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/data_changes.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/effects/ripple_animation.h"
#include "window/window_session_controller.h"
#include "styles/style_dialogs.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

namespace MtsLink {
namespace {

using Api::Task;
using Api::TaskSection;

constexpr auto kPageSize = 50;
constexpr auto kReloadOnActivation = crl::time(30000);
// No events of the task tracker: the open list is polled (the changes made
// in the other clients), the previews are refreshed when it changed.
constexpr auto kPollOpenList = crl::time(30000);

// The lists of the sections in the cache of the account, as the pages of
// the messages: shown at once, refreshed in the background.
constexpr auto kTasksCacheTag = uint64(0xBC10'0000'0000'0000ULL);
constexpr auto kTasksCacheVersion = qint32(1);

[[nodiscard]] Storage::Cache::Key TasksCacheKey(TaskSection section) {
	return { kTasksCacheTag, uint64(section) + 1 };
}

[[nodiscard]] QByteArray SerializeTasks(
		const std::vector<Task> &tasks,
		const Api::TaskCounters &counters) {
	auto result = QByteArray();
	auto stream = QDataStream(&result, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream
		<< kTasksCacheVersion
		<< qint32(counters.total)
		<< qint32(counters.resolved)
		<< qint32(counters.notResolved)
		<< qint32(counters.attention)
		<< qint32(counters.filteredTotal)
		<< qint32(tasks.size());
	for (const auto &task : tasks) {
		stream
			<< task.id
			<< task.title
			<< task.description
			<< task.assigneeId
			<< task.authorId
			<< task.updatedBy
			<< task.messageUrl
			<< task.sourceType
			<< task.deadline
			<< task.createdAt
			<< task.updatedAt
			<< task.resolvedAt
			<< qint32(task.important ? 1 : 0);
	}
	return result;
}

struct CachedTasks {
	std::vector<Task> tasks;
	Api::TaskCounters counters;
};

[[nodiscard]] std::optional<CachedTasks> DeserializeTasks(
		const QByteArray &data) {
	if (data.isEmpty()) {
		return std::nullopt;
	}
	auto stream = QDataStream(data);
	stream.setVersion(QDataStream::Qt_5_1);
	auto version = qint32();
	stream >> version;
	if (version != kTasksCacheVersion) {
		return std::nullopt;
	}
	auto result = CachedTasks();
	auto total = qint32(), resolved = qint32(), notResolved = qint32();
	auto attention = qint32(), filtered = qint32(), count = qint32();
	stream >> total >> resolved >> notResolved >> attention >> filtered
		>> count;
	result.counters = Api::TaskCounters{
		.total = total,
		.resolved = resolved,
		.notResolved = notResolved,
		.attention = attention,
		.filteredTotal = filtered,
	};
	if (count < 0 || count > 10000) {
		return std::nullopt;
	}
	result.tasks.reserve(count);
	for (auto i = 0; i != count; ++i) {
		auto task = Task();
		auto important = qint32();
		stream
			>> task.id
			>> task.title
			>> task.description
			>> task.assigneeId
			>> task.authorId
			>> task.updatedBy
			>> task.messageUrl
			>> task.sourceType
			>> task.deadline
			>> task.createdAt
			>> task.updatedAt
			>> task.resolvedAt
			>> important;
		task.important = (important != 0);
		result.tasks.push_back(std::move(task));
	}
	if (stream.status() != QDataStream::Ok) {
		return std::nullopt;
	}
	return result;
}

void SaveTasksCache(
		not_null<Main::Session*> session,
		TaskSection section,
		const std::vector<Task> &tasks,
		const Api::TaskCounters &counters) {
	session->data().cache().put(
		TasksCacheKey(section),
		SerializeTasks(tasks, counters));
}

void LoadTasksCache(
		not_null<Main::Session*> session,
		TaskSection section,
		Fn<void(CachedTasks)> done) {
	const auto weak = base::make_weak(session);
	session->data().cache().get(TasksCacheKey(section), [=](
			QByteArray &&data) {
		crl::on_main(weak, [=, data = std::move(data)] {
			if (auto cached = DeserializeTasks(data)) {
				done(std::move(*cached));
			}
		});
	});
}

[[nodiscard]] QString TasksSignature(
		const std::vector<Task> &tasks,
		const Api::TaskCounters &counters) {
	auto result = QString::number(counters.filteredTotal)
		+ QChar(':') + QString::number(counters.attention);
	const auto time = [](const QDateTime &value) {
		return value.isValid()
			? QString::number(value.toMSecsSinceEpoch())
			: QString();
	};
	for (const auto &task : tasks) {
		result += QChar('|') + task.id
			+ QChar(',') + time(task.updatedAt)
			+ QChar(',') + time(task.resolvedAt)
			+ QChar(',') + time(task.deadline)
			+ QChar(',') + task.assigneeId
			+ QChar(',') + QChar(task.important ? '1' : '0')
			+ QChar(',') + task.title;
	}
	return result;
}

constexpr auto kRowHeight = 64;
// The bubble of the row in it (the margins as of the messages).
constexpr auto kBubbleSide = 12;
constexpr auto kBubbleVertical = 3;
constexpr auto kBubbleRadius = 12;
constexpr auto kRowPadding = 16;
constexpr auto kCheckSize = 20;
constexpr auto kUserpicSize = 32;

// The order of the sidebar of the web client.
constexpr auto kSections = std::array{
	TaskSection::My,
	TaskSection::All,
	TaskSection::Important,
	TaskSection::AssignedByMe,
	TaskSection::Someday,
	TaskSection::Completed,
};

[[nodiscard]] QString SectionName(TaskSection section) {
	switch (section) {
	case TaskSection::My: return tr::lng_mtslink_tasks_my(tr::now);
	case TaskSection::Important:
		return tr::lng_mtslink_tasks_important(tr::now);
	case TaskSection::AssignedByMe:
		return tr::lng_mtslink_tasks_assigned_by_me(tr::now);
	case TaskSection::Someday:
		return tr::lng_mtslink_tasks_someday(tr::now);
	case TaskSection::Completed:
		return tr::lng_mtslink_tasks_completed(tr::now);
	case TaskSection::All: return tr::lng_mtslink_tasks_all(tr::now);
	}
	return QString();
}

// The groups by the deadline, in this order.
enum class Group {
	Overdue,
	Today,
	Tomorrow,
	Later,
	NoDeadline,
	Completed,
};

[[nodiscard]] Group GroupOf(const Task &task, const QDateTime &now) {
	if (task.resolved()) {
		return Group::Completed;
	} else if (!task.deadline.isValid()) {
		return Group::NoDeadline;
	} else if (task.deadline < now) {
		return Group::Overdue;
	}
	const auto date = task.deadline.toLocalTime().date();
	const auto today = now.toLocalTime().date();
	return (date == today)
		? Group::Today
		: (date == today.addDays(1))
		? Group::Tomorrow
		: Group::Later;
}

[[nodiscard]] QString GroupName(Group group) {
	switch (group) {
	case Group::Overdue: return tr::lng_mtslink_tasks_overdue(tr::now);
	case Group::Today: return tr::lng_mtslink_tasks_today(tr::now);
	case Group::Tomorrow: return tr::lng_mtslink_tasks_tomorrow(tr::now);
	case Group::Later: return tr::lng_mtslink_tasks_later(tr::now);
	case Group::NoDeadline:
		return tr::lng_mtslink_tasks_no_deadline(tr::now);
	case Group::Completed: return tr::lng_mtslink_tasks_completed(tr::now);
	}
	return QString();
}

[[nodiscard]] QString DeadlineText(const QDateTime &deadline) {
	if (!deadline.isValid()) {
		return QString();
	}
	const auto local = deadline.toLocalTime();
	const auto today = QDate::currentDate();
	const auto date = local.date();
	const auto time = QLocale().toString(local.time(), QLocale::ShortFormat);
	const auto day = (date == today)
		? tr::lng_mtslink_tasks_day_today(tr::now)
		: (date == today.addDays(1))
		? tr::lng_mtslink_tasks_day_tomorrow(tr::now)
		: (date == today.addDays(-1))
		? tr::lng_mtslink_tasks_day_yesterday(tr::now)
		: langDayOfMonthFull(date);
	return day + u", "_q + time;
}

[[nodiscard]] UserData *UserByUuid(
		not_null<Main::Session*> session,
		const QString &uuid) {
	if (uuid.isEmpty()) {
		return nullptr;
	}
	const auto user = session->data().userLoaded(
		::UserId(uuidToBareId(uuid)));
	if (!user || user->name().isEmpty()) {
		// Loaded once, the rows are repainted by the member profile.
		static auto Requested = base::flat_set<QString>();
		if (Requested.emplace(uuid).second) {
			if (const auto mts = session->account().mtsLinkSession()) {
				mts->users()->loadMember(uuid, mts->organizationId());
			}
		}
	}
	return user;
}

// A pill as the dates of the chats: the groups (collapsed by a click), the
// "Show more".
class TaskPill final : public Ui::AbstractButton {
public:
	TaskPill(
		QWidget *parent,
		not_null<const Ui::ChatStyle*> st,
		QString text);

private:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

	const not_null<const Ui::ChatStyle*> _st;
	QString _text;

public:
	void setText(const QString &text) {
		_text = text;
		update();
	}

};

TaskPill::TaskPill(
	QWidget *parent,
	not_null<const Ui::ChatStyle*> st,
	QString text)
: AbstractButton(parent)
, _st(st)
, _text(std::move(text)) {
	setCursor(style::cur_pointer);
	resize(width(), resizeGetHeight(width()));
}

int TaskPill::resizeGetHeight(int newWidth) {
	const auto &padding = st::msgServicePadding;
	return st::msgServiceMargin.top()
		+ padding.top()
		+ st::msgServiceFont->height
		+ padding.bottom()
		+ st::msgServiceMargin.bottom();
}

void TaskPill::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto &padding = st::msgServicePadding;
	const auto &font = st::msgServiceFont;
	const auto textWidth = font->width(_text);
	const auto width = textWidth + padding.left() + padding.right();
	const auto height = font->height + padding.top() + padding.bottom();
	const auto rect = QRect(
		kBubbleSide,
		st::msgServiceMargin.top(),
		width,
		height);
	HistoryView::ServiceMessagePainter::PaintBubble(p, _st, rect);
	p.setFont(font);
	p.setPen(_st->msgServiceFg());
	p.drawText(
		rect.x() + padding.left(),
		rect.y() + padding.top() + font->ascent,
		_text);
}

class TaskRow final : public Ui::RippleButton {
public:
	TaskRow(
		QWidget *parent,
		not_null<Main::Session*> session,
		Task task,
		Fn<void(Task)> toggle,
		Fn<void(Task)> open,
		Fn<void(Task)> context,
		Fn<Ui::PeerUserpicView&(not_null<PeerData*>)> userpic);

private:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;

	[[nodiscard]] QRect checkRect() const;

	const not_null<Main::Session*> _session;
	const Task _task;
	const Fn<void(Task)> _toggle;
	const Fn<void(Task)> _open;
	const Fn<void(Task)> _context;
	const Fn<Ui::PeerUserpicView&(not_null<PeerData*>)> _userpic;

};

TaskRow::TaskRow(
	QWidget *parent,
	not_null<Main::Session*> session,
	Task task,
	Fn<void(Task)> toggle,
	Fn<void(Task)> open,
	Fn<void(Task)> context,
	Fn<Ui::PeerUserpicView&(not_null<PeerData*>)> userpic)
: RippleButton(parent, st::defaultRippleAnimation)
, _session(session)
, _task(std::move(task))
, _toggle(std::move(toggle))
, _open(std::move(open))
, _context(std::move(context))
, _userpic(std::move(userpic)) {
	resize(width(), kRowHeight);
}

int TaskRow::resizeGetHeight(int newWidth) {
	return kRowHeight;
}

QRect TaskRow::checkRect() const {
	return QRect(
		kBubbleSide + kRowPadding,
		(height() - kCheckSize) / 2,
		kCheckSize,
		kCheckSize);
}

void TaskRow::mouseReleaseEvent(QMouseEvent *e) {
	const auto onCheck = checkRect().marginsAdded(
		QMargins(8, 8, 8, 8)).contains(e->pos());
	RippleButton::mouseReleaseEvent(e);
	if (!rect().contains(e->pos()) || e->button() != Qt::LeftButton) {
		return;
	}
	if (onCheck) {
		_toggle(_task);
	} else {
		_open(_task);
	}
}

void TaskRow::contextMenuEvent(QContextMenuEvent *e) {
	e->accept();
	_context(_task);
}

void TaskRow::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);

	// The bubble of an inbox message, its shadow below.
	const auto bubble = QRectF(rect().marginsRemoved({
		kBubbleSide,
		kBubbleVertical,
		kBubbleSide,
		kBubbleVertical,
	}));
	p.setPen(Qt::NoPen);
	p.setBrush(st::msgInShadow);
	p.drawRoundedRect(
		bubble.translated(0, st::msgShadow),
		kBubbleRadius,
		kBubbleRadius);
	p.setBrush(isOver() ? st::msgInBgSelected : st::msgInBg);
	p.drawRoundedRect(bubble, kBubbleRadius, kBubbleRadius);

	const auto resolved = _task.resolved();
	const auto check = checkRect();
	if (resolved) {
		p.setPen(Qt::NoPen);
		p.setBrush(st::activeButtonBg);
		p.drawEllipse(check);
		auto pen = QPen(st::activeButtonFg);
		pen.setWidthF(2.);
		pen.setCapStyle(Qt::RoundCap);
		pen.setJoinStyle(Qt::RoundJoin);
		p.setPen(pen);
		const auto c = QRectF(check);
		p.drawPolyline(QPolygonF{
			QPointF(c.x() + c.width() * 0.28, c.y() + c.height() * 0.52),
			QPointF(c.x() + c.width() * 0.44, c.y() + c.height() * 0.68),
			QPointF(c.x() + c.width() * 0.73, c.y() + c.height() * 0.36),
		});
	} else {
		auto pen = QPen(st::checkboxFg);
		pen.setWidthF(1.5);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawEllipse(QRectF(check).marginsRemoved(
			QMarginsF(0.75, 0.75, 0.75, 0.75)));
	}

	const auto assignee = UserByUuid(_session, _task.assigneeId);
	auto right = width() - kBubbleSide - kRowPadding;
	if (assignee) {
		right -= kUserpicSize;
		assignee->paintUserpicLeft(
			p,
			_userpic(assignee),
			right,
			(height() - kUserpicSize) / 2,
			width(),
			kUserpicSize);
		right -= kRowPadding / 2;
	}
	auto left = check.x() + check.width() + kRowPadding;
	if (_task.important) {
		// A big "!" before the text.
		auto font = st::semiboldFont->f;
		font.setPixelSize(st::semiboldFont->f.pixelSize() * 2);
		const auto metrics = QFontMetrics(font);
		const auto text = u"!"_q;
		p.setFont(font);
		p.setPen(st::boxTextFgError);
		p.drawText(
			left,
			(height() - metrics.height()) / 2 + metrics.ascent(),
			text);
		left += metrics.horizontalAdvance(text) + kRowPadding / 2;
	}

	const auto available = std::max(right - left, 0);
	const auto &titleFont = st::semiboldFont;
	const auto &infoFont = st::normalFont;
	const auto top = (height() - titleFont->height - infoFont->height - 2) / 2;
	p.setFont(titleFont);
	const auto over = isOver();
	p.setPen(resolved
		? (over ? st::msgInDateFgSelected : st::msgInDateFg)
		: (over ? st::historyTextInFgSelected : st::historyTextInFg));
	p.drawText(
		left,
		top + titleFont->ascent,
		titleFont->elided(_task.title, available));

	auto info = QStringList();
	const auto overdue = !resolved
		&& _task.deadline.isValid()
		&& (_task.deadline < QDateTime::currentDateTime());
	if (_task.deadline.isValid()) {
		info.push_back(DeadlineText(_task.deadline));
	}
	if (assignee) {
		info.push_back(chatListUserName(assignee));
	}
	if (!info.isEmpty()) {
		p.setFont(infoFont);
		p.setPen(overdue
			? st::boxTextFgError
			: over
			? st::msgInDateFgSelected
			: st::msgInDateFg);
		p.drawText(
			left,
			top + titleFont->height + 2 + infoFont->ascent,
			infoFont->elided(info.join(u" \xB7 "_q), available));
	}
}

void ShowTaskBox(
		not_null<Window::SessionController*> controller,
		const Task &task) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(task.title));
		box->setWidth(st::boxWideWidth);
		const auto session = &controller->session();
		const auto add = [&](const QString &label, const QString &value) {
			if (value.isEmpty()) {
				return;
			}
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				label,
				st::defaultSubsectionTitle));
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				value,
				st::boxLabel));
		};
		const auto name = [&](const QString &uuid) {
			const auto user = UserByUuid(session, uuid);
			return user ? user->name() : QString();
		};
		add(tr::lng_mtslink_tasks_description(tr::now), task.description);
		add(tr::lng_mtslink_tasks_deadline(tr::now), DeadlineText(task.deadline));
		add(tr::lng_mtslink_tasks_assignee(tr::now), name(task.assigneeId));
		add(tr::lng_mtslink_tasks_author(tr::now), name(task.authorId));
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

} // namespace

struct TasksWidget::State {
	TaskSection section = TaskSection::My;
	std::vector<Task> tasks;
	Api::TaskCounters counters;
	bool loading = false;
	bool failed = false;
	int generation = 0;
	crl::time loadedAt = 0;
	QString signature; // Of the shown list: not rebuilt when the same.
	base::unique_qptr<Ui::PopupMenu> menu;

	// No tasks: the text on the background of the chats, as in an empty chat.
	bool emptyShown = false;

	Ui::IconButton *refresh = nullptr;
	Ui::IconButton *create = nullptr;
	Ui::ScrollArea *scroll = nullptr;
	Ui::VerticalLayout *content = nullptr;
	std::vector<not_null<TaskRow*>> rows;
	std::map<not_null<PeerData*>, Ui::PeerUserpicView> userpics;
};

object_ptr<Window::SectionWidget> TasksMemento::createWidget(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		Window::Column column,
		const QRect &geometry) {
	auto result = object_ptr<TasksWidget>(parent, controller, _section);
	result->setGeometry(geometry);
	return result;
}

TasksWidget::TasksWidget(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	Api::TaskSection section)
: SectionWidget(parent, controller)
, _state(std::make_unique<State>()) {
	_state->section = section;
	setupTop();

	_state->scroll = Ui::CreateChild<Ui::ScrollArea>(
		this,
		st::historyScroll);
	_state->content = _state->scroll->setOwnedWidget(
		object_ptr<Ui::VerticalLayout>(_state->scroll)).data();

	controller->defaultChatTheme()->repaintBackgroundRequests(
	) | rpl::on_next([=] {
		update();
	}, lifetime());

	Api::Tasks::Instance().changes(
	) | rpl::on_next([=] {
		reload();
	}, lifetime());

	const auto poll = lifetime().make_state<base::Timer>([=] {
		if (!_state->loading) {
			reload();
		}
	});
	poll->callEach(kPollOpenList);

	// The names and the userpics of the assignees.
	if (const auto mts = controller->session().account().mtsLinkSession()) {
		QObject::connect(
			mts->users(),
			&Api::Users::memberLoaded,
			_state->content,
			[=] {
				for (const auto row : _state->rows) {
					row->update();
				}
			});
	}
	controller->session().downloaderTaskFinished(
	) | rpl::on_next([=] {
		for (const auto row : _state->rows) {
			row->update();
		}
	}, lifetime());

	loadCached();
	reload();
}

TasksWidget::~TasksWidget() = default;

void TasksWidget::loadCached() {
	const auto section = _state->section;
	const auto weak = base::make_weak(this);
	LoadTasksCache(&controller()->session(), section, [=](CachedTasks cached) {
		// Not when the server answered already or another section shown.
		if (!weak
			|| _state->section != section
			|| !_state->signature.isEmpty()
			|| !_state->tasks.empty()) {
			return;
		}
		_state->signature = TasksSignature(cached.tasks, cached.counters);
		_state->tasks = std::move(cached.tasks);
		_state->counters = cached.counters;
		rebuild();
	});
}

void TasksWidget::setupTop() {
	// The actions are in the context menu of the list (no buttons).
	const auto filter = [=](not_null<QEvent*> e) {
		if (e->type() == QEvent::ContextMenu) {
			showMenu(std::nullopt);
			return base::EventFilterResult::Cancel;
		}
		return base::EventFilterResult::Continue;
	};
	base::install_event_filter(this, filter);
}

void TasksWidget::showMenu(std::optional<Api::Task> task) {
	_state->menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuWithIcons);
	const auto menu = _state->menu.get();
	const auto controller = this->controller();
	menu->addAction(tr::lng_mtslink_tasks_add(tr::now), [=] {
		auto defaults = Api::TaskFields();
		defaults.important = (_state->section == TaskSection::Important);
		showTaskEditBox(controller, std::nullopt, defaults);
	}, &st::menuIconAdd);
	menu->addAction(tr::lng_mtslink_refresh_chat(tr::now), [=] {
		reload();
		refreshTaskPeers(&controller->session());
	}, &st::menuIconRestore);
	if (task) {
		const auto id = task->id;
		const auto addAction = Ui::Menu::CreateAddActionCallback(menu);
		addAction({
			.text = tr::lng_mtslink_tasks_delete(tr::now),
			.handler = [=] { confirmTaskRemove(controller, id, [] {}); },
			.icon = &st::menuIconDeleteAttention,
			.isAttention = true,
		});
	}
	menu->popup(QCursor::pos());
}

void TasksWidget::setSection(Api::TaskSection section) {
	if (_state->section == section) {
		return;
	}
	_state->section = section;
	_state->tasks.clear();
	_state->signature = QString();
	loadCached();
	reload();
	update();
}

Dialogs::RowDescriptor TasksWidget::activeChat() const {
	const auto history = controller()->session().data().historyLoaded(
		taskPeerId(_state->section));
	return history
		? Dialogs::RowDescriptor(Dialogs::Key(history), FullMsgId())
		: Dialogs::RowDescriptor();
}

void TasksWidget::reload() {
	const auto generation = ++_state->generation;
	_state->loading = true;
	_state->failed = false;
	if (_state->tasks.empty()) {
		rebuild();
	}
	const auto weak = base::make_weak(this);
	// The pages loaded already are reloaded too.
	const auto limit = std::max(kPageSize, int(_state->tasks.size()));
	Api::Tasks::Instance().list(
		Api::FilterForSection(_state->section),
		0,
		limit,
		[=](std::vector<Task> tasks, Api::TaskCounters counters) {
			if (!weak || _state->generation != generation) {
				return;
			}
			_state->loading = false;
			_state->loadedAt = crl::now();
			const auto signature = TasksSignature(tasks, counters);
			const auto was = std::exchange(_state->signature, signature);
			_state->tasks = std::move(tasks);
			_state->counters = counters;
			if (was == signature && !_state->tasks.empty()) {
				return;
			} else if (!was.isEmpty() && was != signature) {
				// Changed elsewhere: the rows of the sections too.
				refreshTaskPeers(&controller()->session());
			}
			SaveTasksCache(
				&controller()->session(),
				_state->section,
				_state->tasks,
				_state->counters);
			rebuild();
		},
		[=](QString error) {
			if (!weak || _state->generation != generation) {
				return;
			}
			_state->loading = false;
			_state->failed = true;
			_state->signature = QString();
			rebuild();
		});
	Api::Tasks::Instance().refreshAttention();
}

void TasksWidget::loadMore() {
	if (_state->loading) {
		return;
	}
	const auto generation = _state->generation;
	_state->loading = true;
	const auto weak = base::make_weak(this);
	Api::Tasks::Instance().list(
		Api::FilterForSection(_state->section),
		int(_state->tasks.size()),
		kPageSize,
		[=](std::vector<Task> tasks, Api::TaskCounters counters) {
			if (!weak || _state->generation != generation) {
				return;
			}
			_state->loading = false;
			for (auto &task : tasks) {
				_state->tasks.push_back(std::move(task));
			}
			_state->counters = counters;
			_state->signature = TasksSignature(
				_state->tasks,
				_state->counters);
			SaveTasksCache(
				&controller()->session(),
				_state->section,
				_state->tasks,
				_state->counters);
			rebuild();
		},
		[=](QString) {
			if (weak) {
				_state->loading = false;
			}
		});
}

void TasksWidget::rebuild() {
	const auto content = _state->content;
	const auto scrollTop = _state->scroll->scrollTop();
	while (content->count() > 0) {
		delete content->widgetAt(0);
	}
	_state->rows.clear();

	const auto addLabel = [&](const QString &text) {
		content->add(
			object_ptr<Ui::FlatLabel>(
				content,
				text,
				st::defaultSubsectionTitle),
			st::defaultSubsectionTitlePadding);
	};
	const auto empty = _state->tasks.empty();
	_state->scroll->setVisible(!empty);
	_state->emptyShown = empty;
	if (empty) {
		update();
	} else {
		const auto now = QDateTime::currentDateTime();
		auto groups = std::map<Group, std::vector<Task>>();
		for (const auto &task : _state->tasks) {
			groups[GroupOf(task, now)].push_back(task);
		}
		const auto session = &controller()->session();
		const auto controller = this->controller();
		const auto toggle = [=](Task task) {
			const auto resolve = !task.resolved();
			Api::Tasks::Instance().setResolved(
				task.id,
				resolve,
				[=](Api::Task) {
					// As the web client: the toast of the change.
					controller->showToast(resolve
						? tr::lng_mtslink_tasks_resolved_toast(tr::now)
						: tr::lng_mtslink_tasks_unresolved_toast(tr::now));
				},
				[=](QString) {
					controller->showToast(
						tr::lng_mtslink_tasks_save_failed(tr::now));
				});
		};
		const auto open = [=](Task task) {
			showTaskEditBox(controller, task);
		};
		const auto context = [=](Task task) {
			showMenu(task);
		};
		static auto Toggled = std::map<TaskSection, base::flat_set<Group>>();
		const auto collapsedByDefault = [](TaskSection section, Group group) {
			return (group == Group::Completed)
				&& (section != TaskSection::Completed);
		};
		const auto chatStyle = controller->chatStyle();
		const auto section = _state->section;
		const auto userpic = [=](not_null<PeerData*> peer)
		-> Ui::PeerUserpicView& {
			return _state->userpics[peer];
		};
		const auto pillText = [](Group group, bool collapsed) {
			return GroupName(group)
				+ QChar(' ')
				+ (collapsed ? QChar(0x25B8) : QChar(0x25BE));
		};
		for (const auto &[group, tasks] : groups) {
			const auto collapsed = collapsedByDefault(section, group)
				!= Toggled[section].contains(group);
			const auto pill = content->add(object_ptr<TaskPill>(
				content,
				chatStyle,
				pillText(group, collapsed)));
			const auto wrap = content->add(
				object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
					content,
					object_ptr<Ui::VerticalLayout>(content)));
			const auto inner = wrap->entity();
			for (const auto &task : tasks) {
				const auto row = inner->add(object_ptr<TaskRow>(
					inner,
					session,
					task,
					toggle,
					open,
					context,
					userpic));
				_state->rows.push_back(row);
			}
			wrap->toggle(!collapsed, anim::type::instant);
			pill->setClickedCallback([=, group = group] {
				auto &set = Toggled[section];
				if (!set.remove(group)) {
					set.emplace(group);
				}
				const auto now = collapsedByDefault(section, group)
					!= set.contains(group);
				pill->setText(pillText(group, now));
				wrap->toggle(!now, anim::type::normal);
			});
		}
		if (int(_state->tasks.size()) < _state->counters.filteredTotal) {
			const auto more = content->add(object_ptr<TaskPill>(
				content,
				chatStyle,
				tr::lng_mtslink_tasks_more(tr::now)));
			more->setClickedCallback([=] {
				loadMore();
			});
		}
		Ui::AddSkip(content);
	}
	content->resizeToWidth(_state->scroll->width());
	_state->scroll->scrollToY(scrollTop);
}

void TasksWidget::updateControlsGeometry() {
	const auto top = st::topBarHeight;
	const auto scrollTop = top;
	_state->scroll->setGeometry(
		0,
		scrollTop,
		width(),
		std::max(height() - scrollTop, 0));
	_state->content->resizeToWidth(width());
}

void TasksWidget::resizeEvent(QResizeEvent *e) {
	updateControlsGeometry();
}

void TasksWidget::paintEvent(QPaintEvent *e) {
	const auto top = st::topBarHeight;
	Window::SectionWidget::PaintBackground(
		controller(),
		controller()->defaultChatTheme().get(),
		this,
		e->rect());
	auto p = Painter(this);
	p.fillRect(0, 0, width(), top, st::windowBg);
	if (_state->emptyShown) {
		// The loading, the empty list: the service bubble, paler.
		constexpr auto kBubbleOpacity = 0.5;
		const auto &font = st::normalFont;
		const auto &padding = st::msgServicePadding;
		const auto text = _state->loading
			? tr::lng_mtslink_tasks_loading(tr::now)
			: _state->failed
			? tr::lng_mtslink_tasks_failed(tr::now)
			: tr::lng_mtslink_tasks_empty_filter(tr::now);
		const auto size = QSize(
			font->width(text) + padding.left() + padding.right(),
			font->height + padding.top() + padding.bottom());
		const auto rect = QRect(
			(width() - size.width()) / 2,
			top + (height() - top - size.height()) / 2,
			size.width(),
			size.height());
		const auto st = controller()->chatStyle();
		p.setOpacity(kBubbleOpacity);
		HistoryView::ServiceMessagePainter::PaintBubble(p, st, rect);
		// Not bright white: grayer.
		constexpr auto kTextOpacity = 0.7;
		p.setOpacity(kTextOpacity);
		p.setFont(font);
		p.setPen(st->msgServiceFg());
		p.drawText(
			rect.x() + padding.left(),
			rect.y() + padding.top() + font->ascent,
			text);
		p.setOpacity(1.);
	}
	p.setFont(st::semiboldFont);
	p.setPen(st::windowFg);
	p.drawText(
		st::defaultSubsectionTitlePadding.left(),
		(top - st::semiboldFont->height) / 2 + st::semiboldFont->ascent,
		SectionName(_state->section));
}

bool TasksWidget::showInternal(
		not_null<Window::SectionMemento*> memento,
		const Window::SectionShow &params) {
	if (const auto tasks = dynamic_cast<TasksMemento*>(memento.get())) {
		setSection(tasks->section());
		return true;
	}
	return false;
}

std::shared_ptr<Window::SectionMemento> TasksWidget::createMemento() {
	return std::make_shared<TasksMemento>(_state->section);
}

bool TasksWidget::floatPlayerHandleWheelEvent(QEvent *e) {
	return _state->scroll->viewportEvent(e);
}

QRect TasksWidget::floatPlayerAvailableRect() {
	return mapToGlobal(_state->scroll->geometry());
}

void TasksWidget::checkActivation() {
	if (!_state->loading
		&& crl::now() - _state->loadedAt > kReloadOnActivation) {
		reload();
	}
}

void showTasks(not_null<Window::SessionController*> controller) {
	controller->showSection(std::make_shared<TasksMemento>());
}

namespace {

// The chat list time of the sections: below the chats in a common folder,
// in the order of the sections.
constexpr auto kTaskPeersTime = TimeId(1000000000);
constexpr auto kTaskPreviewId = MsgId(1);
constexpr auto kTaskPeersRefresh = crl::time(2 * 60 * 1000);
constexpr auto kTasksFolderCreatedKey = "tasksFolderCreated";

[[nodiscard]] const char *SectionKey(TaskSection section) {
	switch (section) {
	case TaskSection::My: return "my";
	case TaskSection::Important: return "important";
	case TaskSection::AssignedByMe: return "assigned-by-me";
	case TaskSection::Someday: return "someday";
	case TaskSection::Completed: return "completed";
	case TaskSection::All: return "all";
	}
	return "";
}

[[nodiscard]] PeerId ComputeTaskPeerId(TaskSection section) {
	return PeerId(ChannelId(uuidToBareId(
		u"mtslink-tasks-"_q + QString::fromLatin1(SectionKey(section)))));
}

[[nodiscard]] const std::array<PeerId, kSections.size()> &TaskPeerIds() {
	static const auto result = [] {
		auto ids = std::array<PeerId, kSections.size()>();
		for (auto i = 0; i != int(kSections.size()); ++i) {
			ids[i] = ComputeTaskPeerId(kSections[i]);
		}
		return ids;
	}();
	return result;
}

// The preview of the server is not replaced by the cached one.
base::flat_set<std::pair<not_null<Main::Session*>, int>> ServerPreviews;

[[nodiscard]] bool TaskPreviewFromServer(
		not_null<Main::Session*> session,
		int index) {
	return ServerPreviews.contains(std::make_pair(session, index));
}

void ApplyTaskPreview(
		not_null<Main::Session*> session,
		int index,
		const std::vector<Task> &tasks,
		const Api::TaskCounters &counters) {
	const auto section = kSections[index];
	const auto peerId = TaskPeerIds()[index];
	const auto history = session->data().historyLoaded(peerId);
	if (!history) {
		return;
	}
	auto text = (counters.filteredTotal > 0)
		? tr::lng_mtslink_tasks_count(
			tr::now,
			lt_count,
			counters.filteredTotal)
		: tr::lng_mtslink_tasks_empty(tr::now);
	if (!tasks.empty()) {
		const auto &task = tasks.front();
		text += u" · "_q + task.title;
		if (task.deadline.isValid() && !task.resolved()) {
			text += u", "_q + DeadlineText(task.deadline);
		}
	}
	const auto date = kTaskPeersTime - index;
	if (const auto existing = session->data().message(peerId, kTaskPreviewId)) {
		if (existing->originalText().text != text) {
			existing->setText({ text });
			session->data().requestItemTextRefresh(existing);
			existing->invalidateChatListEntry();
		}
	} else {
		history->addNewLocalMessage(
			HistoryItemCommonFields{
				.id = kTaskPreviewId,
				.flags = MessageFlag::Post,
				.from = peerId,
				.date = date,
			},
			TextWithEntities{ text },
			MTP_messageMediaEmpty());
	}
	// The tasks needing attention: the counter of "My tasks".
	if (section == TaskSection::My) {
		history->setUnreadCount(counters.attention);
	}
	history->setChatListTimeId(date);
	session->data().refreshChatListEntry(Dialogs::Key(history));
}

// The userpic of a section: the icon of the web client, white on a circle
// of the colors of the empty userpics (as "Saved Messages").
[[nodiscard]] QImage SectionUserpic(TaskSection section) {
	const auto [icon, colorIndex] = [&]() -> std::pair<QString, uint8> {
		switch (section) {
		case TaskSection::My: return { u"my"_q, 5 };
		case TaskSection::All: return { u"all"_q, 4 };
		case TaskSection::Important: return { u"important"_q, 0 };
		case TaskSection::AssignedByMe:
			return { u"assigned_by_me"_q, 2 };
		case TaskSection::Someday: return { u"someday"_q, 1 };
		case TaskSection::Completed: return { u"completed"_q, 3 };
		}
		return { u"my"_q, 5 };
	}();
	constexpr auto kSize = 160;
	constexpr auto kIconSize = 88;
	auto result = QImage(kSize, kSize, QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::transparent);
	{
		auto p = QPainter(&result);
		PainterHighQualityEnabler hq(p);
		const auto colors = Ui::EmptyUserpic::UserpicColor(colorIndex);
		auto bg = QLinearGradient(0, 0, 0, kSize);
		bg.setStops({
			{ 0., colors.color1->c },
			{ 1., colors.color2->c },
		});
		p.setPen(Qt::NoPen);
		p.setBrush(bg);
		p.drawEllipse(0, 0, kSize, kSize);

		auto glyph = QImage(
			kIconSize,
			kIconSize,
			QImage::Format_ARGB32_Premultiplied);
		glyph.fill(Qt::transparent);
		{
			auto q = QPainter(&glyph);
			auto renderer = QSvgRenderer(
				u":/mtslink/tasks/"_q + icon + u".svg"_q);
			renderer.render(&q, QRectF(0, 0, kIconSize, kIconSize));
			q.setCompositionMode(QPainter::CompositionMode_SourceIn);
			q.fillRect(glyph.rect(), Qt::white);
		}
		const auto skip = (kSize - kIconSize) / 2;
		p.drawImage(skip, skip, glyph);
	}
	return result;
}

void ApplySectionUserpic(
		not_null<ChannelData*> channel,
		TaskSection section) {
	auto bytes = QByteArray();
	auto buffer = QBuffer(&bytes);
	const auto image = SectionUserpic(section);
	image.save(&buffer, "PNG");
	// The same id for the same picture: not reloaded each start.
	const auto photoId = PhotoId(uuidToBareId(
		u"mtslink-tasks-userpic-2-"_q
		+ QString::fromLatin1(SectionKey(section))));
	if (channel->userpicPhotoId() == photoId) {
		return;
	}
	channel->setUserpic(
		photoId,
		ImageLocation(
			{ .data = InMemoryLocation{ .bytes = bytes } },
			image.width(),
			image.height()),
		false);
}

void EnsureTasksFolder(not_null<Main::Session*> session) {
	using Flag = Data::ChatFilter::Flag;
	auto &filters = session->data().chatsFilters();
	auto maxId = FilterId(5);
	for (const auto &filter : filters.list()) {
		if (filter.flags() & Flag::Tasks) {
			return;
		}
		maxId = std::max(maxId, filter.id());
	}
	// Created once: a removed folder is not created again.
	auto settings = readLocalSettings();
	if (settings.value(QString::fromLatin1(kTasksFolderCreatedKey)).toBool()) {
		return;
	}
	settings.insert(QString::fromLatin1(kTasksFolderCreatedKey), true);
	writeLocalSettings(settings);
	LOG(("MtsLink Tasks: the folder %1 created").arg(maxId + 1));
	filters.set(Data::ChatFilter(
		maxId + 1,
		Data::ChatFilterTitle{ { tr::lng_mtslink_tasks(tr::now) } },
		QString(),
		std::nullopt,
		Flag::Tasks,
		{},
		{},
		{}));
}

} // namespace

bool isTaskPeer(PeerId peerId) {
	return ranges::contains(TaskPeerIds(), peerId);
}

std::optional<TaskSection> taskSectionForPeer(PeerId peerId) {
	const auto &ids = TaskPeerIds();
	const auto i = ranges::find(ids, peerId);
	return (i != end(ids))
		? std::make_optional(kSections[i - begin(ids)])
		: std::nullopt;
}

std::vector<PeerId> taskPeerIds() {
	const auto &ids = TaskPeerIds();
	return { begin(ids), end(ids) };
}

PeerId taskPeerId(TaskSection section) {
	const auto i = ranges::find(kSections, section);
	return TaskPeerIds()[i - begin(kSections)];
}

void initTaskPeers(not_null<Main::Session*> session) {
	static auto Initialized = base::flat_set<not_null<Main::Session*>>();
	if (!Initialized.emplace(session).second) {
		// A reconnect: the previews again.
		refreshTaskPeers(session);
		return;
	}
	session->lifetime().add([=] {
		Initialized.remove(session);
	});
	for (auto i = 0; i != int(kSections.size()); ++i) {
		const auto peerId = TaskPeerIds()[i];
		const auto channel = session->data().channel(peerToChannel(peerId));
		using Flag = ChannelDataFlag;
		auto flags = channel->flags();
		flags |= Flag::Broadcast;
		flags &= ~(Flag::Megagroup
			| Flag::Left
			| Flag::Forbidden
			| Flag::Forum);
		channel->setFlags(flags);
		channel->setName(SectionName(kSections[i]), {});
		ApplySectionUserpic(channel, kSections[i]);
		channel->setLoadedStatus(PeerData::LoadedStatus::Normal);
		const auto history = session->data().history(peerId);
		if (!history->folderKnown()) {
			history->clearFolder();
		}
		history->setChatListTimeId(kTaskPeersTime - i);
		history->updateChatListExistence();
		session->data().refreshChatListEntry(Dialogs::Key(history));
	}
	EnsureTasksFolder(session);
	for (auto i = 0; i != int(kSections.size()); ++i) {
		LoadTasksCache(session, kSections[i], [=](CachedTasks cached) {
			if (!TaskPreviewFromServer(session, i)) {
				ApplyTaskPreview(session, i, cached.tasks, cached.counters);
			}
		});
	}

	Api::Tasks::Instance().changes(
	) | rpl::on_next([=] {
		refreshTaskPeers(session);
	}, session->lifetime());
	const auto timer = session->lifetime().make_state<base::Timer>([=] {
		refreshTaskPeers(session);
	});
	timer->callEach(kTaskPeersRefresh);
	refreshTaskPeers(session);
}

void refreshTaskPeers(not_null<Main::Session*> session) {
	const auto weak = base::make_weak(session);
	for (auto i = 0; i != int(kSections.size()); ++i) {
		Api::Tasks::Instance().list(
			Api::FilterForSection(kSections[i]),
			0,
			1,
			[=](std::vector<Task> tasks, Api::TaskCounters counters) {
				if (const auto strong = weak.get()) {
					ServerPreviews.emplace(std::make_pair(strong, i));
					ApplyTaskPreview(strong, i, tasks, counters);
				}
			});
	}
}

} // namespace MtsLink

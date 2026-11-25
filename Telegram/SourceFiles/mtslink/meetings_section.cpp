/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.
*/
#include "mtslink/meetings_section.h"

#include "mtslink/meetings_peers.h"
#include "mtslink/outlook_calendar.h"
#include "mtslink/data_adapters.h"
#include "mtslink/call_window.h"
#include "base/event_filter.h"
#include "base/timer.h"
#include "data/data_channel.h"
#include "data/data_chat_filters.h"
#include "data/data_session.h"
#include "dialogs/dialogs_key.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_service_message.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/chat/chat_style.h"
#include "ui/chat/chat_theme.h"
#include "ui/empty_userpic.h"
#include "ui/image/image_location.h"
#include "ui/abstract_button.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_chat.h"
#include "styles/style_dialogs.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QBuffer>

namespace MtsLink {
namespace {

// The current period and the next ones: the rows of the most of them.
constexpr auto kMaxPeriods = 7;
constexpr auto kPeriodKey = "outlookMeetingsPeriod";
// In the background, the folder is shown or not: each N minutes.
constexpr auto kRefreshKey = "outlookMeetingsRefreshMinutes";
constexpr auto kDefaultRefreshMinutes = 1;
// Asked again on the open of a week not sooner.
constexpr auto kReloadOnOpen = crl::time(60 * 1000);
// The chat list time of the periods: in their order in the folder.
constexpr auto kMeetingPeersTime = TimeId(1000000000);
constexpr auto kPreviewId = MsgId(1);

constexpr auto kRowHeight = 64;
constexpr auto kBubbleSide = 12;
constexpr auto kBubbleVertical = 3;
constexpr auto kBubbleRadius = 12;
constexpr auto kRowPadding = 16;

// The meetings of all the periods, for all the sessions (the calendar is
// of this computer).
struct Meetings {
	std::optional<std::vector<OutlookMeeting>> list;
	bool loaded = false; // Asked at least once (list empty: no Outlook).
	bool loading = false;
	crl::time loadedAt = 0;
	QDate from; // The first day of the loaded periods.
	QDate till; // The day after them.
	rpl::event_stream<> changes;
};

[[nodiscard]] Meetings &MeetingsData() {
	static auto result = Meetings();
	return result;
}

// The setting, read once.
[[nodiscard]] rpl::variable<int> &RefreshMinutes() {
	static auto result = rpl::variable<int>([] {
		const auto value = readLocalSettings().value(
			QString::fromLatin1(kRefreshKey));
		return value.isDouble()
			? std::clamp(value.toInt(), 0, 10)
			: kDefaultRefreshMinutes;
	}());
	return result;
}

// The setting, read once.
[[nodiscard]] MeetingsPeriod &CurrentPeriod() {
	static auto result = [] {
		const auto value = readLocalSettings().value(
			QString::fromLatin1(kPeriodKey)).toString();
		return (value == u"day"_q)
			? MeetingsPeriod::Day
			: (value == u"month"_q)
			? MeetingsPeriod::Month
			: (value == u"quarter"_q)
			? MeetingsPeriod::Quarter
			: MeetingsPeriod::Week;
	}();
	return result;
}

[[nodiscard]] QString PeriodKey(MeetingsPeriod period) {
	switch (period) {
	case MeetingsPeriod::Day: return u"day"_q;
	case MeetingsPeriod::Week: return u"week"_q;
	case MeetingsPeriod::Month: return u"month"_q;
	case MeetingsPeriod::Quarter: return u"quarter"_q;
	}
	return u"week"_q;
}

// The rows shown: 7 days, 4 weeks, 3 months, 2 quarters.
[[nodiscard]] int PeriodsCount() {
	switch (CurrentPeriod()) {
	case MeetingsPeriod::Day: return 7;
	case MeetingsPeriod::Week: return 4;
	case MeetingsPeriod::Month: return 3;
	case MeetingsPeriod::Quarter: return 2;
	}
	return 4;
}

[[nodiscard]] QDate PeriodStart(int index) {
	const auto today = QDate::currentDate();
	switch (CurrentPeriod()) {
	case MeetingsPeriod::Day:
		return today.addDays(index);
	case MeetingsPeriod::Week:
		return today.addDays(1 - today.dayOfWeek() + 7 * index);
	case MeetingsPeriod::Month:
		return QDate(today.year(), today.month(), 1).addMonths(index);
	case MeetingsPeriod::Quarter:
		return QDate(
			today.year(),
			((today.month() - 1) / 3) * 3 + 1,
			1).addMonths(3 * index);
	}
	return today;
}

[[nodiscard]] QString PeriodDates(int index) {
	const auto start = PeriodStart(index);
	const auto end = PeriodStart(index + 1).addDays(-1);
	if (start == end) {
		return langDayOfMonthFull(start);
	} else if (start.month() == end.month() && start.year() == end.year()) {
		return QString::number(start.day())
			+ QChar(0x2013)
			+ langDayOfMonthFull(end);
	}
	return langDayOfMonthFull(start)
		+ u" \x2013 "_q
		+ langDayOfMonthFull(end);
}

[[nodiscard]] QString PeriodName(int index) {
	const auto dates = PeriodDates(index);
	switch (CurrentPeriod()) {
	case MeetingsPeriod::Day:
		return (index == 0)
			? tr::lng_mtslink_meetings_day_today(tr::now, lt_dates, dates)
			: (index == 1)
			? tr::lng_mtslink_meetings_day_tomorrow(tr::now, lt_dates, dates)
			: (langDayOfWeekFull(PeriodStart(index)) + u", "_q + dates);
	case MeetingsPeriod::Month:
		return (index == 0)
			? tr::lng_mtslink_meetings_month_current(tr::now, lt_dates, dates)
			: (index == 1)
			? tr::lng_mtslink_meetings_month_next(tr::now, lt_dates, dates)
			: langMonthFull(PeriodStart(index));
	case MeetingsPeriod::Quarter:
		return (index == 0)
			? tr::lng_mtslink_meetings_quarter_current(
				tr::now,
				lt_dates,
				dates)
			: (index == 1)
			? tr::lng_mtslink_meetings_quarter_next(tr::now, lt_dates, dates)
			: tr::lng_mtslink_meetings_quarter(tr::now, lt_dates, dates);
	case MeetingsPeriod::Week:
		break;
	}
	return (index == 0)
		? tr::lng_mtslink_meetings_week_current(tr::now, lt_dates, dates)
		: (index == 1)
		? tr::lng_mtslink_meetings_week_next(tr::now, lt_dates, dates)
		: tr::lng_mtslink_meetings_week(tr::now, lt_dates, dates);
}

// The text on the userpic of a period: the first day, the month, the
// number of the quarter.
[[nodiscard]] QString PeriodUserpicText(int index) {
	const auto start = PeriodStart(index);
	switch (CurrentPeriod()) {
	case MeetingsPeriod::Month:
		return QString::number(start.month());
	case MeetingsPeriod::Quarter: {
		static const auto kRoman = std::array<QString, 4>{
			u"I"_q,
			u"II"_q,
			u"III"_q,
			u"IV"_q,
		};
		return kRoman[(start.month() - 1) / 3];
	}
	case MeetingsPeriod::Day:
	case MeetingsPeriod::Week:
		break;
	}
	return QString::number(start.day());
}

// The meetings of a period (any part of a meeting is in it).
[[nodiscard]] std::vector<OutlookMeeting> PeriodMeetings(int index) {
	const auto &data = MeetingsData();
	if (!data.list) {
		return {};
	}
	const auto from = QDateTime(PeriodStart(index), QTime(0, 0));
	const auto till = QDateTime(PeriodStart(index + 1), QTime(0, 0));
	auto result = std::vector<OutlookMeeting>();
	for (const auto &meeting : *data.list) {
		const auto end = meeting.end.isValid() ? meeting.end : meeting.start;
		if (meeting.start < till && (end > from || meeting.start >= from)) {
			result.push_back(meeting);
		}
	}
	return result;
}

[[nodiscard]] QString TimeText(const QTime &time) {
	return QLocale().toString(time, QLocale::ShortFormat);
}

[[nodiscard]] QString MeetingTime(const OutlookMeeting &meeting) {
	if (meeting.allDay) {
		return tr::lng_mtslink_meetings_all_day(tr::now);
	}
	const auto start = TimeText(meeting.start.time());
	if (!meeting.end.isValid()) {
		return start;
	}
	const auto end = (meeting.end.date() == meeting.start.date())
		? TimeText(meeting.end.time())
		: (langDayOfMonthFull(meeting.end.date())
			+ u", "_q
			+ TimeText(meeting.end.time()));
	return start + u" \x2013 "_q + end;
}

// The link of a meeting of MTS Link in the place of the meeting
// (https://my.mts-link.ru/j/...), else empty.
[[nodiscard]] QString MeetingLink(const OutlookMeeting &meeting) {
	static const auto re = QRegularExpression(
		u"(?:https?://)?[\\w.-]*mts-link\\.ru/j/[^\\s;,<>\"')]+"_q,
		QRegularExpression::CaseInsensitiveOption);
	const auto match = re.match(meeting.location);
	if (!match.hasMatch()) {
		return QString();
	}
	return QUrl::fromUserInput(match.captured(0)).toString();
}

[[nodiscard]] QString MeetingSubject(const OutlookMeeting &meeting) {
	return meeting.subject.isEmpty()
		? tr::lng_mtslink_meetings_no_subject(tr::now)
		: meeting.subject;
}

void LoadMeetings() {
	auto &data = MeetingsData();
	if (data.loading) {
		return;
	}
	data.loading = true;
	const auto from = PeriodStart(0);
	const auto till = PeriodStart(PeriodsCount());
	loadOutlookMeetings(
		QDateTime(from, QTime(0, 0)),
		QDateTime(till, QTime(0, 0)),
		[=](std::optional<std::vector<OutlookMeeting>> result) {
			auto &data = MeetingsData();
			data.loading = false;
			data.loaded = true;
			data.loadedAt = crl::now();
			data.from = from;
			data.till = till;
			data.list = std::move(result);
			data.changes.fire({});
			// The length of the periods was changed while loading.
			if (data.list
				&& (from != PeriodStart(0)
					|| till != PeriodStart(PeriodsCount()))) {
				LoadMeetings();
			}
		});
}

// Not installed (not registered for COM) or any error of the load.
[[nodiscard]] bool NoConnection() {
	const auto &data = MeetingsData();
	return data.loaded && !data.list;
}

// -- The rows of the periods in the chats list. --

// The ids are of the weeks of the first version (the same rows).
[[nodiscard]] const std::array<PeerId, kMaxPeriods> &MeetingPeerIds() {
	static const auto result = [] {
		auto ids = std::array<PeerId, kMaxPeriods>();
		for (auto i = 0; i != kMaxPeriods; ++i) {
			ids[i] = PeerId(ChannelId(uuidToBareId(
				u"telelink-outlook-week-"_q + QString::number(i))));
		}
		return ids;
	}();
	return result;
}

[[nodiscard]] QString PreviewText(int index) {
	const auto &data = MeetingsData();
	if (!data.loaded) {
		return QString();
	} else if (NoConnection()) {
		return tr::lng_mtslink_meetings_no_outlook(tr::now);
	}
	const auto meetings = PeriodMeetings(index);
	if (meetings.empty()) {
		return tr::lng_mtslink_meetings_empty(tr::now);
	}
	auto result = tr::lng_mtslink_meetings_count(
		tr::now,
		lt_count,
		int(meetings.size()));
	// The nearest one not finished yet.
	const auto now = QDateTime::currentDateTime();
	const auto i = ranges::find_if(meetings, [&](const OutlookMeeting &m) {
		return (m.end.isValid() ? m.end : m.start) > now;
	});
	if (i != end(meetings)) {
		result += u" \xB7 "_q
			+ MeetingSubject(*i)
			+ u", "_q
			+ langDayOfWeek(i->start.date())
			+ (i->allDay
				? QString()
				: (QChar(' ') + TimeText(i->start.time())));
	}
	return result;
}

// A calendar on a circle of the colors of the empty userpics, the first
// day of the period on it (the month, the quarter).
[[nodiscard]] QImage PeriodUserpic(int index) {
	constexpr auto kSize = 160;
	auto result = QImage(kSize, kSize, QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::transparent);
	auto p = QPainter(&result);
	PainterHighQualityEnabler hq(p);
	const auto colors = Ui::EmptyUserpic::UserpicColor(
		uint8((index + 4) % 7));
	auto bg = QLinearGradient(0, 0, 0, kSize);
	bg.setStops({
		{ 0., colors.color1->c },
		{ 1., colors.color2->c },
	});
	p.setPen(Qt::NoPen);
	p.setBrush(bg);
	p.drawEllipse(0, 0, kSize, kSize);

	const auto calendar = QRectF(44, 46, 72, 70);
	auto pen = QPen(Qt::white);
	pen.setWidthF(6.);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(calendar, 12, 12);
	p.setPen(Qt::NoPen);
	p.setBrush(Qt::white);
	p.drawRoundedRect(
		QRectF(calendar.x(), calendar.y(), calendar.width(), 20),
		10,
		10);
	p.drawRect(QRectF(calendar.x(), calendar.y() + 10, calendar.width(), 10));
	p.drawRoundedRect(QRectF(60, 36, 8, 20), 4, 4);
	p.drawRoundedRect(QRectF(92, 36, 8, 20), 4, 4);

	const auto text = PeriodUserpicText(index);
	auto font = st::semiboldFont->f;
	font.setPixelSize((text.size() > 2) ? 26 : 34);
	p.setFont(font);
	p.setPen(Qt::white);
	p.drawText(
		QRectF(calendar.x(), calendar.y() + 20, calendar.width(), 50),
		Qt::AlignCenter,
		text);
	p.end();
	return result;
}

void ApplyPeriodUserpic(not_null<ChannelData*> channel, int index) {
	// The same id for the same picture: not made again each refresh.
	const auto photoId = PhotoId(uuidToBareId(
		u"telelink-outlook-userpic-"_q
		+ PeriodKey(CurrentPeriod())
		+ QChar('-')
		+ QString::number(index)
		+ QChar('-')
		+ PeriodStart(index).toString(u"yyyyMMdd"_q)));
	if (channel->userpicPhotoId() == photoId) {
		return;
	}
	const auto image = PeriodUserpic(index);
	auto bytes = QByteArray();
	auto buffer = QBuffer(&bytes);
	image.save(&buffer, "PNG");
	channel->setUserpic(
		photoId,
		ImageLocation(
			{ .data = InMemoryLocation{ .bytes = bytes } },
			image.width(),
			image.height()),
		false);
}

void ApplyPeriod(not_null<Main::Session*> session, int index) {
	const auto peerId = MeetingPeerIds()[index];
	const auto channel = session->data().channelLoaded(peerToChannel(peerId));
	const auto history = session->data().historyLoaded(peerId);
	if (!channel || !history) {
		return;
	} else if (index >= PeriodsCount()) {
		// Not shown with this length of the periods: out of the folder.
		session->data().refreshChatListEntry(Dialogs::Key(history));
		return;
	}
	const auto name = PeriodName(index);
	if (channel->name() != name) {
		channel->setName(name, {});
	}
	ApplyPeriodUserpic(channel, index);
	const auto date = kMeetingPeersTime - index;
	const auto text = PreviewText(index);
	if (const auto existing = session->data().message(peerId, kPreviewId)) {
		if (existing->originalText().text != text) {
			existing->setText({ text });
			session->data().requestItemTextRefresh(existing);
			existing->invalidateChatListEntry();
		}
	} else if (!text.isEmpty()) {
		history->addNewLocalMessage(
			HistoryItemCommonFields{
				.id = kPreviewId,
				.flags = MessageFlag::Post,
				.from = peerId,
				.date = date,
			},
			TextWithEntities{ text },
			MTP_messageMediaEmpty());
	}
	history->setChatListTimeId(date);
	session->data().refreshChatListEntry(Dialogs::Key(history));
}

void ApplyPeriods(not_null<Main::Session*> session) {
	for (auto i = 0; i != kMaxPeriods; ++i) {
		ApplyPeriod(session, i);
	}
}

// Created when the setting is turned on (removed when turned off).
void EnsureMeetingsFolder(not_null<Main::Session*> session) {
	using Flag = Data::ChatFilter::Flag;
	auto &filters = session->data().chatsFilters();
	auto maxId = FilterId(5);
	for (const auto &filter : filters.list()) {
		if (filter.flags() & Flag::Meetings) {
			return;
		}
		maxId = std::max(maxId, filter.id());
	}
	LOG(("MtsLink Outlook: the folder %1 created").arg(maxId + 1));
	filters.set(Data::ChatFilter(
		maxId + 1,
		Data::ChatFilterTitle{ { tr::lng_mtslink_meetings(tr::now) } },
		QString(),
		std::nullopt,
		Flag::Meetings,
		{},
		{},
		{}));
}

void RemoveMeetingsFolders(not_null<Main::Session*> session) {
	using Flag = Data::ChatFilter::Flag;
	auto &filters = session->data().chatsFilters();
	auto ids = std::vector<FilterId>();
	for (const auto &filter : filters.list()) {
		if (filter.flags() & Flag::Meetings) {
			ids.push_back(filter.id());
		}
	}
	for (const auto id : ids) {
		LOG(("MtsLink Outlook: the folder %1 removed").arg(id));
		filters.remove(id);
	}
}

// -- The meetings of a period. --

// A pill as the dates in the chats: the day of the meetings under it.
class DayPill final : public Ui::RpWidget {
public:
	DayPill(
		QWidget *parent,
		not_null<const Ui::ChatStyle*> st,
		QString text)
	: RpWidget(parent)
	, _st(st)
	, _text(std::move(text)) {
		resize(width(), PillHeight());
	}

	[[nodiscard]] static int PillHeight() {
		const auto &padding = st::msgServicePadding;
		return st::msgServiceMargin.top()
			+ padding.top()
			+ st::msgServiceFont->height
			+ padding.bottom()
			+ st::msgServiceMargin.bottom();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		return PillHeight();
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		const auto &padding = st::msgServicePadding;
		const auto &font = st::msgServiceFont;
		const auto rect = QRect(
			kBubbleSide,
			st::msgServiceMargin.top(),
			font->width(_text) + padding.left() + padding.right(),
			font->height + padding.top() + padding.bottom());
		HistoryView::ServiceMessagePainter::PaintBubble(p, _st, rect);
		p.setFont(font);
		p.setPen(_st->msgServiceFg());
		p.drawText(
			rect.x() + padding.left(),
			rect.y() + padding.top() + font->ascent,
			_text);
	}

private:
	const not_null<const Ui::ChatStyle*> _st;
	const QString _text;

};

// A meeting in a bubble of an inbox message: the subject, the time and the
// place under it. A click shows it in Outlook.
class MeetingRow final : public Ui::AbstractButton {
public:
	MeetingRow(QWidget *parent, OutlookMeeting meeting)
	: AbstractButton(parent)
	, _meeting(std::move(meeting))
	, _link(MeetingLink(_meeting)) {
		resize(width(), kRowHeight);
		setPointerCursor(!_meeting.entryId.isEmpty());
		setClickedCallback([=] {
			openOutlookMeeting(_meeting);
		});
		if (!_link.isEmpty()) {
			// The link of MTS Link in the place: the meeting window.
			_call = Ui::CreateChild<Ui::IconButton>(this, st::mtslinkMeetingCall);
			_call->setClickedCallback([=] {
				LOG(("MtsLink Outlook: join %1").arg(_link));
				joinCallLink(_link, MeetingSubject(_meeting));
			});
			_call->events(
			) | rpl::filter([](not_null<QEvent*> e) {
				return (e->type() == QEvent::Enter)
					|| (e->type() == QEvent::Leave);
			}) | rpl::on_next([=] {
				update();
			}, _call->lifetime());
			_call->show();
		}
	}

protected:
	int resizeGetHeight(int newWidth) override {
		if (_call) {
			_call->moveToLeft(
				newWidth - kBubbleSide - kRowPadding / 2 - _call->width(),
				(kRowHeight - _call->height()) / 2,
				newWidth);
		}
		return kRowHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
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
		p.setBrush((isOver() && !_meeting.entryId.isEmpty())
			? st::msgInBgSelected
			: st::msgInBg);
		p.drawRoundedRect(bubble, kBubbleRadius, kBubbleRadius);
		if (_call) {
			// The call button on a filled circle of the accent color.
			p.setBrush(_call->isOver()
				? st::activeButtonBgOver
				: st::activeButtonBg);
			p.drawEllipse(_call->geometry());
		}

		const auto now = QDateTime::currentDateTime();
		const auto end = _meeting.end.isValid()
			? _meeting.end
			: _meeting.start;
		const auto finished = (end <= now);
		const auto current = !finished && (_meeting.start <= now);

		const auto left = kBubbleSide + kRowPadding;
		const auto available = std::max(
			width()
				- left
				- kBubbleSide
				- kRowPadding
				- (_call ? (_call->width() - kRowPadding / 2) : 0),
			0);
		const auto &titleFont = st::semiboldFont;
		const auto &infoFont = st::normalFont;
		const auto top = (height()
			- titleFont->height
			- infoFont->height
			- 2) / 2;
		p.setFont(titleFont);
		p.setPen(finished ? st::msgInDateFg : st::historyTextInFg);
		p.drawText(
			left,
			top + titleFont->ascent,
			titleFont->elided(MeetingSubject(_meeting), available));

		auto info = MeetingTime(_meeting);
		if (!_meeting.location.isEmpty()) {
			info += u" \xB7 "_q + _meeting.location;
		}
		p.setFont(infoFont);
		p.setPen(current ? st::windowActiveTextFg : st::msgInDateFg);
		p.drawText(
			left,
			top + titleFont->height + 2 + infoFont->ascent,
			infoFont->elided(info, available));
	}

private:
	const OutlookMeeting _meeting;
	const QString _link;
	Ui::IconButton *_call = nullptr;

};

} // namespace

struct MeetingsWidget::State {
	int index = 0;
	QString emptyText; // Shown in the middle, no meetings shown.
	// The shown ones: not made again by a refresh without the changes.
	std::optional<std::vector<OutlookMeeting>> shown;
	QDate shownFrom;
	Ui::ScrollArea *scroll = nullptr;
	Ui::VerticalLayout *content = nullptr;
	base::unique_qptr<Ui::PopupMenu> menu;
};

object_ptr<Window::SectionWidget> MeetingsMemento::createWidget(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		Window::Column column,
		const QRect &geometry) {
	auto result = object_ptr<MeetingsWidget>(parent, controller, _index);
	result->setGeometry(geometry);
	return result;
}

MeetingsWidget::MeetingsWidget(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	int index)
: SectionWidget(parent, controller)
, _state(std::make_unique<State>()) {
	_state->index = std::clamp(index, 0, PeriodsCount() - 1);

	_state->scroll = Ui::CreateChild<Ui::ScrollArea>(
		this,
		st::historyScroll);
	_state->content = _state->scroll->setOwnedWidget(
		object_ptr<Ui::VerticalLayout>(_state->scroll)).data();

	controller->defaultChatTheme()->repaintBackgroundRequests(
	) | rpl::on_next([=] {
		update();
	}, lifetime());

	MeetingsData().changes.events(
	) | rpl::on_next([=] {
		rebuild();
	}, lifetime());

	// The actions are in the context menu (no buttons).
	base::install_event_filter(this, [=](not_null<QEvent*> e) {
		if (e->type() == QEvent::ContextMenu) {
			showMenu();
			return base::EventFilterResult::Cancel;
		}
		return base::EventFilterResult::Continue;
	});

	rebuild();
	checkActivation();
}

MeetingsWidget::~MeetingsWidget() = default;

void MeetingsWidget::showMenu() {
	_state->menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuWithIcons);
	const auto session = &controller()->session();
	_state->menu->addAction(tr::lng_mtslink_refresh_chat(tr::now), [=] {
		refreshMeetingPeers(session);
	}, &st::menuIconRestore);
	_state->menu->popup(QCursor::pos());
}

void MeetingsWidget::setIndex(int index) {
	index = std::clamp(index, 0, PeriodsCount() - 1);
	if (_state->index == index) {
		return;
	}
	_state->index = index;
	rebuild();
	_state->scroll->scrollToY(0);
	checkActivation();
}

Dialogs::RowDescriptor MeetingsWidget::activeChat() const {
	const auto history = controller()->session().data().historyLoaded(
		meetingPeerId(_state->index));
	return history
		? Dialogs::RowDescriptor(Dialogs::Key(history), FullMsgId())
		: Dialogs::RowDescriptor();
}

void MeetingsWidget::rebuild() {
	// The length of the periods may be changed.
	_state->index = std::clamp(_state->index, 0, PeriodsCount() - 1);
	const auto &data = MeetingsData();
	const auto meetings = PeriodMeetings(_state->index);
	const auto emptyText = !data.loaded
		? tr::lng_mtslink_meetings_loading(tr::now)
		: NoConnection()
		? tr::lng_mtslink_meetings_no_outlook(tr::now)
		: meetings.empty()
		? tr::lng_mtslink_meetings_empty(tr::now)
		: QString();
	const auto from = PeriodStart(_state->index);
	if (_state->shown
		&& *_state->shown == meetings
		&& _state->shownFrom == from
		&& _state->emptyText == emptyText) {
		// The same: only the finished / the current ones are repainted.
		_state->content->update();
		update();
		return;
	}
	_state->shown = meetings;
	_state->shownFrom = from;
	_state->emptyText = emptyText;

	const auto content = _state->content;
	const auto scrollTop = _state->scroll->scrollTop();
	while (content->count() > 0) {
		delete content->widgetAt(0);
	}
	const auto empty = !_state->emptyText.isEmpty();
	_state->scroll->setVisible(!empty);
	if (!empty) {
		const auto chatStyle = controller()->chatStyle();
		auto day = QDate();
		for (const auto &meeting : meetings) {
			// A meeting from the previous period is under its first day.
			const auto date = std::max(
				meeting.start.date(),
				PeriodStart(_state->index));
			if (date != day) {
				day = date;
				const auto name = langDayOfWeekFull(day)
					+ u", "_q
					+ langDayOfMonthFull(day);
				content->add(object_ptr<DayPill>(content, chatStyle, name));
			}
			content->add(object_ptr<MeetingRow>(content, meeting));
		}
		Ui::AddSkip(content);
		content->resizeToWidth(_state->scroll->width());
		_state->scroll->scrollToY(scrollTop);
	}
	update();
}

void MeetingsWidget::updateControlsGeometry() {
	const auto top = st::topBarHeight;
	_state->scroll->setGeometry(
		0,
		top,
		width(),
		std::max(height() - top, 0));
	_state->content->resizeToWidth(width());
}

void MeetingsWidget::resizeEvent(QResizeEvent *e) {
	updateControlsGeometry();
}

void MeetingsWidget::paintEvent(QPaintEvent *e) {
	const auto top = st::topBarHeight;
	Window::SectionWidget::PaintBackground(
		controller(),
		controller()->defaultChatTheme().get(),
		this,
		e->rect());
	auto p = Painter(this);
	p.fillRect(0, 0, width(), top, st::windowBg);
	if (!_state->emptyText.isEmpty()) {
		// As the text of an empty chat: the service bubble in the middle.
		constexpr auto kBubbleOpacity = 0.5;
		constexpr auto kTextOpacity = 0.7;
		const auto &font = st::normalFont;
		const auto &padding = st::msgServicePadding;
		const auto &text = _state->emptyText;
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
		PeriodName(_state->index));
}

bool MeetingsWidget::showInternal(
		not_null<Window::SectionMemento*> memento,
		const Window::SectionShow &params) {
	if (const auto meetings = dynamic_cast<MeetingsMemento*>(memento.get())) {
		setIndex(meetings->index());
		return true;
	}
	return false;
}

std::shared_ptr<Window::SectionMemento> MeetingsWidget::createMemento() {
	return std::make_shared<MeetingsMemento>(_state->index);
}

bool MeetingsWidget::floatPlayerHandleWheelEvent(QEvent *e) {
	return _state->scroll->viewportEvent(e);
}

QRect MeetingsWidget::floatPlayerAvailableRect() {
	return mapToGlobal(_state->scroll->geometry());
}

void MeetingsWidget::checkActivation() {
	const auto &data = MeetingsData();
	if (!data.loading
		&& (!data.loaded
			|| (RefreshMinutes().current() > 0
				&& crl::now() - data.loadedAt > kReloadOnOpen)
			|| (data.from != PeriodStart(0))
			|| (data.till != PeriodStart(PeriodsCount())))) {
		refreshMeetingPeers(&controller()->session());
	}
}

MeetingsPeriod meetingsPeriod() {
	return CurrentPeriod();
}

QString meetingsPeriodName(MeetingsPeriod period) {
	switch (period) {
	case MeetingsPeriod::Day:
		return tr::lng_mtslink_meetings_period_day(tr::now);
	case MeetingsPeriod::Week:
		return tr::lng_mtslink_meetings_period_week(tr::now);
	case MeetingsPeriod::Month:
		return tr::lng_mtslink_meetings_period_month(tr::now);
	case MeetingsPeriod::Quarter:
		return tr::lng_mtslink_meetings_period_quarter(tr::now);
	}
	return QString();
}

std::vector<MeetingsPeriod> meetingsPeriods() {
	return {
		MeetingsPeriod::Day,
		MeetingsPeriod::Week,
		MeetingsPeriod::Month,
		MeetingsPeriod::Quarter,
	};
}

int meetingsRefreshMinutes() {
	return RefreshMinutes().current();
}

std::vector<int> meetingsRefreshOptions() {
	return { 0, 1, 2, 5, 10 };
}

QString meetingsRefreshName(int minutes) {
	return (minutes <= 0)
		? tr::lng_mtslink_meetings_refresh_manual(tr::now)
		: (minutes == 1)
		? tr::lng_mtslink_meetings_refresh_minute(tr::now)
		: tr::lng_mtslink_meetings_refresh_minutes(
			tr::now,
			lt_count,
			minutes);
}

void setMeetingsRefreshMinutes(int minutes) {
	minutes = std::clamp(minutes, 0, 10);
	if (RefreshMinutes().current() == minutes) {
		return;
	}
	auto settings = readLocalSettings();
	settings.insert(QString::fromLatin1(kRefreshKey), minutes);
	writeLocalSettings(settings);
	LOG(("MtsLink Outlook: the refresh each %1 minutes").arg(minutes));
	RefreshMinutes() = minutes;
}

bool meetingsEnabled() {
	return outlookMeetingsEnabled();
}

bool isMeetingPeer(PeerId peerId) {
	return outlookSupported() && ranges::contains(MeetingPeerIds(), peerId);
}

std::optional<int> meetingIndexForPeer(PeerId peerId) {
	if (!outlookSupported()) {
		return std::nullopt;
	}
	const auto &ids = MeetingPeerIds();
	const auto i = ranges::find(ids, peerId);
	const auto index = int(i - begin(ids));
	return (i != end(ids) && index < PeriodsCount())
		? std::make_optional(index)
		: std::nullopt;
}

PeerId meetingPeerId(int index) {
	return MeetingPeerIds()[std::clamp(index, 0, PeriodsCount() - 1)];
}

std::vector<PeerId> meetingPeerIds() {
	if (!outlookSupported()) {
		return {};
	}
	const auto &ids = MeetingPeerIds();
	return { begin(ids), end(ids) };
}

void initMeetingPeers(not_null<Main::Session*> session) {
	if (!outlookMeetingsEnabled()) {
		// The setting is off: no folder (one of the earlier version).
		RemoveMeetingsFolders(session);
		return;
	}
	static auto Initialized = base::flat_set<not_null<Main::Session*>>();
	if (!Initialized.emplace(session).second) {
		// A reconnect: the meetings again.
		refreshMeetingPeers(session);
		return;
	}
	session->lifetime().add([=] {
		Initialized.remove(session);
	});
	for (auto i = 0; i != kMaxPeriods; ++i) {
		const auto peerId = MeetingPeerIds()[i];
		const auto channel = session->data().channel(peerToChannel(peerId));
		using Flag = ChannelDataFlag;
		auto flags = channel->flags();
		flags |= Flag::Broadcast;
		flags &= ~(Flag::Megagroup
			| Flag::Left
			| Flag::Forbidden
			| Flag::Forum);
		channel->setFlags(flags);
		channel->setName(PeriodName(i), {});
		ApplyPeriodUserpic(channel, i);
		channel->setLoadedStatus(PeerData::LoadedStatus::Normal);
		const auto history = session->data().history(peerId);
		if (!history->folderKnown()) {
			history->clearFolder();
		}
		history->setChatListTimeId(kMeetingPeersTime - i);
		history->updateChatListExistence();
		session->data().refreshChatListEntry(Dialogs::Key(history));
	}
	EnsureMeetingsFolder(session);

	MeetingsData().changes.events(
	) | rpl::on_next([=] {
		ApplyPeriods(session);
	}, session->lifetime());

	const auto timer = session->lifetime().make_state<base::Timer>([=] {
		refreshMeetingPeers(session);
	});
	RefreshMinutes().value(
	) | rpl::on_next([=](int minutes) {
		timer->cancel();
		if (minutes > 0) {
			timer->callEach(crl::time(minutes) * 60 * 1000);
		}
	}, session->lifetime());

	// The registration of Outlook first: not asked at all without it.
	const auto weak = base::make_weak(session);
	checkOutlookAvailable([=] {
		if (const auto strong = weak.get()) {
			refreshMeetingPeers(strong);
		}
	});
}

void setMeetingsEnabled(not_null<Main::Session*> session, bool enabled) {
	setOutlookMeetingsEnabled(enabled);
	if (enabled) {
		initMeetingPeers(session);
		EnsureMeetingsFolder(session);
	} else {
		RemoveMeetingsFolders(session);
	}
}

void refreshMeetingPeers(not_null<Main::Session*> session) {
	if (!outlookMeetingsEnabled()) {
		return;
	}
	// The names of the periods (a new one started).
	ApplyPeriods(session);
	LoadMeetings();
}

void setMeetingsPeriod(
		not_null<Main::Session*> session,
		MeetingsPeriod period) {
	if (CurrentPeriod() == period) {
		return;
	}
	CurrentPeriod() = period;
	auto settings = readLocalSettings();
	settings.insert(QString::fromLatin1(kPeriodKey), PeriodKey(period));
	writeLocalSettings(settings);
	LOG(("MtsLink Outlook: the period %1").arg(PeriodKey(period)));
	// The rows: shown in the folder again (or hidden), the meetings again.
	refreshMeetingPeers(session);
	MeetingsData().changes.fire({});
}

} // namespace MtsLink

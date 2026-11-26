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
#include "core/application.h"
#include "core/core_settings.h"
#include "ui/effects/animations.h"
#include "window/notifications_manager.h"
#include "data/data_channel.h"
#include "data/data_chat_filters.h"
#include "data/data_session.h"
#include "data/data_user.h"
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
#include "ui/widgets/tooltip.h"
#include "ui/ui_utility.h"
#include "ui/wrap/slide_wrap.h"
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
#include <QtCore/QJsonArray>

namespace MtsLink {
namespace {

// The current period and the next ones: the rows of the most of them.
constexpr auto kMaxPeriods = 14;
constexpr auto kPeriodKey = "outlookMeetingsPeriod";
// The number of the periods in the chats list, for each length.
constexpr auto kPeriodsCountsKey = "outlookMeetingsPeriodsCounts";
// In the background, the folder is shown or not: each N minutes.
constexpr auto kRefreshKey = "outlookMeetingsRefreshMinutes"; // Earlier.
constexpr auto kRefreshSecondsKey = "outlookMeetingsRefreshSeconds";
constexpr auto kDefaultRefreshSeconds = 60;
constexpr auto kMaxRefreshSeconds = 600;
// The rows of the meetings: on the whole width (else as a message), the
// call button at the right (else at the left).
constexpr auto kFullWidthKey = "outlookMeetingsFullWidth";
constexpr auto kCallLeftKey = "outlookMeetingsCallLeft";
// The participants: as the mentions (@name), else as the names.
constexpr auto kPeopleMentionsKey = "outlookMeetingsPeopleMentions";
// The reminders before the meetings, in minutes.
constexpr auto kRemindKey = "outlookMeetingsRemindMinutes";
constexpr auto kDefaultRemindMinutes = 5;
constexpr auto kRemindSoundKey = "outlookMeetingsRemindSound";
constexpr auto kRemindColorKey = "outlookMeetingsRemindColor";
constexpr auto kDefaultRemindSound = "msg_incoming";
// The past days (not today) are shown in the lists.
constexpr auto kShowPastKey = "outlookMeetingsShowPast";
// The weekends (Saturday, Sunday) are shown: the days, the meetings.
constexpr auto kShowWeekendsKey = "outlookMeetingsShowWeekends";
// The participants are named in the list: not more of them.
constexpr auto kMaxNamedAttendees = 10;
// The colors of the userpics of the days (of the empty userpics).
constexpr auto kWorkdayColorIndex = 5;
constexpr auto kWeekendColorIndex = 0;
// OlResponseStatus.
constexpr auto kResponseOrganized = 1;
constexpr auto kResponseTentative = 2;
constexpr auto kResponseAccepted = 3;
constexpr auto kResponseDeclined = 4;
constexpr auto kResponseNotResponded = 5;
// Without the reminders: the meetings starting soon are highlighted.
constexpr auto kDefaultUpcomingMinutes = 15;
constexpr auto kFlashDuration = crl::time(3000);
// The ids of the reminders: not of the messages (the preview is 1).
constexpr auto kRemindFirstId = MsgId(1000000);
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
// The call button: from the border of the bubble, from the text.
constexpr auto kCallMargin = 14;
constexpr auto kCallGap = 14;

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

// The setting, read once (the minutes of the earlier version too).
[[nodiscard]] rpl::variable<int> &RefreshSeconds() {
	static auto result = rpl::variable<int>([] {
		const auto settings = readLocalSettings();
		const auto seconds = settings.value(
			QString::fromLatin1(kRefreshSecondsKey));
		if (seconds.isDouble()) {
			return std::clamp(seconds.toInt(), 0, kMaxRefreshSeconds);
		}
		const auto minutes = settings.value(QString::fromLatin1(kRefreshKey));
		return minutes.isDouble()
			? std::clamp(minutes.toInt() * 60, 0, kMaxRefreshSeconds)
			: kDefaultRefreshSeconds;
	}());
	return result;
}

// The setting, read once.
[[nodiscard]] bool &FullWidth() {
	static auto result = [] {
		const auto value = readLocalSettings().value(
			QString::fromLatin1(kFullWidthKey));
		return value.isBool() ? value.toBool() : true;
	}();
	return result;
}

// The setting, read once.
[[nodiscard]] bool &CallLeft() {
	static auto result = readLocalSettings().value(
		QString::fromLatin1(kCallLeftKey)).toBool();
	return result;
}

// The setting, read once.
[[nodiscard]] bool &PeopleAsMentions() {
	static auto result = readLocalSettings().value(
		QString::fromLatin1(kPeopleMentionsKey)).toBool();
	return result;
}

// The rows of the lists are made again (the width, the call button).
[[nodiscard]] rpl::event_stream<> &LayoutChanges() {
	static auto result = rpl::event_stream<>();
	return result;
}

// The setting, read once.
[[nodiscard]] base::flat_set<int> &RemindMinutes() {
	static auto result = [] {
		auto list = base::flat_set<int>();
		const auto value = readLocalSettings().value(
			QString::fromLatin1(kRemindKey));
		if (!value.isArray()) {
			list.emplace(kDefaultRemindMinutes);
			return list;
		}
		for (const auto entry : value.toArray()) {
			const auto minutes = entry.toInt();
			if (minutes > 0) {
				list.emplace(minutes);
			}
		}
		return list;
	}();
	return result;
}

// The setting, read once.
[[nodiscard]] QString &RemindSound() {
	static auto result = [] {
		const auto value = readLocalSettings().value(
			QString::fromLatin1(kRemindSoundKey)).toString();
		return value.isEmpty() ? QString::fromLatin1(kDefaultRemindSound) : value;
	}();
	return result;
}

// The setting, read once.
[[nodiscard]] std::optional<QColor> &RemindColor() {
	static auto result = [] {
		const auto value = QColor(readLocalSettings().value(
			QString::fromLatin1(kRemindColorKey)).toString());
		return value.isValid()
			? std::make_optional(value)
			: std::optional<QColor>();
	}();
	return result;
}

// The color of the reminders in the list.
[[nodiscard]] QColor RemindAccent() {
	return RemindColor().value_or(st::windowBgActive->c);
}

// The setting, read once.
[[nodiscard]] bool &ShowPast() {
	static auto result = [] {
		const auto value = readLocalSettings().value(
			QString::fromLatin1(kShowPastKey));
		return value.isBool() ? value.toBool() : true;
	}();
	return result;
}

// The setting, read once.
[[nodiscard]] bool &ShowWeekends() {
	static auto result = [] {
		const auto value = readLocalSettings().value(
			QString::fromLatin1(kShowWeekendsKey));
		return value.isBool() ? value.toBool() : true;
	}();
	return result;
}

[[nodiscard]] bool IsWeekend(QDate date) {
	return date.dayOfWeek() >= 6;
}

// The day itself or the next one not hidden (a weekend).
[[nodiscard]] QDate ShownDay(QDate date) {
	if (!ShowWeekends()) {
		while (IsWeekend(date)) {
			date = date.addDays(1);
		}
	}
	return date;
}

// A meeting starts soon: as the longest of the reminders.
[[nodiscard]] int UpcomingSeconds() {
	const auto &leads = RemindMinutes();
	return 60 * (leads.empty()
		? kDefaultUpcomingMinutes
		: *(end(leads) - 1));
}

// The reminders shown: their meetings.
[[nodiscard]] base::flat_map<MsgId, QString> &ReminderKeys() {
	static auto result = base::flat_map<MsgId, QString>();
	return result;
}

// The meeting of a clicked reminder, to be shown in the list.
[[nodiscard]] QString &PendingMeeting() {
	static auto result = QString();
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

[[nodiscard]] int MaxPeriodsCount(MeetingsPeriod period) {
	switch (period) {
	case MeetingsPeriod::Day: return 14;
	case MeetingsPeriod::Week: return 8;
	case MeetingsPeriod::Month: return 12;
	case MeetingsPeriod::Quarter: return 4;
	}
	return 4;
}

[[nodiscard]] int DefaultPeriodsCount(MeetingsPeriod period) {
	switch (period) {
	case MeetingsPeriod::Day: return 7;
	case MeetingsPeriod::Week: return 4;
	case MeetingsPeriod::Month: return 3;
	case MeetingsPeriod::Quarter: return 2;
	}
	return 4;
}

// The setting, read once: the rows of each length (7 days, 4 weeks,
// 3 months, 2 quarters by default).
[[nodiscard]] base::flat_map<MeetingsPeriod, int> &PeriodsCounts() {
	static auto result = [] {
		auto counts = base::flat_map<MeetingsPeriod, int>();
		const auto saved = readLocalSettings().value(
			QString::fromLatin1(kPeriodsCountsKey)).toObject();
		for (const auto period : {
				MeetingsPeriod::Day,
				MeetingsPeriod::Week,
				MeetingsPeriod::Month,
				MeetingsPeriod::Quarter }) {
			const auto value = saved.value(PeriodKey(period));
			counts[period] = value.isDouble()
				? std::clamp(value.toInt(), 1, MaxPeriodsCount(period))
				: DefaultPeriodsCount(period);
		}
		return counts;
	}();
	return result;
}

// The rows shown.
[[nodiscard]] int PeriodsCount() {
	return PeriodsCounts()[CurrentPeriod()];
}

[[nodiscard]] QDate PeriodStart(int index) {
	const auto today = QDate::currentDate();
	switch (CurrentPeriod()) {
	case MeetingsPeriod::Day: {
		// Without the weekends: the next work days.
		auto day = ShownDay(today);
		for (auto i = 0; i != index; ++i) {
			day = ShownDay(day.addDays(1));
		}
		return day;
	}
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

// The day after the period (a day: the next day, even a hidden one).
[[nodiscard]] QDate PeriodEnd(int index) {
	return (CurrentPeriod() == MeetingsPeriod::Day)
		? PeriodStart(index).addDays(1)
		: PeriodStart(index + 1);
}

[[nodiscard]] QString PeriodDates(int index) {
	const auto start = PeriodStart(index);
	const auto end = PeriodEnd(index).addDays(-1);
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
	case MeetingsPeriod::Day: {
		// Without the weekends the first one may be not today.
		const auto start = PeriodStart(index);
		const auto today = QDate::currentDate();
		return (start == today)
			? tr::lng_mtslink_meetings_day_today(tr::now, lt_dates, dates)
			: (start == today.addDays(1))
			? tr::lng_mtslink_meetings_day_tomorrow(tr::now, lt_dates, dates)
			: (langDayOfWeekFull(start) + u", "_q + dates);
	}
	case MeetingsPeriod::Month:
		return (index == 0)
			? tr::lng_mtslink_meetings_month_current(tr::now, lt_dates, dates)
			: (index == 1)
			? tr::lng_mtslink_meetings_month_next(tr::now, lt_dates, dates)
			: dates;
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
	const auto till = QDateTime(PeriodEnd(index), QTime(0, 0));
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

// The same meeting in the lists loaded again.
[[nodiscard]] QString MeetingKey(const OutlookMeeting &meeting) {
	return meeting.entryId
		+ QChar('/')
		+ meeting.subject
		+ QChar('/')
		+ meeting.start.toString(Qt::ISODate);
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
			if (!outlookMeetingsEnabled()) {
				// Turned off while loading: nothing kept.
				LOG(("MtsLink Outlook: the loaded meetings dropped "
					"(the integration is off)"));
				return;
			}
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

// The last day of a meeting (the one till the midnight: the day before).
[[nodiscard]] QDate MeetingLastDate(const OutlookMeeting &meeting) {
	return (!meeting.end.isValid() || meeting.end <= meeting.start)
		? meeting.start.date()
		: meeting.end.addSecs(-1).date();
}

// The day of the meeting in the list: from the previous period under the
// first day of it, from the hidden past days under today, from a hidden
// weekend under the next work day.
[[nodiscard]] QDate RowDate(const OutlookMeeting &meeting, int index) {
	auto result = std::max(meeting.start.date(), PeriodStart(index));
	if (!ShowPast()) {
		result = std::max(result, QDate::currentDate());
	}
	return ShownDay(result);
}

// The meetings of the list of a period: without the past days if the
// setting is off (the finished meetings of today are shown), without the
// weekends if that setting is off (the ones only on a weekend).
[[nodiscard]] std::vector<OutlookMeeting> VisibleMeetings(int index) {
	auto result = PeriodMeetings(index);
	const auto end = PeriodEnd(index);
	result.erase(ranges::remove_if(result, [&](const OutlookMeeting &m) {
		const auto date = RowDate(m, index);
		return (date > MeetingLastDate(m)) || (date >= end);
	}), result.end());
	return result;
}

// The participants without the organizer.
[[nodiscard]] std::vector<OutlookPerson> MeetingAttendees(
		const OutlookMeeting &meeting) {
	auto result = std::vector<OutlookPerson>();
	const auto add = [&](const std::vector<OutlookPerson> &list) {
		for (const auto &person : list) {
			if (person.name != meeting.organizer.name
				&& !ranges::contains(
					result,
					person.name,
					&OutlookPerson::name)) {
				result.push_back(person);
			}
		}
	};
	add(meeting.required);
	add(meeting.optional);
	return result;
}

// The line under the subject: the time, the place.
[[nodiscard]] QString MeetingInfo(const OutlookMeeting &meeting) {
	auto result = MeetingTime(meeting);
	if (!meeting.location.isEmpty()) {
		result += u" \xB7 "_q + meeting.location;
	}
	return result;
}

// -- The participants as the members of MTS Link. --

struct PersonEntry {
	UserData *user = nullptr;
	bool requested = false;
};

[[nodiscard]] QHash<QString, PersonEntry> &People() {
	static auto result = QHash<QString, PersonEntry>();
	return result;
}

// One request at a time (not many at once to the server).
[[nodiscard]] std::deque<std::pair<QString, OutlookPerson>> &PeopleQueue() {
	static auto result = std::deque<std::pair<QString, OutlookPerson>>();
	return result;
}

[[nodiscard]] bool &PeopleRequesting() {
	static auto result = false;
	return result;
}

[[nodiscard]] bool &PeopleFound() {
	static auto result = false;
	return result;
}

[[nodiscard]] QString PersonKey(const OutlookPerson &person) {
	return person.email.isEmpty()
		? (u"name:"_q + person.name)
		: person.email.toLower();
}

void RequestNextPerson(base::weak_ptr<Main::Session> weak) {
	auto &queue = PeopleQueue();
	const auto session = weak.get();
	if (queue.empty() || !session) {
		PeopleRequesting() = false;
		if (base::take(PeopleFound())) {
			// The names of the found ones: the rows again.
			LayoutChanges().fire({});
		}
		return;
	}
	PeopleRequesting() = true;
	const auto key = queue.front().first;
	const auto person = queue.front().second;
	queue.pop_front();
	findOrganizationMember(session, person.email, person.name, [=](
			UserData *user) {
		if (user) {
			People()[key].user = user;
			PeopleFound() = true;
		}
		RequestNextPerson(weak);
	});
}

// The member of a participant if found, else requested (once).
[[nodiscard]] UserData *PersonUser(
		not_null<Main::Session*> session,
		const OutlookPerson &person) {
	if (person.name.isEmpty() && person.email.isEmpty()) {
		return nullptr;
	}
	const auto key = PersonKey(person);
	auto &entry = People()[key];
	if (!entry.requested) {
		entry.requested = true;
		PeopleQueue().emplace_back(key, person);
		if (!PeopleRequesting()) {
			PeopleRequesting() = true;
			const auto weak = base::make_weak(session);
			crl::on_main([=] {
				RequestNextPerson(weak);
			});
		}
	}
	return entry.user;
}

// A part of a line under the time: a text or a member (a link to the
// profile).
struct PeopleSegment {
	QString text;
	UserData *user = nullptr;
};
using PeopleLine = std::vector<PeopleSegment>;

[[nodiscard]] PeopleSegment PersonSegment(
		not_null<Main::Session*> session,
		const OutlookPerson &person) {
	const auto user = PersonUser(session, person);
	return {
		.text = !user
			? person.name
			: PeopleAsMentions()
			? (u"@"_q + user->name())
			: chatListUserName(user),
		.user = user,
	};
}

// The lines under the time: the organizer with the answer, the participants.
[[nodiscard]] std::vector<PeopleLine> PeopleLines(
		not_null<Main::Session*> session,
		const OutlookMeeting &meeting) {
	auto first = PeopleLine();
	const auto separate = [&] {
		if (!first.empty()) {
			first.push_back({ .text = u" \xB7 "_q });
		}
	};
	if (meeting.response == kResponseOrganized) {
		first.push_back({
			.text = tr::lng_mtslink_meetings_organizer_you(tr::now),
		});
	} else if (!meeting.organizer.name.isEmpty()) {
		first.push_back({
			.text = tr::lng_mtslink_meetings_organizer_label(tr::now)
				+ QChar(' '),
		});
		first.push_back(PersonSegment(session, meeting.organizer));
	}
	const auto answer = [&]() -> QString {
		switch (meeting.response) {
		case kResponseTentative:
			return tr::lng_mtslink_meetings_response_tentative(tr::now);
		case kResponseAccepted:
			return tr::lng_mtslink_meetings_response_accepted(tr::now);
		case kResponseDeclined:
			return tr::lng_mtslink_meetings_response_declined(tr::now);
		case kResponseNotResponded:
			return tr::lng_mtslink_meetings_response_none(tr::now);
		}
		return QString();
	}();
	if (!answer.isEmpty()) {
		separate();
		first.push_back({ .text = answer });
	}
	auto result = std::vector<PeopleLine>();
	if (!first.empty()) {
		result.push_back(std::move(first));
	}
	const auto attendees = MeetingAttendees(meeting);
	const auto count = int(attendees.size());
	if (count > kMaxNamedAttendees) {
		result.push_back({ {
			.text = tr::lng_mtslink_meetings_attendees(
				tr::now,
				lt_count,
				count),
		} });
	} else if (count > 0) {
		auto line = PeopleLine{ {
			.text = tr::lng_mtslink_meetings_attendees_label(tr::now)
				+ QChar(' '),
		} };
		for (auto i = 0; i != count; ++i) {
			if (i > 0) {
				line.push_back({ .text = u", "_q });
			}
			line.push_back(PersonSegment(session, attendees[i]));
		}
		result.push_back(std::move(line));
	}
	return result;
}

// All the names: in the tooltip of the meeting.
[[nodiscard]] QString PeopleTooltip(const OutlookMeeting &meeting) {
	const auto names = [&](const std::vector<OutlookPerson> &list) {
		auto result = QStringList();
		for (const auto &person : list) {
			if (person.name != meeting.organizer.name) {
				result.push_back(person.name);
			}
		}
		return result.join(u", "_q);
	};
	auto lines = QStringList();
	if (!meeting.organizer.name.isEmpty()) {
		lines.push_back(tr::lng_mtslink_meetings_organizer(
			tr::now,
			lt_name,
			meeting.organizer.name));
	}
	if (const auto required = names(meeting.required); !required.isEmpty()) {
		lines.push_back(tr::lng_mtslink_meetings_required(
			tr::now,
			lt_names,
			required));
	}
	if (const auto optional = names(meeting.optional); !optional.isEmpty()) {
		lines.push_back(tr::lng_mtslink_meetings_optional(
			tr::now,
			lt_names,
			optional));
	}
	return lines.join(QChar('\n'));
}

[[nodiscard]] QString PreviewText(int index) {
	const auto &data = MeetingsData();
	if (!data.loaded) {
		return QString();
	} else if (NoConnection()) {
		return tr::lng_mtslink_meetings_no_outlook(tr::now);
	}
	const auto meetings = VisibleMeetings(index);
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
	const auto today = (CurrentPeriod() == MeetingsPeriod::Day)
		&& (PeriodStart(index) == QDate::currentDate());
	const auto colors = Ui::EmptyUserpic::UserpicColor(
		(CurrentPeriod() != MeetingsPeriod::Day)
			? uint8((index + 4) % 7)
			: IsWeekend(PeriodStart(index))
			? uint8(kWeekendColorIndex)
			: uint8(kWorkdayColorIndex));
	auto bg = QLinearGradient(0, 0, 0, kSize);
	bg.setStops({
		// Today: as its day in the list of the meetings.
		{ 0., today ? st::windowBgActive->c : colors.color1->c },
		{ 1., today ? st::windowBgActive->c : colors.color2->c },
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
		u"telelink-outlook-userpic-v2-"_q
		+ st::windowBgActive->c.name()
		+ QChar('-')
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

// The period of a day: 0 is the current one, 1 the next one, ...
[[nodiscard]] int PeriodIndexForDate(QDate date) {
	const auto count = PeriodsCount();
	for (auto i = 0; i != count; ++i) {
		if (date < PeriodEnd(i)) {
			return i;
		}
	}
	return count - 1;
}

// Each minute: the reminders of the meetings started soon. One for each of
// the settings (30, 15, ... minutes), only the nearest one after a pause
// (the start of the app, the sleep of the computer).
void CheckReminders(not_null<Main::Session*> session) {
	const auto &data = MeetingsData();
	const auto &leads = RemindMinutes();
	if (!outlookMeetingsEnabled() || !data.list || leads.empty()) {
		return;
	}
	static auto Shown = base::flat_set<QString>();
	static auto NextId = kRemindFirstId;
	const auto now = QDateTime::currentDateTime();
	for (const auto &meeting : *data.list) {
		if (meeting.allDay
			|| !meeting.start.isValid()
			|| meeting.start <= now) {
			continue;
		}
		const auto left = now.secsTo(meeting.start);
		const auto i = ranges::find_if(leads, [&](int minutes) {
			return left <= minutes * 60;
		});
		if (i == end(leads)) {
			continue;
		}
		const auto key = MeetingKey(meeting) + QChar('/');
		if (Shown.contains(key + QString::number(*i))) {
			continue;
		}
		for (auto j = i; j != end(leads); ++j) {
			Shown.emplace(key + QString::number(*j));
		}
		const auto index = PeriodIndexForDate(meeting.start.date());
		const auto peer = session->data().peerLoaded(MeetingPeerIds()[index]);
		if (!peer) {
			continue;
		}
		const auto minutes = int((left + 59) / 60);
		auto text = tr::lng_mtslink_meetings_remind_in(
			tr::now,
			lt_count,
			minutes);
		if (!meeting.location.isEmpty()) {
			text += u" \xB7 "_q + meeting.location;
		}
		LOG(("MtsLink Outlook: the reminder of a meeting in %1 minutes "
			"(the setting %2, the period %3)"
			).arg(minutes
			).arg(*i
			).arg(index));
		const auto id = NextId++;
		ReminderKeys().emplace(id, MeetingKey(meeting));
		Core::App().notifications().showCustom(
			peer,
			id,
			MeetingSubject(meeting),
			MeetingTime(meeting),
			text,
			meetingsRemindSoundPath(RemindSound()),
			RemindColor());
	}
}

// -- The meetings of a period. --

// A pill as the dates in the chats: the day of the meetings under it
// (collapsed by a click, as the groups of the tasks).
class DayPill final : public Ui::AbstractButton {
public:
	DayPill(
		QWidget *parent,
		not_null<const Ui::ChatStyle*> st,
		QString text,
		bool past,
		bool today)
	: AbstractButton(parent)
	, _st(st)
	, _text(std::move(text))
	, _past(past)
	, _today(today) {
		setCursor(style::cur_pointer);
		resize(width(), PillHeight());
	}

	void setText(const QString &text) {
		_text = text;
		update();
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
		if (_past) {
			// As the finished meetings: grey.
			p.setOpacity(0.5);
		}
		const auto &padding = st::msgServicePadding;
		const auto &font = st::msgServiceFont;
		const auto rect = QRect(
			kBubbleSide,
			st::msgServiceMargin.top(),
			font->width(_text) + padding.left() + padding.right(),
			font->height + padding.top() + padding.bottom());
		if (_today) {
			// Today: the accent color of the theme.
			auto hq = PainterHighQualityEnabler(p);
			const auto radius = rect.height() / 2.;
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgActive);
			p.drawRoundedRect(rect, radius, radius);
			p.setPen(st::windowFgActive);
		} else {
			HistoryView::ServiceMessagePainter::PaintBubble(p, _st, rect);
			p.setPen(_st->msgServiceFg());
		}
		p.setFont(font);
		p.drawText(
			rect.x() + padding.left(),
			rect.y() + padding.top() + font->ascent,
			_text);
	}

private:
	const not_null<const Ui::ChatStyle*> _st;
	QString _text;
	const bool _past = false;
	const bool _today = false;

};

// The days toggled by the user (while the app is running): the past ones
// are collapsed by default, the others are expanded.
[[nodiscard]] base::flat_set<QDate> &ToggledDays() {
	static auto result = base::flat_set<QDate>();
	return result;
}

[[nodiscard]] bool DayCollapsed(QDate day) {
	const auto past = (day < QDate::currentDate());
	return past != ToggledDays().contains(day);
}

// A meeting in a bubble of an inbox message: the subject, the time and the
// place under it, the people. A click shows it in Outlook, a click on a
// member shows the profile.
class MeetingRow final
	: public Ui::AbstractButton
	, public Ui::AbstractTooltipShower {
public:
	MeetingRow(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		OutlookMeeting meeting)
	: AbstractButton(parent)
	, _controller(controller)
	, _meeting(std::move(meeting))
	, _link(MeetingLink(_meeting))
	, _info(MeetingInfo(_meeting))
	, _people(PeopleLines(&controller->session(), _meeting))
	, _tooltip(PeopleTooltip(_meeting)) {
		resize(width(), rowHeight());
		setPointerCursor(!_meeting.entryId.isEmpty());
		setAttribute(Qt::WA_Hover);
		setClickedCallback([=] {
			if (const auto user = _overLink) {
				LOG(("MtsLink Outlook: the profile of a participant"));
				_controller->showPeerInfo(user);
			} else {
				openOutlookMeeting(_meeting);
			}
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

	~MeetingRow() {
		// The tooltip keeps the pointer: not to a destroyed row (the list
		// is made again by a refresh under the cursor).
		if (_tooltipShown) {
			LOG(("MtsLink Outlook: the tooltip of a destroyed row hidden"));
			Ui::Tooltip::Hide();
		}
	}

	[[nodiscard]] QString key() const {
		return MeetingKey(_meeting);
	}

	// The meeting of a clicked reminder.
	void flash() {
		_flash.start([=] { update(); }, 1., 0., kFlashDuration);
	}

	QString tooltipText() const override {
		return _overLink ? QString() : _tooltip;
	}
	QPoint tooltipPos() const override {
		return QCursor::pos();
	}
	bool tooltipWindowActive() const override {
		return Ui::AppInFocus() && Ui::InFocusChain(window());
	}

protected:
	int resizeGetHeight(int newWidth) override {
		if (_call) {
			const auto bubble = bubbleRect(newWidth, rowHeight());
			_call->moveToLeft(
				(CallLeft()
					? (bubble.x() + kCallMargin)
					: (bubble.x()
						+ bubble.width()
						- kCallMargin
						- _call->width())),
				(rowHeight() - _call->height()) / 2,
				newWidth);
		}
		return rowHeight();
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		AbstractButton::mouseMoveEvent(e);
		auto link = (UserData*)nullptr;
		for (const auto &[rect, user] : _links) {
			if (rect.contains(e->pos())) {
				link = user;
				break;
			}
		}
		if (_overLink != link) {
			_overLink = link;
			setCursor((link || !_meeting.entryId.isEmpty())
				? style::cur_pointer
				: style::cur_default);
			update();
		}
		if (!_tooltip.isEmpty()) {
			_tooltipShown = true;
			Ui::Tooltip::Show(1000, this);
		}
	}

	void leaveEventHook(QEvent *e) override {
		_tooltipShown = false;
		Ui::Tooltip::Hide();
		if (_overLink) {
			_overLink = nullptr;
			update();
		}
		AbstractButton::leaveEventHook(e);
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto bubble = QRectF(bubbleRect(width(), height()));
		p.setPen(Qt::NoPen);
		p.setBrush(st::msgInShadow);
		p.drawRoundedRect(
			bubble.translated(0, st::msgShadow),
			kBubbleRadius,
			kBubbleRadius);
		const auto now = QDateTime::currentDateTime();
		const auto end = _meeting.end.isValid()
			? _meeting.end
			: _meeting.start;
		const auto finished = (end <= now);
		const auto current = !finished && (_meeting.start <= now);
		const auto upcoming = !finished
			&& !current
			&& !_meeting.allDay
			&& (now.secsTo(_meeting.start) <= UpcomingSeconds());
		const auto over = isOver() && !_meeting.entryId.isEmpty();
		const auto accent = RemindAccent();
		auto path = QPainterPath();
		path.addRoundedRect(bubble, kBubbleRadius, kBubbleRadius);
		p.setBrush(over
			? st::msgInBgSelected->c
			: upcoming
			? anim::color(st::msgInBg->c, accent, 0.18)
			: st::msgInBg->c);
		p.drawPath(path);
		if (upcoming) {
			// Starts soon: the stripe of the color of the reminders.
			p.save();
			p.setClipPath(path);
			p.fillRect(
				QRectF(bubble.x(), bubble.y(), 4., bubble.height()),
				accent);
			p.restore();
		}
		if (_flash.animating()) {
			p.setOpacity(_flash.value(0.) * 0.45);
			p.setBrush(accent);
			p.drawPath(path);
			p.setOpacity(1.);
		}
		if (_call) {
			// The call button on a filled circle of the accent color.
			p.setBrush(_call->isOver()
				? st::activeButtonBgOver
				: st::activeButtonBg);
			p.drawEllipse(_call->geometry());
		}

		const auto callSpace = this->callSpace();
		const auto left = int(bubble.x())
			+ kRowPadding
			+ (CallLeft() ? callSpace : 0);
		const auto available = std::max(
			int(bubble.x() + bubble.width())
				- kRowPadding
				- (CallLeft() ? 0 : callSpace)
				- left,
			0);
		const auto &titleFont = st::semiboldFont;
		const auto &infoFont = st::normalFont;
		const auto top = (height()
			- titleFont->height
			- infoFont->height
			- 2
			- int(_people.size()) * (infoFont->height + 2)) / 2;
		p.setFont(titleFont);
		// Under the cursor: the colors of a selected message.
		p.setPen(over
			? st::historyTextInFgSelected->c
			: finished
			? st::msgInDateFg->c
			: st::historyTextInFg->c);
		p.drawText(
			left,
			top + titleFont->ascent,
			titleFont->elided(MeetingSubject(_meeting), available));

		p.setFont(infoFont);
		p.setPen(over
			? (current
				? st::msgInServiceFgSelected->c
				: st::msgInDateFgSelected->c)
			: current
			? st::windowActiveTextFg->c
			: upcoming
			? accent
			: st::msgInDateFg->c);
		p.drawText(
			left,
			top + titleFont->height + 2 + infoFont->ascent,
			infoFont->elided(_info, available));

		// The people: the members as the links.
		_links.clear();
		const auto textFg = over
			? st::msgInDateFgSelected->c
			: st::msgInDateFg->c;
		const auto linkFg = over
			? st::historyLinkInFgSelected->c
			: st::historyLinkInFg->c;
		const auto right = left + available;
		auto y = top + titleFont->height + 2 + infoFont->height + 2;
		for (const auto &line : _people) {
			auto x = left;
			for (const auto &segment : line) {
				const auto &font = (segment.user && segment.user == _overLink)
					? infoFont->underline()
					: infoFont;
				const auto width = font->width(segment.text);
				const auto fits = (x + width <= right);
				const auto shown = fits ? width : std::max(right - x, 0);
				p.setFont(font);
				p.setPen(segment.user ? linkFg : textFg);
				p.drawText(
					x,
					y + infoFont->ascent,
					fits ? segment.text : font->elided(segment.text, shown));
				if (segment.user) {
					_links.push_back({
						QRect(x, y, shown, infoFont->height),
						segment.user,
					});
				}
				if (!fits) {
					break;
				}
				x += width;
			}
			y += infoFont->height + 2;
		}
	}

private:
	// The text: kCallGap from the call button (instead of kRowPadding
	// from the border of the bubble).
	[[nodiscard]] int callSpace() const {
		return _call
			? (kCallMargin + _call->width() + kCallGap - kRowPadding)
			: 0;
	}

	// On the whole width, else as a message: not narrower than a message,
	// wider by the text (till the whole width).
	[[nodiscard]] QRect bubbleRect(int width, int height) const {
		const auto full = width - 2 * kBubbleSide;
		auto result = full;
		if (!FullWidth()) {
			auto text = std::max(
				st::semiboldFont->width(MeetingSubject(_meeting)),
				st::normalFont->width(_info));
			for (const auto &line : _people) {
				auto lineWidth = 0;
				for (const auto &segment : line) {
					lineWidth += st::normalFont->width(segment.text);
				}
				text = std::max(text, lineWidth);
			}
			const auto natural = 2 * kRowPadding + callSpace() + text;
			result = std::min(full, std::max(natural, int(st::msgMaxWidth)));
		}
		return QRect(
			kBubbleSide,
			kBubbleVertical,
			std::max(result, 0),
			height - 2 * kBubbleVertical);
	}

	[[nodiscard]] int rowHeight() const {
		return kRowHeight
			+ int(_people.size()) * (st::normalFont->height + 2);
	}

	const not_null<Window::SessionController*> _controller;
	const OutlookMeeting _meeting;
	const QString _link;
	const QString _info;
	const std::vector<PeopleLine> _people;
	const QString _tooltip;
	mutable std::vector<std::pair<QRect, UserData*>> _links;
	UserData *_overLink = nullptr;
	bool _tooltipShown = false;
	Ui::IconButton *_call = nullptr;
	Ui::Animations::Simple _flash;

};

} // namespace

struct MeetingsWidget::State {
	// The list of a period shown before: kept (out of the scroll), shown
	// again at once (made again only if its meetings changed).
	struct Page {
		object_ptr<Ui::VerticalLayout> content = { nullptr };
		std::optional<std::vector<OutlookMeeting>> shown;
		QDate shownFrom;
		QString emptyText;
		std::vector<QPointer<MeetingRow>> rows;
		int scrollTop = 0;
	};
	base::flat_map<int, Page> pages;

	int index = 0;
	QString emptyText; // Shown in the middle, no meetings shown.
	// The shown ones: not made again by a refresh without the changes.
	std::optional<std::vector<OutlookMeeting>> shown;
	QDate shownFrom;
	Ui::ScrollArea *scroll = nullptr;
	Ui::VerticalLayout *content = nullptr;
	base::unique_qptr<Ui::PopupMenu> menu;
	std::vector<QPointer<MeetingRow>> rows;
	QPointer<MeetingRow> scrollTo;
	// The finished, the current, the starting soon ones change.
	base::Timer repaintTimer;
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

	LayoutChanges().events(
	) | rpl::on_next([=] {
		_state->pages.clear();
		_state->shown = std::nullopt;
		rebuild();
	}, lifetime());

	_state->repaintTimer.setCallback([=] {
		_state->content->update();
	});
	_state->repaintTimer.callEach(30 * 1000);

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
	{
		auto &page = _state->pages[_state->index];
		page.scrollTop = _state->scroll->scrollTop();
		page.content = _state->scroll->takeWidget<Ui::VerticalLayout>();
		if (page.content) {
			page.content->hide();
		}
		page.shown = base::take(_state->shown);
		page.shownFrom = _state->shownFrom;
		page.emptyText = _state->emptyText;
		page.rows = base::take(_state->rows);
	}
	_state->index = index;
	auto scrollTop = 0;
	const auto i = _state->pages.find(index);
	if (i != end(_state->pages) && i->second.content) {
		auto page = std::move(i->second);
		_state->pages.erase(i);
		_state->content = _state->scroll->setOwnedWidget(
			std::move(page.content)).data();
		_state->shown = std::move(page.shown);
		_state->shownFrom = page.shownFrom;
		_state->emptyText = page.emptyText;
		_state->rows = std::move(page.rows);
		scrollTop = page.scrollTop;
	} else {
		_state->content = _state->scroll->setOwnedWidget(
			object_ptr<Ui::VerticalLayout>(_state->scroll)).data();
		_state->shown = std::nullopt;
		_state->emptyText = QString();
		_state->rows.clear();
	}
	_state->content->show();
	_state->content->resizeToWidth(_state->scroll->width());
	rebuild();
	_state->scroll->scrollToY(scrollTop);
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
	const auto meetings = VisibleMeetings(_state->index);
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
		_state->scroll->setVisible(_state->emptyText.isEmpty());
		_state->content->update();
		update();
		showPendingMeeting();
		return;
	}
	_state->shown = meetings;
	_state->shownFrom = from;
	_state->emptyText = emptyText;

	const auto content = _state->content;
	const auto scrollTop = _state->scroll->scrollTop();
	_state->rows.clear();
	while (content->count() > 0) {
		delete content->widgetAt(0);
	}
	const auto empty = !_state->emptyText.isEmpty();
	_state->scroll->setVisible(!empty);
	if (!empty) {
		const auto chatStyle = controller()->chatStyle();
		const auto pillText = [](QDate day, bool collapsed) {
			return langDayOfWeekFull(day)
				+ u", "_q
				+ langDayOfMonthFull(day)
				+ QChar(' ')
				+ (collapsed ? QChar(0x25B8) : QChar(0x25BE));
		};
		const auto state = _state.get();
		const auto window = controller();
		const auto fill = [=](
				not_null<Ui::VerticalLayout*> inner,
				const std::vector<OutlookMeeting> &list) {
			for (const auto &meeting : list) {
				state->rows.push_back(inner->add(
					object_ptr<MeetingRow>(inner, window, meeting)));
			}
		};
		using Day = std::pair<QDate, std::vector<OutlookMeeting>>;
		auto days = std::vector<Day>();
		for (const auto &meeting : meetings) {
			const auto date = RowDate(meeting, _state->index);
			if (days.empty() || days.back().first != date) {
				days.emplace_back(date, std::vector<OutlookMeeting>());
			}
			days.back().second.push_back(meeting);
		}
		const auto today = QDate::currentDate();
		for (auto &entry : days) {
			const auto day = entry.first;
			const auto collapsed = DayCollapsed(day);
			const auto pill = content->add(object_ptr<DayPill>(
				content,
				chatStyle,
				pillText(day, collapsed),
				day < today,
				day == today));
			const auto wrap = content->add(
				object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
					content,
					object_ptr<Ui::VerticalLayout>(content)));
			const auto inner = wrap->entity();
			// The rows of a collapsed day: made on its first expand.
			const auto pending = std::make_shared<
				std::vector<OutlookMeeting>>();
			if (collapsed) {
				*pending = std::move(entry.second);
			} else {
				fill(inner, entry.second);
			}
			wrap->toggle(!collapsed, anim::type::instant);
			pill->setClickedCallback([=] {
				auto &set = ToggledDays();
				if (!set.remove(day)) {
					set.emplace(day);
				}
				const auto now = DayCollapsed(day);
				if (!now && !pending->empty()) {
					fill(inner, *pending);
					pending->clear();
					inner->resizeToWidth(content->width());
				}
				pill->setText(pillText(day, now));
				wrap->toggle(!now, anim::type::normal);
			});
		}
		Ui::AddSkip(content);
		content->resizeToWidth(_state->scroll->width());
		_state->scroll->scrollToY(scrollTop);
	}
	update();
	showPendingMeeting();
}

void MeetingsWidget::showPendingMeeting() {
	const auto key = PendingMeeting();
	if (key.isEmpty() || !_state->emptyText.isEmpty()) {
		if (MeetingsData().loaded && !MeetingsData().loading) {
			PendingMeeting() = QString();
		}
		return;
	}
	const auto meetings = VisibleMeetings(_state->index);
	const auto i = ranges::find(meetings, key, MeetingKey);
	if (i == end(meetings)) {
		LOG(("MtsLink Outlook: the meeting of the reminder not found"));
		PendingMeeting() = QString();
		return;
	}
	const auto date = RowDate(*i, _state->index);
	if (DayCollapsed(date)) {
		// Expanded: all again (here again from the end of it).
		auto &set = ToggledDays();
		if (!set.remove(date)) {
			set.emplace(date);
		}
		_state->shown = std::nullopt;
		rebuild();
		return;
	}
	PendingMeeting() = QString();
	for (const auto &row : _state->rows) {
		if (row && row->key() == key) {
			row->flash();
			_state->scrollTo = row;
			scrollToPendingRow();
			break;
		}
	}
}

void MeetingsWidget::scrollToPendingRow() {
	const auto row = _state->scrollTo.data();
	if (!row || _state->scroll->height() <= 0) {
		return;
	}
	_state->scrollTo = nullptr;
	const auto y = row->mapTo(_state->content, QPoint()).y();
	_state->scroll->scrollToY(
		y - (_state->scroll->height() - row->height()) / 2);
}

void MeetingsWidget::updateControlsGeometry() {
	const auto top = st::topBarHeight;
	_state->scroll->setGeometry(
		0,
		top,
		width(),
		std::max(height() - top, 0));
	_state->content->resizeToWidth(width());
	scrollToPendingRow();
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
		showPendingMeeting();
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
			|| (RefreshSeconds().current() <= 0
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

int meetingsRefreshSeconds() {
	return RefreshSeconds().current();
}

std::vector<int> meetingsRefreshOptions() {
	return { 0, 10, 30, 60, 120, 300, 600 };
}

QString meetingsRefreshName(int seconds) {
	return (seconds <= 0)
		? tr::lng_mtslink_meetings_refresh_manual(tr::now)
		: (seconds < 60)
		? tr::lng_mtslink_meetings_refresh_seconds(
			tr::now,
			lt_count,
			seconds)
		: (seconds == 60)
		? tr::lng_mtslink_meetings_refresh_minute(tr::now)
		: tr::lng_mtslink_meetings_refresh_minutes(
			tr::now,
			lt_count,
			seconds / 60);
}

bool meetingsFullWidth() {
	return FullWidth();
}

void setMeetingsFullWidth(bool full) {
	if (FullWidth() == full) {
		return;
	}
	FullWidth() = full;
	auto settings = readLocalSettings();
	settings.insert(QString::fromLatin1(kFullWidthKey), full);
	writeLocalSettings(settings);
	LOG(("MtsLink Outlook: the rows %1").arg(full ? "full" : "as messages"));
	LayoutChanges().fire({});
}

int meetingsPeriodsCount(MeetingsPeriod period) {
	return PeriodsCounts()[period];
}

std::vector<int> meetingsPeriodsCountOptions(MeetingsPeriod period) {
	auto result = std::vector<int>();
	for (auto i = 1, till = MaxPeriodsCount(period); i <= till; ++i) {
		result.push_back(i);
	}
	return result;
}

void setMeetingsPeriodsCount(
		not_null<Main::Session*> session,
		MeetingsPeriod period,
		int count) {
	count = std::clamp(count, 1, MaxPeriodsCount(period));
	if (PeriodsCounts()[period] == count) {
		return;
	}
	PeriodsCounts()[period] = count;
	auto saved = QJsonObject();
	for (const auto &[key, value] : PeriodsCounts()) {
		saved.insert(PeriodKey(key), value);
	}
	auto settings = readLocalSettings();
	settings.insert(QString::fromLatin1(kPeriodsCountsKey), saved);
	writeLocalSettings(settings);
	LOG(("MtsLink Outlook: %1 periods of %2"
		).arg(count
		).arg(PeriodKey(period)));
	if (period == CurrentPeriod()) {
		// The rows: shown in the folder again (or hidden), the meetings.
		refreshMeetingPeers(session);
		MeetingsData().changes.fire({});
	}
}

bool meetingsPeopleAsMentions() {
	return PeopleAsMentions();
}

void setMeetingsPeopleAsMentions(bool mentions) {
	if (PeopleAsMentions() == mentions) {
		return;
	}
	PeopleAsMentions() = mentions;
	auto settings = readLocalSettings();
	settings.insert(QString::fromLatin1(kPeopleMentionsKey), mentions);
	writeLocalSettings(settings);
	LOG(("MtsLink Outlook: the participants as %1"
		).arg(mentions ? "mentions" : "names"));
	LayoutChanges().fire({});
}

bool meetingsCallLeft() {
	return CallLeft();
}

void setMeetingsCallLeft(bool left) {
	if (CallLeft() == left) {
		return;
	}
	CallLeft() = left;
	auto settings = readLocalSettings();
	settings.insert(QString::fromLatin1(kCallLeftKey), left);
	writeLocalSettings(settings);
	LOG(("MtsLink Outlook: the call button %1").arg(left ? "left" : "right"));
	LayoutChanges().fire({});
}

std::vector<int> meetingsRemindOptions() {
	return { 30, 15, 5, 1 };
}

QString meetingsRemindName(int minutes) {
	return tr::lng_mtslink_meetings_remind_minutes(
		tr::now,
		lt_count,
		minutes);
}

bool meetingsRemindEnabled(int minutes) {
	return RemindMinutes().contains(minutes);
}

void setMeetingsRemindEnabled(int minutes, bool enabled) {
	auto &list = RemindMinutes();
	if (enabled) {
		list.emplace(minutes);
	} else {
		list.remove(minutes);
	}
	auto array = QJsonArray();
	for (const auto value : list) {
		array.append(value);
	}
	auto settings = readLocalSettings();
	settings.insert(QString::fromLatin1(kRemindKey), array);
	writeLocalSettings(settings);
	LOG(("MtsLink Outlook: the reminder %1 minutes before: %2"
		).arg(minutes
		).arg(Logs::b(enabled)));
}

std::vector<QString> meetingsRemindSounds() {
	return {
		u"msg_incoming"_q,
		u"call_incoming"_q,
		u"group_call_start"_q,
		u"group_call_allowed"_q,
		u"call_connect"_q,
		u"none"_q,
	};
}

QString meetingsRemindSound() {
	return RemindSound();
}

QString meetingsRemindSoundName(const QString &key) {
	if (key.startsWith(u"file:"_q)) {
		return tr::lng_mtslink_meetings_remind_sound_file(
			tr::now,
			lt_name,
			QFileInfo(key.mid(5)).fileName());
	} else if (key == u"call_incoming"_q) {
		return tr::lng_mtslink_meetings_remind_sound_call(tr::now);
	} else if (key == u"group_call_start"_q) {
		return tr::lng_mtslink_meetings_remind_sound_start(tr::now);
	} else if (key == u"group_call_allowed"_q) {
		return tr::lng_mtslink_meetings_remind_sound_signal(tr::now);
	} else if (key == u"call_connect"_q) {
		return tr::lng_mtslink_meetings_remind_sound_connect(tr::now);
	} else if (key == u"none"_q) {
		return tr::lng_mtslink_meetings_remind_sound_none(tr::now);
	}
	return tr::lng_mtslink_meetings_remind_sound_default(tr::now);
}

QString meetingsRemindSoundPath(const QString &key) {
	if (key == u"none"_q) {
		return QString();
	} else if (key.startsWith(u"file:"_q)) {
		const auto path = key.mid(5);
		if (QFileInfo::exists(path)) {
			return path;
		}
		// The file is removed: as for the messages.
		return Core::App().settings().getSoundPath(
			QString::fromLatin1(kDefaultRemindSound));
	} else if (!ranges::contains(meetingsRemindSounds(), key)) {
		return Core::App().settings().getSoundPath(
			QString::fromLatin1(kDefaultRemindSound));
	}
	return Core::App().settings().getSoundPath(key);
}

void setMeetingsRemindSound(const QString &key) {
	RemindSound() = key;
	auto settings = readLocalSettings();
	settings.insert(QString::fromLatin1(kRemindSoundKey), key);
	writeLocalSettings(settings);
	LOG(("MtsLink Outlook: the reminder sound %1").arg(key));
}

std::optional<QColor> meetingsRemindColor() {
	return RemindColor();
}

void setMeetingsRemindColor(std::optional<QColor> color) {
	RemindColor() = color;
	auto settings = readLocalSettings();
	settings.insert(
		QString::fromLatin1(kRemindColorKey),
		color ? color->name(QColor::HexRgb) : QString());
	writeLocalSettings(settings);
	LOG(("MtsLink Outlook: the reminder color %1"
		).arg(color ? color->name(QColor::HexRgb) : u"theme"_q));
}

bool meetingsShowPast() {
	return ShowPast();
}

void setMeetingsShowPast(bool show) {
	if (ShowPast() == show) {
		return;
	}
	ShowPast() = show;
	auto settings = readLocalSettings();
	settings.insert(QString::fromLatin1(kShowPastKey), show);
	writeLocalSettings(settings);
	LOG(("MtsLink Outlook: the past days %1").arg(show ? "shown" : "hidden"));
	MeetingsData().changes.fire({});
}

bool meetingsShowWeekends() {
	return ShowWeekends();
}

void setMeetingsShowWeekends(
		not_null<Main::Session*> session,
		bool show) {
	if (ShowWeekends() == show) {
		return;
	}
	ShowWeekends() = show;
	auto settings = readLocalSettings();
	settings.insert(QString::fromLatin1(kShowWeekendsKey), show);
	writeLocalSettings(settings);
	LOG(("MtsLink Outlook: the weekends %1").arg(show ? "shown" : "hidden"));
	// The days of the rows are others: the meetings of them again.
	refreshMeetingPeers(session);
	MeetingsData().changes.fire({});
}

void meetingReminderActivated(MsgId id) {
	const auto i = ReminderKeys().find(id);
	if (i == ReminderKeys().end()) {
		return;
	}
	LOG(("MtsLink Outlook: the reminder %1 clicked").arg(id.bare));
	PendingMeeting() = i->second;
}

void setMeetingsRefreshSeconds(int seconds) {
	seconds = std::clamp(seconds, 0, kMaxRefreshSeconds);
	if (RefreshSeconds().current() == seconds) {
		return;
	}
	auto settings = readLocalSettings();
	settings.insert(QString::fromLatin1(kRefreshSecondsKey), seconds);
	writeLocalSettings(settings);
	LOG(("MtsLink Outlook: the refresh each %1 seconds").arg(seconds));
	RefreshSeconds() = seconds;
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

namespace {

// The background work of a session (the reminders, the refresh of the
// meetings from Outlook): only while the integration is on.
[[nodiscard]] auto BackgroundLifetimes()
-> base::flat_map<not_null<Main::Session*>, std::unique_ptr<rpl::lifetime>> & {
	static auto result = base::flat_map<
		not_null<Main::Session*>,
		std::unique_ptr<rpl::lifetime>>();
	return result;
}

void StopBackground(not_null<Main::Session*> session) {
	auto &map = BackgroundLifetimes();
	const auto i = map.find(session);
	if (i == map.end()) {
		return;
	}
	auto lifetime = std::move(i->second);
	map.erase(i);
	lifetime->destroy();
	LOG(("MtsLink Outlook: the background work stopped "
		"(no reminders, no refresh)"));
}

void ClearMeetingsData() {
	auto &data = MeetingsData();
	data.loaded = false;
	data.list = std::nullopt;
	data.loadedAt = 0;
	data.from = QDate();
	data.till = QDate();
}

} // namespace

void initMeetingPeers(not_null<Main::Session*> session) {
	if (!outlookMeetingsEnabled()) {
		// The setting is off: no folder (one of the earlier version).
		LOG(("MtsLink Outlook: the integration is off, "
			"no background work"));
		StopBackground(session);
		RemoveMeetingsFolders(session);
		return;
	}
	auto &map = BackgroundLifetimes();
	if (map.contains(session)) {
		// A reconnect: the meetings again.
		refreshMeetingPeers(session);
		return;
	}
	const auto lifetime = map.emplace(
		session,
		std::make_unique<rpl::lifetime>()).first->second.get();
	session->lifetime().add([=] {
		BackgroundLifetimes().remove(session);
	});
	LOG(("MtsLink Outlook: the background work started"));
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
		CheckReminders(session);
	}, *lifetime);

	// The reminders: right after the start of each minute (the meetings
	// start at the round minutes). Only the loaded list, no Outlook here:
	// the list is loaded by the refresh timer below.
	const auto remindTimer = lifetime->make_state<base::Timer>();
	const auto remindNext = [=] {
		const auto passed = QTime::currentTime().msecsSinceStartOfDay()
			% (60 * 1000);
		remindTimer->callOnce(60 * 1000 - passed + 1000, Qt::PreciseTimer);
	};
	remindTimer->setCallback([=] {
		CheckReminders(session);
		remindNext();
	});
	remindNext();

	const auto timer = lifetime->make_state<base::Timer>([=] {
		refreshMeetingPeers(session);
	});
	RefreshSeconds().value(
	) | rpl::on_next([=](int seconds) {
		timer->cancel();
		if (seconds > 0) {
			timer->callEach(crl::time(seconds) * 1000);
		}
	}, *lifetime);

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
		StopBackground(session);
		ClearMeetingsData();
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

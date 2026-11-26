/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.
*/
#pragma once

namespace Main {
class Session;
} // namespace Main

namespace MtsLink {

// The periods of the Outlook calendar ("This week 1-7 March", ...) as the
// chats, as the sections of the tasks: only in the folders (of the
// "Outlook meetings" type), Windows only. A click shows the meetings.

// The length of the periods (a setting, the week by default).
enum class MeetingsPeriod {
	Day,
	Week,
	Month,
	Quarter,
};
[[nodiscard]] MeetingsPeriod meetingsPeriod();
[[nodiscard]] QString meetingsPeriodName(MeetingsPeriod period);
[[nodiscard]] std::vector<MeetingsPeriod> meetingsPeriods();

// The number of the periods in the chats list, for each length (a setting:
// 7 days, 4 weeks, 3 months, 2 quarters by default).
[[nodiscard]] int meetingsPeriodsCount(MeetingsPeriod period);
[[nodiscard]] std::vector<int> meetingsPeriodsCountOptions(
	MeetingsPeriod period);
void setMeetingsPeriodsCount(
	not_null<Main::Session*> session,
	MeetingsPeriod period,
	int count);

// Any of the rows (shown or not by the period).
[[nodiscard]] bool isMeetingPeer(PeerId peerId);
// The setting is on (the periods are shown in their folder).
[[nodiscard]] bool meetingsEnabled();
// The period of the row: 0 is the current one, 1 the next one, ...;
// std::nullopt for a row not shown with the current length of the periods.
[[nodiscard]] std::optional<int> meetingIndexForPeer(PeerId peerId);
[[nodiscard]] PeerId meetingPeerId(int index);
[[nodiscard]] std::vector<PeerId> meetingPeerIds();

// After the session start: the rows, the meetings (if the setting is on).
void initMeetingPeers(not_null<Main::Session*> session);
// The setting turned on (after the test connection): the folder, the rows;
// turned off: the folder is removed (the rows with it).
void setMeetingsEnabled(not_null<Main::Session*> session, bool enabled);
// The meetings again (the names of the periods too).
void refreshMeetingPeers(not_null<Main::Session*> session);
// The background refresh of the meetings: each N seconds, 0 is manual
// only (the "Refresh" of the menu). A setting, each minute by default.
[[nodiscard]] int meetingsRefreshSeconds();
[[nodiscard]] std::vector<int> meetingsRefreshOptions();
[[nodiscard]] QString meetingsRefreshName(int seconds);
void setMeetingsRefreshSeconds(int seconds);

// The rows of the meetings: on the whole width of the window, else not
// wider than a message (a setting, the whole width by default).
[[nodiscard]] bool meetingsFullWidth();
void setMeetingsFullWidth(bool full);
// The participants found in MTS Link: as the mentions (@name), else as the
// names in the order of the chats settings (a setting, the names by
// default).
[[nodiscard]] bool meetingsPeopleAsMentions();
void setMeetingsPeopleAsMentions(bool mentions);
// The call button of a meeting at the left border (else at the right).
[[nodiscard]] bool meetingsCallLeft();
void setMeetingsCallLeft(bool left);

// The reminders before the meetings: any of 30, 15, 5 and 1 minutes
// (a setting, 5 by default). Checked each minute, shown as notifications
// (not of a message), a click shows the meetings of the period.
[[nodiscard]] std::vector<int> meetingsRemindOptions();
[[nodiscard]] QString meetingsRemindName(int minutes);
[[nodiscard]] bool meetingsRemindEnabled(int minutes);
void setMeetingsRemindEnabled(int minutes, bool enabled);

// The sound of the reminders: a built-in one ("msg_incoming" by default,
// as for the messages), "none" or "file:" + the path of a file.
[[nodiscard]] std::vector<QString> meetingsRemindSounds();
[[nodiscard]] QString meetingsRemindSound();
[[nodiscard]] QString meetingsRemindSoundName(const QString &key);
// The file to play, empty for "none".
[[nodiscard]] QString meetingsRemindSoundPath(const QString &key);
void setMeetingsRemindSound(const QString &key);

// The color of the reminders: the background of the notification, the
// meetings starting soon in the list. std::nullopt: of the theme.
[[nodiscard]] std::optional<QColor> meetingsRemindColor();
void setMeetingsRemindColor(std::optional<QColor> color);

// The past days in the lists of the meetings (a setting, on by default);
// the finished meetings of today are shown anyway.
[[nodiscard]] bool meetingsShowPast();
void setMeetingsShowPast(bool show);

// The weekends in the lists of the meetings and as the days in the chats
// list (a setting, on by default).
[[nodiscard]] bool meetingsShowWeekends();
void setMeetingsShowWeekends(
	not_null<Main::Session*> session,
	bool show);

// A click on a reminder (of the period peer): its meeting is shown in the
// list of the period (the day expanded, the meeting highlighted).
void meetingReminderActivated(MsgId id);

// Another length of the periods: the rows and the meetings again.
void setMeetingsPeriod(
	not_null<Main::Session*> session,
	MeetingsPeriod period);

} // namespace MtsLink

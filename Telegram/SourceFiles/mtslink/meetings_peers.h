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
// The background refresh of the meetings: each N minutes, 0 is manual
// only (the "Refresh" of the menu). A setting, each minute by default.
[[nodiscard]] int meetingsRefreshMinutes();
[[nodiscard]] std::vector<int> meetingsRefreshOptions();
[[nodiscard]] QString meetingsRefreshName(int minutes);
void setMeetingsRefreshMinutes(int minutes);

// Another length of the periods: the rows and the meetings again.
void setMeetingsPeriod(
	not_null<Main::Session*> session,
	MeetingsPeriod period);

} // namespace MtsLink

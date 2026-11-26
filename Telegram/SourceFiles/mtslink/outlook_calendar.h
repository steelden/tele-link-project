/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.

The meetings of the default calendar of Outlook (Windows, COM).
*/
#pragma once

namespace MtsLink {

// A participant: the name as in Outlook, the email if resolved there.
struct OutlookPerson {
	QString name;
	QString email;

	friend inline bool operator==(
		const OutlookPerson &,
		const OutlookPerson &) = default;
};

struct OutlookMeeting {
	QString subject;
	QString location;
	QDateTime start;
	QDateTime end;
	QString entryId; // Of the item (of the series for an occurrence).
	bool recurring = false;
	bool allDay = false;
	OutlookPerson organizer;
	std::vector<OutlookPerson> required;
	std::vector<OutlookPerson> optional;
	int response = 0; // OlResponseStatus: of the user of Outlook.

	friend inline bool operator==(
		const OutlookMeeting &,
		const OutlookMeeting &) = default;
};

// Outlook can be asked on this platform at all (Windows only).
[[nodiscard]] bool outlookSupported();

// The setting (off by default): the meetings of Outlook are shown. It is
// turned on only after a successful test connection.
[[nodiscard]] bool outlookMeetingsEnabled();
void setOutlookMeetingsEnabled(bool enabled);

// The test connection: the meetings of today are read (Outlook is started
// if needed). done(true): the connection works. Called on the main thread.
void testOutlookConnection(Fn<void(bool)> done);

// The COM registration of Outlook is checked once, in the background
// (nothing is shown, whatever the result).
void checkOutlookAvailable(Fn<void()> checked = nullptr);
// Known after the check: Outlook is installed (registered for COM).
[[nodiscard]] bool outlookAvailable();

// The meetings of the default calendar in [from, till), sorted by the
// start; std::nullopt: no connection to Outlook (not installed, not
// configured, busy, any error). Called on the main thread.
void loadOutlookMeetings(
	QDateTime from,
	QDateTime till,
	Fn<void(std::optional<std::vector<OutlookMeeting>>)> done);

// The meeting is shown in Outlook (the occurrence of a recurring one, else
// the series), in the background; nothing is shown on a failure.
void openOutlookMeeting(const OutlookMeeting &meeting);

} // namespace MtsLink

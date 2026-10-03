/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.

MTS Link video conferences in an embedded browser window.
*/
#pragma once

namespace MtsLink {

// Opens the conference link in a TeleLink window with an embedded browser,
// an already opened window for the link is just activated. Falls back to
// the external browser if no embedded browser is available.
enum class CallBrowser {
	Default, // As chosen in the settings.
	Embedded,
	System,
};
// video = false: the page can't turn the camera on at the start.
void openCallLink(
	const QString &url,
	const QString &title = QString(),
	CallBrowser browser = CallBrowser::Default,
	bool video = true);

// Joining somebody's conference: the link from the call message is the
// creator's one, a personal link is generated for the event first.
void rememberCallEvent(const QString &joinLink, const QString &eventId);
void joinCallLink(
	const QString &joinLink,
	const QString &title = QString(),
	CallBrowser browser = CallBrowser::Default,
	bool video = true);

// Opening the conferences in the embedded browser or the system one.
[[nodiscard]] bool callsInEmbeddedBrowser();
void setCallsInEmbeddedBrowser(bool embedded);

} // namespace MtsLink

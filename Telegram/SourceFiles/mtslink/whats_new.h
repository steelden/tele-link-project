/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.
*/
#pragma once

namespace MtsLink {

// "What's new" after an update: the markdown of the version from the
// repository (WHATSNEW.md by the tag v<version>), shown once in the
// markdown viewer.

// At the start: the previous version of the settings (0 - a new install).
void rememberWhatsNewAfterUpdate(int oldVersion);

// After the connection: downloaded and shown, if the update is not shown.
void showWhatsNewIfPending();

// By the version link in the main menu: always shown, fail(text) if not.
void showWhatsNew(Fn<void(QString)> fail);

} // namespace MtsLink

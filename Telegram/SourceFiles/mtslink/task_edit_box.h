/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.
*/
#pragma once

#include "mtslink/api/api_tasks.h"

class HistoryItem;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace MtsLink {

// The card of a task: a new one (no task) with the fields given, or an
// existing one (its fields), edited / resolved / removed.
void showTaskEditBox(
	not_null<Window::SessionController*> controller,
	std::optional<Api::Task> task,
	Api::TaskFields defaults = {});

[[nodiscard]] QString taskDeadlineText(const QDateTime &deadline);

// "Create task" of a message: its text as the description, assigned to me,
// the link to the message (as the web client does), the title to enter.
void showCreateTaskFromMessage(
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item,
	bool inRepliesContext);

// "Create task" in the middle of the message menu: after "Forward" (or
// "Reply"), the middle if neither is there.
void addCreateTaskMenuAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item,
	bool inRepliesContext);

// The removal confirmed, then done (closes what shows the task).
void confirmTaskRemove(
	not_null<Window::SessionController*> controller,
	const QString &id,
	Fn<void()> done);

} // namespace MtsLink

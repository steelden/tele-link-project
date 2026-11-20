/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.
*/
#pragma once

#include "window/section_widget.h"
#include "window/section_memento.h"
#include "mtslink/api/api_tasks.h"

namespace Ui {
class ScrollArea;
class VerticalLayout;
class SettingsSlider;
class FlatLabel;
class IconButton;
} // namespace Ui

namespace MtsLink {

// The tasks of the task tracker in the main column: the sections of the
// web client as the tabs, the tasks grouped by the deadline.
class TasksMemento final : public Window::SectionMemento {
public:
	explicit TasksMemento(Api::TaskSection section = Api::TaskSection::My)
	: _section(section) {
	}

	object_ptr<Window::SectionWidget> createWidget(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		Window::Column column,
		const QRect &geometry) override;

	[[nodiscard]] Api::TaskSection section() const {
		return _section;
	}

private:
	Api::TaskSection _section = Api::TaskSection::My;

};

class TasksWidget final : public Window::SectionWidget {
public:
	TasksWidget(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		Api::TaskSection section);
	~TasksWidget();

	bool hasTopBarShadow() const override {
		return true;
	}
	// The row of the section in the chats list is the active one.
	Dialogs::RowDescriptor activeChat() const override;
	bool showInternal(
		not_null<Window::SectionMemento*> memento,
		const Window::SectionShow &params) override;
	std::shared_ptr<Window::SectionMemento> createMemento() override;

	bool floatPlayerHandleWheelEvent(QEvent *e) override;
	QRect floatPlayerAvailableRect() override;

	void checkActivation() override;

private:
	struct State;

	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

	void setupTop();
	void setSection(Api::TaskSection section);
	void showMenu(std::optional<Api::Task> task);
	void loadCached();
	void reload();
	void loadMore();
	void rebuild();
	void updateControlsGeometry();

	const std::unique_ptr<State> _state;

};

// Opens the tasks in the main column.
void showTasks(not_null<Window::SessionController*> controller);

} // namespace MtsLink

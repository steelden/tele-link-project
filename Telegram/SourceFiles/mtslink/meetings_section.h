/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.
*/
#pragma once

#include "window/section_widget.h"
#include "window/section_memento.h"

namespace Ui {
class ScrollArea;
class VerticalLayout;
} // namespace Ui

namespace MtsLink {

// The meetings of a period of the Outlook calendar in the main column,
// grouped by the days.
class MeetingsMemento final : public Window::SectionMemento {
public:
	explicit MeetingsMemento(int index = 0) : _index(index) {
	}

	object_ptr<Window::SectionWidget> createWidget(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		Window::Column column,
		const QRect &geometry) override;

	[[nodiscard]] int index() const {
		return _index;
	}

private:
	int _index = 0;

};

class MeetingsWidget final : public Window::SectionWidget {
public:
	MeetingsWidget(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		int index);
	~MeetingsWidget();

	bool hasTopBarShadow() const override {
		return true;
	}
	// The row of the period in the chats list is the active one.
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

	void setIndex(int index);
	// The meeting of a clicked reminder: expanded, scrolled to, flashed.
	void showPendingMeeting();
	void scrollToPendingRow();
	void showMenu();
	void rebuild();
	void updateControlsGeometry();

	const std::unique_ptr<State> _state;

};

} // namespace MtsLink

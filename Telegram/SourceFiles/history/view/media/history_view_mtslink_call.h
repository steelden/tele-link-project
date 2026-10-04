/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.

An MTS Link call message: a big green "join" button while the call is
going on, a small hung up handset when it is finished.
*/
#pragma once

#include "history/view/media/history_view_media.h"

namespace Data {
struct MtsLinkCall;
} // namespace Data

namespace HistoryView {

class MtsLinkCall final : public Media {
public:
	MtsLinkCall(
		not_null<Element*> parent,
		not_null<const Data::MtsLinkCall*> call);

	void draw(Painter &p, const PaintContext &context) const override;
	TextState textState(QPoint point, StateRequest request) const override;

	bool toggleSelectionByHandlerClick(const ClickHandlerPtr &p) const override {
		return true;
	}
	bool dragItemByHandler(const ClickHandlerPtr &p) const override {
		return false;
	}
	bool needsBubble() const override {
		return true;
	}
	bool customInfoLayout() const override {
		return false;
	}

private:
	QSize countOptimalSize() override;

	const bool _ongoing = false;
	const QString _joinLink;
	const QString _recordLink;
	QString _title;
	QString _status;
	ClickHandlerPtr _link;

};

} // namespace HistoryView

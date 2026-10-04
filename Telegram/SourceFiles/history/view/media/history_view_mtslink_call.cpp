/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.
*/
#include "history/view/media/history_view_mtslink_call.h"

#include "data/data_media_types.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_cursor_state.h"
#include "history/view/history_view_element.h"
#include "lang/lang_keys.h"
#include "mtslink/call_window.h"
#include "ui/chat/chat_style.h"
#include "ui/painter.h"
#include "ui/text/format_values.h"
#include "styles/style_chat.h"

namespace HistoryView {

MtsLinkCall::MtsLinkCall(
	not_null<Element*> parent,
	not_null<const Data::MtsLinkCall*> call)
: Media(parent)
, _ongoing(call->ongoing)
, _joinLink(call->joinLink)
, _recordLink(call->recordLink)
, _title(call->title) {
	// The time is in the usual message info, the status is the duration.
	_status = _ongoing
		? tr::lng_mtslink_call_ongoing(tr::now)
		: !_recordLink.isEmpty()
		? ((call->duration > 0)
			? Ui::FormatDurationWords(call->duration)
			: tr::lng_mtslink_materials_record_open(tr::now))
		: (call->duration > 0)
		? Ui::FormatDurationWords(call->duration)
		: QString();
}

QSize MtsLinkCall::countOptimalSize() {
	if (_ongoing && !_joinLink.isEmpty()) {
		const auto link = _joinLink;
		// The whole message joins, as the download button of a file.
		_link = std::make_shared<LambdaClickHandler>([=] {
			MtsLink::joinCallLink(link);
		});
	}
	if (!_recordLink.isEmpty()) {
		const auto link = _recordLink;
		const auto title = _title;
		_link = std::make_shared<LambdaClickHandler>([=] {
			MtsLink::openCallLink(link, title);
		});
	}
	const auto big = _ongoing || !_recordLink.isEmpty();
	auto maxWidth = big ? st::mtsLinkCallWidth : st::historyCallWidth;
	if (!big) {
		// The duration and the message info in the bottom line.
		accumulate_max(
			maxWidth,
			st::historyCallLeft
				+ st::normalFont->width(_status)
				+ st::historyCallStatusSkip
				+ _parent->bottomInfoFirstLineWidth()
				+ st::msgPadding.right());
	}
	auto minHeight = big ? st::mtsLinkCallHeight : st::historyCallHeight;
	if (!isBubbleTop()) {
		minHeight -= st::msgFileTopMinus;
	}
	return { maxWidth, minHeight };
}

void MtsLinkCall::draw(Painter &p, const PaintContext &context) const {
	if (width() < st::msgPadding.left() + st::msgPadding.right() + 1) {
		return;
	}
	const auto stm = context.messageStyle();
	const auto paintw = std::min(width(), maxWidth());
	const auto topMinus = isBubbleTop() ? 0 : st::msgFileTopMinus;

	if (_ongoing || !_recordLink.isEmpty()) {
		const auto size = st::mtsLinkCallJoinSize;
		const auto left = st::historyCallLeft - st::lineWidth * 4;
		const auto top = (height() - size) / 2;
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(_ongoing ? st::callAnswerBg : stm->msgFileBg);
			p.drawEllipse(left, top, size, size);
		}
		const auto &icon = _ongoing
			? st::mtsLinkCallJoinIcon
			: stm->historyFilePlay;
		icon.paint(
			p,
			left + (size - icon.width()) / 2,
			top + (size - icon.height()) / 2,
			paintw);
		const auto textLeft = left + size + st::historyCallLeft;
		const auto textWidth = paintw - textLeft - st::historyCallLeft;
		const auto textTop = top
			+ (size - st::semiboldFont->height - st::normalFont->height) / 2;
		p.setFont(st::semiboldFont);
		p.setPen(stm->historyFileNameFg);
		p.drawTextLeft(
			textLeft,
			textTop,
			paintw,
			st::semiboldFont->elided(_title, textWidth));
		p.setFont(st::normalFont);
		p.setPen(stm->mediaFg);
		p.drawTextLeft(
			textLeft,
			textTop + st::semiboldFont->height,
			paintw,
			st::normalFont->elided(
				_status,
				textWidth
					- _parent->bottomInfoFirstLineWidth()
					- st::historyCallStatusSkip));
		return;
	}

	const auto nameleft = st::historyCallLeft;
	const auto &icon = st::mtsLinkCallEndedIcon;
	const auto textWidth = paintw
		- nameleft
		- st::historyCallIconPosition.x()
		- icon.width()
		- st::historyCallStatusSkip;
	p.setFont(st::semiboldFont);
	p.setPen(stm->historyFileNameFg);
	p.drawTextLeft(
		nameleft,
		st::historyCallTop - topMinus,
		paintw,
		st::semiboldFont->elided(_title, textWidth));
	p.setFont(st::normalFont);
	p.setPen(stm->mediaFg);
	p.drawTextLeft(
		nameleft,
		st::historyCallStatusTop - topMinus,
		paintw,
		st::normalFont->elided(
			_status,
			paintw
				- nameleft
				- st::historyCallStatusSkip
				- _parent->bottomInfoFirstLineWidth()));
	// In the title line, the message info is in the bottom right corner.
	icon.paint(
		p,
		paintw - st::msgPadding.right() - icon.width(),
		st::historyCallTop
			- topMinus
			+ (st::semiboldFont->height - icon.height()) / 2,
		paintw,
		stm->mediaFg->c);
}

TextState MtsLinkCall::textState(QPoint point, StateRequest request) const {
	auto result = TextState(_parent);
	if (_link && QRect(0, 0, width(), height()).contains(point)) {
		result.link = _link;
	}
	return result;
}

} // namespace HistoryView

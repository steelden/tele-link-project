/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "chat_helpers/rich_paste_toast.h"

#include "boxes/premium_preview_box.h"
#include "chat_helpers/message_field.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "iv/editor/iv_editor_clipboard_import.h"
#include "iv/editor/iv_editor_session.h"
#include "iv/iv_rich_page.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mtslink/code_language.h"
#include "window/window_session_controller.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/toast/toast_widget.h"
#include "ui/widgets/buttons.h"
#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"

#include <QtCore/QMimeData>
#include <QtCore/QRegularExpression>

namespace ChatHelpers {
namespace {

constexpr auto kToastDuration = 9 * crl::time(1000);
constexpr auto kOfferTimeout = 30 * crl::time(1000);

// The html of the code from an IDE: an element with "white-space:pre" and the
// lines in <p> (JetBrains) or <div> (VS Code), the html parser collapsed the
// spaces in it and made a code block of each line. It is replaced by <pre>
// with the lines separated by newlines.
[[nodiscard]] QString PreformattedToPre(const QString &html) {
	static const auto kTag = QRegularExpression(
		u"<(/?)([a-zA-Z][\\w-]*)([^>]*)>"_q);
	static const auto kPreformatted = QRegularExpression(
		u"white-space\\s*:\\s*(pre-wrap|break-spaces|pre)(?![\\w-])"_q,
		QRegularExpression::CaseInsensitiveOption);
	auto result = QString();
	auto position = 0;
	auto element = QString();
	auto nesting = 0;
	auto lastLineBreak = false;
	auto found = false;
	auto tags = kTag.globalMatch(html);
	while (tags.hasNext()) {
		const auto tag = tags.next();
		const auto text = QStringView(html).mid(
			position,
			tag.capturedStart() - position);
		result += text;
		if (!text.isEmpty()) {
			lastLineBreak = false;
		}
		position = tag.capturedEnd();

		const auto closing = !tag.capturedView(1).isEmpty();
		const auto name = tag.captured(2).toLower();
		const auto attributes = tag.capturedView(3);
		const auto selfClosing = attributes.endsWith('/');
		if (element.isEmpty()) {
			if (!closing
				&& !selfClosing
				&& name != u"br"_q
				&& kPreformatted.match(attributes).hasMatch()) {
				element = name;
				nesting = 1;
				lastLineBreak = false;
				found = true;
				result += u"<pre>"_q;
			} else {
				result += tag.capturedView(0);
			}
			continue;
		}
		const auto line = (name == u"p"_q)
			|| (name == u"div"_q)
			|| (name == u"li"_q);
		if (name == element && !selfClosing) {
			if (!closing) {
				++nesting;
			} else if (!--nesting) {
				element = QString();
				result += u"</pre>"_q;
				continue;
			}
		}
		if (name == u"br"_q) {
			result += '\n';
			lastLineBreak = true;
		} else if (line) {
			// A line ends with its element, an empty line is <p></p> or
			// <div><br></div>.
			if (closing) {
				if (!lastLineBreak) {
					result += '\n';
				}
				lastLineBreak = false;
			}
		} else {
			result += tag.capturedView(0);
		}
	}
	if (!found) {
		return html;
	}
	result += QStringView(html).mid(position);
	return result;
}

// The pasted code: the tabs (narrow in the text) as the spaces with the stops
// each 4 columns, the common indent removed. The selection in an IDE usually
// starts after the indent of the first line, so it is not counted.
[[nodiscard]] QString PrepareCode(const QString &code) {
	constexpr auto kTabSize = 4;
	auto lines = code.split('\n');
	for (auto &line : lines) {
		if (!line.contains('\t')) {
			continue;
		}
		auto expanded = QString();
		for (const auto ch : line) {
			if (ch == '\t') {
				expanded += QString(
					kTabSize - (expanded.size() % kTabSize),
					' ');
			} else {
				expanded += ch;
			}
		}
		line = expanded;
	}
	const auto indent = [](const QString &line) {
		auto result = 0;
		while (result < line.size() && line[result] == ' ') {
			++result;
		}
		return result;
	};
	auto common = -1;
	for (auto i = 1; i < lines.size(); ++i) {
		if (!lines[i].trimmed().isEmpty()) {
			const auto value = indent(lines[i]);
			common = (common < 0) ? value : std::min(common, value);
		}
	}
	if (common > 0) {
		for (auto i = 0; i < lines.size(); ++i) {
			lines[i].remove(0, std::min(common, indent(lines[i])));
		}
	}
	return lines.join('\n');
}

// The code blocks: prepared, the language of them without one guessed by the
// content.
void PrepareCodeBlocks(std::vector<Iv::RichPage::Block> &blocks) {
	for (auto &block : blocks) {
		if (block.kind == Iv::RichPage::BlockKind::Code) {
			auto &text = block.text.text;
			text.text = PrepareCode(text.text);
			text.entities.clear();
			if (block.language.isEmpty()) {
				block.language = MtsLink::GuessCodeLanguage(text.text);
			}
		}
		PrepareCodeBlocks(block.blocks);
	}
}

} // namespace

std::optional<RichPasteDecision> MimeDataRichPasteOffer(
		not_null<Main::Session*> session,
		not_null<const QMimeData*> data) {
	if (PasteAsPlainTextRequested()) {
		return std::nullopt;
	}
	const auto limits = Iv::ResolveRichMessageLimits(session);
	LOG(("MtsLink Paste: html=%1 text=%2 formats=%3"
		).arg(data->hasHtml() ? 1 : 0
		).arg(data->hasText() ? 1 : 0
		).arg(data->formats().join(',')));
	// MTS Link: no rich messages (the editor of them is Premium only), the
	// formatting of the pasted HTML a message can have is applied in the
	// field: bold, italic, links, code, quotes, lists and headings as lines.
	if (data->hasHtml()) {
		const auto html = data->html();
		const auto prepared = PreformattedToPre(html);
		auto copy = std::shared_ptr<QMimeData>();
		if (prepared != html) {
			copy = CloneMimeData(data);
			copy->setHtml(prepared);
		}
		auto imported = Iv::Editor::BlocksFromMimeData(
			session,
			copy ? copy.get() : data.get(),
			limits,
			0);
		if (!imported || imported->blocks.empty()) {
			return std::nullopt;
		}
		PrepareCodeBlocks(imported->blocks);
		auto page = Iv::RichPage();
		page.blocks = std::move(imported->blocks);
		const auto flat = Iv::FlattenRichPageToSimpleText(page);
		if (flat.text.trimmed().isEmpty() || flat.entities.isEmpty()) {
			return std::nullopt;
		}
		return RichPasteDecision{
			.offer = RichPasteOffer::Field,
			.markdown = TextWithTags{
				flat.text,
				TextUtilities::ConvertEntitiesToTextTags(flat.entities),
			},
			.applyNow = true,
		};
	} else if (!data->hasText()) {
		return std::nullopt;
	}
	const auto text = data->text();
	if (auto markdown = Iv::Editor::ComposeFieldMarkdown(
			session,
			text,
			limits)) {
		return RichPasteDecision{
			.offer = RichPasteOffer::Field,
			.markdown = std::move(*markdown),
		};
	}
	// MTS Link: no rich messages editor (Premium), the text as pasted.
	return std::nullopt;
}

TextWithTags TextWithTagsReplaced(
		const TextWithTags &original,
		int from,
		int till,
		const TextWithTags &with) {
	auto result = TextWithTags();
	result.text = original.text.mid(0, from)
		+ with.text
		+ original.text.mid(till);
	const auto shift = int(with.text.size()) - (till - from);
	for (const auto &tag : original.tags) {
		if (tag.offset < from) {
			const auto end = std::min(tag.offset + tag.length, from);
			result.tags.push_back({ tag.offset, end - tag.offset, tag.id });
		}
	}
	for (const auto &tag : with.tags) {
		result.tags.push_back({ from + tag.offset, tag.length, tag.id });
	}
	for (const auto &tag : original.tags) {
		const auto end = tag.offset + tag.length;
		if (end > till) {
			const auto start = std::max(tag.offset, till);
			result.tags.push_back({ start + shift, end - start, tag.id });
		}
	}
	return result;
}

std::shared_ptr<QMimeData> CloneMimeData(not_null<const QMimeData*> data) {
	auto result = std::make_shared<QMimeData>();
	for (const auto &format : data->formats()) {
		result->setData(format, data->data(format));
	}
	return result;
}

bool RichPasteOfferThrottle::take() {
	const auto now = crl::now();
	if (_lastShown && (now - _lastShown) < kOfferTimeout) {
		return false;
	}
	_lastShown = now;
	return true;
}

void ShowRichPasteToast(RichPasteToastArgs &&args) {
	const auto session = args.session;
	const auto undo = (args.offer == RichPasteOffer::Plain);
	const auto field = (args.offer == RichPasteOffer::Field);
	const auto markdown = (args.offer == RichPasteOffer::Markdown);
	const auto locked = !undo
		&& !field
		&& !Iv::Editor::SessionPremium(session);
	LOG(("MtsLink Paste: toast offer=%1 locked=%2"
		).arg(int(args.offer)
		).arg(locked ? 1 : 0));
	if (locked
		&& Core::App().settings().readPref<bool>(
			"rich_paste_toast_hidden",
			false)) {
		return;
	}
	const auto button = locked
		? tr::lng_archive_hint_button(tr::now)
		: undo
		? tr::lng_rich_paste_toast_undo(tr::now)
		: field
		? tr::lng_rich_paste_toast_apply(tr::now)
		: tr::lng_rich_paste_toast_open(tr::now);
	auto text = tr::bold(undo
		? tr::lng_rich_paste_toast_markdown(tr::now)
		: tr::lng_rich_paste_toast(tr::now)
	).append('\n').append(locked
		? tr::lng_rich_paste_toast_premium(
			tr::now,
			lt_link,
			tr::link(tr::bold(
				tr::lng_rich_paste_toast_premium_link(tr::now))),
			tr::marked)
		: undo
		? tr::lng_rich_paste_toast_plain(tr::now, tr::rich)
		: field
		? tr::lng_rich_paste_toast_field(tr::now, tr::rich)
		: markdown
		? tr::lng_rich_paste_toast_editor_markdown(tr::now, tr::rich)
		: tr::lng_rich_paste_toast_editor(tr::now, tr::rich));
	auto filter = Ui::Toast::ClickHandlerFilter();
	if (locked) {
		filter = [=](const ClickHandlerPtr &handler, Qt::MouseButton mouse) {
			if (mouse != Qt::LeftButton) {
				return false;
			} else if (auto show = Iv::Editor::ActiveWindowShow(session)) {
				ShowPremiumPreviewToBuy(
					std::move(show),
					PremiumFeature::RichFormatting);
			} else if (const auto window = session->tryResolveWindow()) {
				ShowPremiumPreviewToBuy(
					window,
					PremiumFeature::RichFormatting);
			}
			return true;
		};
	}
	const auto st = std::make_shared<style::Toast>(st::historyPremiumToast);
	if (!button.isEmpty()) {
		st->padding.setRight(
			st::historyPremiumViewSet.style.font->width(button)
			- st::historyPremiumViewSet.width);
	}
	const auto weak = Ui::Toast::Show(args.parent, Ui::Toast::Config{
		.text = std::move(text),
		.filter = std::move(filter),
		.similarLines = true,
		.iconLottie = (locked ? u"toast/star_premium_2"_q : QString()),
		.iconLottieSize = st::toastLottieIconSize,
		.st = st.get(),
		.attach = RectPart::Bottom,
		.addToAttachSide = std::move(args.bottomOffset),
		.acceptinput = true,
		.duration = kToastDuration,
	});
	const auto strong = weak.get();
	if (!strong) {
		return;
	}
	const auto widget = strong->widget();
	widget->lifetime().add([st] {});
	if (args.cancel) {
		std::move(args.cancel) | rpl::on_next([=] {
			if (const auto strong = weak.get()) {
				strong->hideAnimated();
			}
		}, widget->lifetime());
	}
	if (button.isEmpty()) {
		return;
	}
	Fn<void()> action = std::move(args.action);
	if (locked) {
		action = [=] {
			Core::App().settings().writePref<bool>(
				"rich_paste_toast_hidden",
				true);
		};
	}
	const auto activate = Ui::CreateChild<Ui::RoundButton>(
		widget.get(),
		rpl::single(button),
		st::historyPremiumViewSet);
	activate->show();
	activate->setClickedCallback([=, action = std::move(action)] {
		if (const auto strong = weak.get()) {
			strong->hideAnimated();
		}
		if (action) {
			action();
		}
	});
	rpl::combine(
		widget->sizeValue(),
		activate->sizeValue()
	) | rpl::on_next([=](QSize outer, QSize inner) {
		activate->moveToRight(
			0,
			(outer.height() - inner.height()) / 2,
			outer.width());
	}, widget->lifetime());
}

} // namespace ChatHelpers

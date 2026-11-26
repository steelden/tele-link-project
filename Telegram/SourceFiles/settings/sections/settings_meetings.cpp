/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.
*/
#include "settings/sections/settings_meetings.h"

#include "base/options.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mtslink/meetings_peers.h"
#include "mtslink/outlook_calendar.h"
#include "settings/sections/settings_main.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "settings/settings_common_session.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/color_editor.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/notifications_manager.h"
#include "window/window_session_controller.h"
#include "mainwindow.h"
#include "styles/style_boxes.h"
#include "styles/style_passcode_box.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace Settings {
namespace {

using namespace Builder;

// The "Open meeting" button on the folders panel.
void BuildMeetingButtonSection(SectionBuilder &builder) {
	builder.add([](const WidgetContext &ctx) {
		const auto content = ctx.container;

		Ui::AddSkip(content);
		Ui::AddSubsectionTitle(content, tr::lng_mtslink_settings_meetings());
		auto &meetingOption = base::options::lookup<bool>(
			"mtslink-folders-meeting-button");
		const auto meeting = content->add(
			object_ptr<Ui::SettingsButton>(
				content,
				tr::lng_mtslink_folders_meeting_button(),
				st::settingsButtonNoIcon));
		meeting->toggleOn(rpl::single(meetingOption.value()));
		meeting->toggledValue(
		) | rpl::skip(1) | rpl::on_next([=](bool checked) {
			base::options::lookup<bool>(
				"mtslink-folders-meeting-button").set(checked);
		}, meeting->lifetime());
		Ui::AddSkip(content);

		return SectionBuilder::WidgetToAdd{};
	});
}

// The presets of the color of the reminders (and "Custom color").
const auto kRemindColors = std::array<QColor, 5>{
	QColor(0x2f, 0x80, 0xed),
	QColor(0x27, 0xae, 0x60),
	QColor(0xf2, 0x99, 0x4a),
	QColor(0xeb, 0x57, 0x57),
	QColor(0x9b, 0x51, 0xe0),
};

[[nodiscard]] QString RemindColorPresetName(int index) {
	switch (index) {
	case 0: return tr::lng_mtslink_meetings_remind_color_blue(tr::now);
	case 1: return tr::lng_mtslink_meetings_remind_color_green(tr::now);
	case 2: return tr::lng_mtslink_meetings_remind_color_orange(tr::now);
	case 3: return tr::lng_mtslink_meetings_remind_color_red(tr::now);
	case 4: return tr::lng_mtslink_meetings_remind_color_violet(tr::now);
	}
	return QString();
}

// 0: of the theme, 1..5: the presets, 6: custom.
[[nodiscard]] int RemindColorIndex(std::optional<QColor> color) {
	if (!color) {
		return 0;
	}
	const auto i = ranges::find(kRemindColors, *color);
	return (i != end(kRemindColors))
		? (1 + int(i - begin(kRemindColors)))
		: int(kRemindColors.size() + 1);
}

[[nodiscard]] QString RemindColorName(std::optional<QColor> color) {
	const auto index = RemindColorIndex(color);
	return (index == 0)
		? tr::lng_mtslink_meetings_remind_color_theme(tr::now)
		: (index <= int(kRemindColors.size()))
		? RemindColorPresetName(index - 1)
		: tr::lng_mtslink_meetings_remind_color_custom(tr::now);
}

// A radio list in a box (as SingleChoiceBox), the clicks of the options
// are given (the custom color, the file are opened by each click).
void RadioListBox(
		not_null<Ui::GenericBox*> box,
		rpl::producer<QString> title,
		std::vector<QString> options,
		int selected,
		Fn<void(int)> clicked) {
	box->setTitle(std::move(title));
	box->addButton(tr::lng_box_ok(), [=] { box->closeBox(); });
	const auto group = std::make_shared<Ui::RadiobuttonGroup>(selected);
	const auto layout = box->verticalLayout();
	layout->add(object_ptr<Ui::FixedHeightWidget>(
		layout,
		st::boxOptionListPadding.top() + st::autolockButton.margin.top()));
	for (auto index = 0; index != int(options.size()); ++index) {
		const auto radio = layout->add(
			object_ptr<Ui::Radiobutton>(
				layout,
				group,
				index,
				options[index],
				st::defaultBoxCheckbox,
				st::defaultRadio),
			QMargins(
				st::boxPadding.left() + st::boxOptionListPadding.left(),
				0,
				st::boxPadding.right(),
				st::boxOptionListSkip));
		radio->clicks(
		) | rpl::on_next([=] {
			clicked(index);
		}, radio->lifetime());
	}
}

void AddRemindColorButton(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	const auto current = container->lifetime().make_state<
		rpl::variable<std::optional<QColor>>>(MtsLink::meetingsRemindColor());
	const auto set = [=](std::optional<QColor> color) {
		*current = color;
		MtsLink::setMeetingsRemindColor(color);
	};
	const auto editCustom = [=] {
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			const auto editor = box->addRow(object_ptr<ColorEditor>(
				box,
				ColorEditor::Mode::RGBA,
				current->current().value_or(st::windowBgActive->c)));
			const auto save = crl::guard(box, [=] {
				auto color = editor->color();
				color.setAlpha(255);
				set(color);
				box->closeBox();
			});
			editor->submitRequests(
			) | rpl::on_next(save, editor->lifetime());
			box->setFocusCallback([=] {
				editor->setInnerFocus();
			});
			box->addButton(tr::lng_settings_save(), save);
			box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
			box->setTitle(tr::lng_mtslink_meetings_remind_color());
			box->setWidth(editor->width());
		}));
	};
	const auto button = AddButtonWithLabel(
		container,
		tr::lng_mtslink_meetings_remind_color(),
		current->value() | rpl::map(RemindColorName),
		st::settingsButtonNoIcon);
	button->addClickHandler([=] {
		auto options = std::vector<QString>();
		options.push_back(tr::lng_mtslink_meetings_remind_color_theme(tr::now));
		for (auto i = 0; i != int(kRemindColors.size()); ++i) {
			options.push_back(RemindColorPresetName(i));
		}
		options.push_back(tr::lng_mtslink_meetings_remind_color_custom(tr::now));
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			RadioListBox(
				box,
				tr::lng_mtslink_meetings_remind_color(),
				options,
				RemindColorIndex(current->current()),
				crl::guard(box, [=](int index) {
					box->closeBox();
					if (index == 0) {
						set(std::nullopt);
					} else if (index <= int(kRemindColors.size())) {
						set(kRemindColors[index - 1]);
					} else {
						editCustom();
					}
				}));
		}));
	});
}

void AddRemindSoundButton(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	const auto current = container->lifetime().make_state<
		rpl::variable<QString>>(MtsLink::meetingsRemindSound());
	const auto set = [=](const QString &key) {
		*current = key;
		MtsLink::setMeetingsRemindSound(key);
		// The preview.
		Core::App().notifications().playSoundFile(
			MtsLink::meetingsRemindSoundPath(key));
	};
	const auto button = AddButtonWithLabel(
		container,
		tr::lng_mtslink_meetings_remind_sound(),
		current->value() | rpl::map(MtsLink::meetingsRemindSoundName),
		st::settingsButtonNoIcon);
	button->addClickHandler([=] {
		auto keys = MtsLink::meetingsRemindSounds();
		const auto now = current->current();
		if (now.startsWith(u"file:"_q)) {
			keys.push_back(now);
		}
		auto options = std::vector<QString>();
		for (const auto &key : keys) {
			options.push_back(MtsLink::meetingsRemindSoundName(key));
		}
		options.push_back(tr::lng_mtslink_meetings_remind_sound_choose(tr::now));
		const auto i = ranges::find(keys, now);
		const auto selected = (i != end(keys)) ? int(i - begin(keys)) : 0;
		const auto weak = base::make_weak(controller);
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			RadioListBox(
				box,
				tr::lng_mtslink_meetings_remind_sound(),
				options,
				selected,
				crl::guard(box, [=](int index) {
					if (index < int(keys.size())) {
						// Stays open: the sounds are listened to.
						set(keys[index]);
						return;
					}
					box->closeBox();
					const auto strong = weak.get();
					if (!strong) {
						return;
					}
					FileDialog::GetOpenPath(
						strong->widget().get(),
						tr::lng_mtslink_meetings_remind_sound(tr::now),
						FileDialog::AudioFilesFilter(),
						crl::guard(container, [=](
								FileDialog::OpenResult &&result) {
							if (!result.paths.isEmpty()) {
								set(u"file:"_q + result.paths.front());
							}
						}));
				}));
		}));
	});
}

// The reminders before the meetings: any of the options.
void AddRemindButtons(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, tr::lng_mtslink_meetings_remind());
	for (const auto minutes : MtsLink::meetingsRemindOptions()) {
		const auto button = container->add(
			object_ptr<Ui::SettingsButton>(
				container,
				rpl::single(MtsLink::meetingsRemindName(minutes)),
				st::settingsButtonNoIcon));
		button->toggleOn(
			rpl::single(MtsLink::meetingsRemindEnabled(minutes)));
		button->toggledValue(
		) | rpl::skip(1) | rpl::on_next([=](bool checked) {
			MtsLink::setMeetingsRemindEnabled(minutes, checked);
		}, button->lifetime());
	}
	AddRemindColorButton(container, controller);
	AddRemindSoundButton(container, controller);
	Ui::AddSkip(container);
	Ui::AddDividerText(container, tr::lng_mtslink_meetings_remind_about());
}

// The meetings of Outlook (Windows) as a folder: off by default, turned on
// only after a successful test connection.
void BuildOutlookSection(SectionBuilder &builder) {
	if (!MtsLink::outlookSupported()) {
		return;
	}
	builder.add([](const WidgetContext &ctx) {
		const auto controller = ctx.controller;
		const auto content = ctx.container;

		Ui::AddDivider(content);
		Ui::AddSkip(content);
		Ui::AddSubsectionTitle(content, tr::lng_mtslink_settings_outlook());
		const auto button = content->add(
			object_ptr<Ui::SettingsButton>(
				content,
				tr::lng_mtslink_settings_outlook_meetings(),
				st::settingsButtonNoIcon));
		if (ctx.highlights) {
			ctx.highlights->push_back({ u"meetings/outlook"_q, { button } });
		}
		const auto testing = button->lifetime().make_state<bool>(false);
		// Turned back off after a failed test connection.
		const auto toggles = button->lifetime().make_state<
			rpl::event_stream<bool>>();
		button->toggleOn(
			rpl::single(
				MtsLink::outlookMeetingsEnabled()
			) | rpl::then(toggles->events())
		)->toggledValue(
		) | rpl::on_next([=](bool toggled) {
			const auto session = &controller->session();
			if (!toggled) {
				if (!*testing && MtsLink::outlookMeetingsEnabled()) {
					MtsLink::setMeetingsEnabled(session, false);
				}
				return;
			} else if (*testing || MtsLink::outlookMeetingsEnabled()) {
				return;
			}
			*testing = true;
			controller->showToast(
				tr::lng_mtslink_outlook_checking(tr::now));
			MtsLink::testOutlookConnection(crl::guard(button, [=](bool ok) {
				*testing = false;
				if (!button->toggled()) {
					// Turned off while the test was going.
					return;
				} else if (ok) {
					MtsLink::setMeetingsEnabled(session, true);
					controller->showToast(
						tr::lng_mtslink_outlook_enabled(tr::now));
				} else {
					toggles->fire(false);
					controller->showToast(
						tr::lng_mtslink_outlook_failed(tr::now));
				}
			}));
		}, button->lifetime());

		// The settings of the meetings of Outlook, while turned on.
		const auto wrap = content->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				content,
				object_ptr<Ui::VerticalLayout>(content)));
		const auto inner = wrap->entity();

		// The background refresh of the meetings.
		const auto refresh = inner->lifetime().make_state<
			rpl::variable<int>>(MtsLink::meetingsRefreshSeconds());
		const auto refreshButton = AddButtonWithLabel(
			inner,
			tr::lng_mtslink_meetings_refresh(),
			refresh->value() | rpl::map(MtsLink::meetingsRefreshName),
			st::settingsButtonNoIcon);
		refreshButton->addClickHandler([=] {
			const auto values = MtsLink::meetingsRefreshOptions();
			auto options = std::vector<QString>();
			for (const auto value : values) {
				options.push_back(MtsLink::meetingsRefreshName(value));
			}
			const auto found = ranges::find(values, refresh->current());
			controller->show(Box(SingleChoiceBox, SingleChoiceBoxArgs{
				.title = tr::lng_mtslink_meetings_refresh(),
				.options = options,
				.initialSelection = (found != end(values))
					? int(found - begin(values))
					: 3,
				.callback = [=](int index) {
					*refresh = values[index];
					MtsLink::setMeetingsRefreshSeconds(values[index]);
				},
			}));
		});

		// The past days in the lists (today is shown whole anyway).
		const auto showPast = inner->add(
			object_ptr<Ui::SettingsButton>(
				inner,
				tr::lng_mtslink_meetings_show_past(),
				st::settingsButtonNoIcon));
		showPast->toggleOn(rpl::single(MtsLink::meetingsShowPast()));
		showPast->toggledValue(
		) | rpl::skip(1) | rpl::on_next([=](bool checked) {
			MtsLink::setMeetingsShowPast(checked);
		}, showPast->lifetime());

		// The weekends: the days in the chats list, the meetings.
		const auto showWeekends = inner->add(
			object_ptr<Ui::SettingsButton>(
				inner,
				tr::lng_mtslink_meetings_show_weekends(),
				st::settingsButtonNoIcon));
		showWeekends->toggleOn(rpl::single(MtsLink::meetingsShowWeekends()));
		showWeekends->toggledValue(
		) | rpl::skip(1) | rpl::on_next([=](bool checked) {
			MtsLink::setMeetingsShowWeekends(&controller->session(), checked);
		}, showWeekends->lifetime());

		// The rows of the meetings: the width, the call button.
		const auto fullWidth = inner->add(
			object_ptr<Ui::SettingsButton>(
				inner,
				tr::lng_mtslink_meetings_full_width(),
				st::settingsButtonNoIcon));
		fullWidth->toggleOn(rpl::single(MtsLink::meetingsFullWidth()));
		fullWidth->toggledValue(
		) | rpl::skip(1) | rpl::on_next([=](bool checked) {
			MtsLink::setMeetingsFullWidth(checked);
		}, fullWidth->lifetime());

		const auto mentions = inner->lifetime().make_state<
			rpl::variable<bool>>(MtsLink::meetingsPeopleAsMentions());
		const auto peopleName = [](bool mentions) {
			return mentions
				? tr::lng_mtslink_meetings_people_mentions(tr::now)
				: tr::lng_mtslink_meetings_people_names(tr::now);
		};
		const auto peopleButton = AddButtonWithLabel(
			inner,
			tr::lng_mtslink_meetings_people(),
			mentions->value() | rpl::map(peopleName),
			st::settingsButtonNoIcon);
		peopleButton->addClickHandler([=] {
			controller->show(Box(SingleChoiceBox, SingleChoiceBoxArgs{
				.title = tr::lng_mtslink_meetings_people(),
				.options = { peopleName(false), peopleName(true) },
				.initialSelection = mentions->current() ? 1 : 0,
				.callback = [=](int index) {
					*mentions = (index == 1);
					MtsLink::setMeetingsPeopleAsMentions(index == 1);
				},
			}));
		});

		const auto callLeft = inner->lifetime().make_state<
			rpl::variable<bool>>(MtsLink::meetingsCallLeft());
		const auto sideName = [](bool left) {
			return left
				? tr::lng_mtslink_meetings_call_left(tr::now)
				: tr::lng_mtslink_meetings_call_right(tr::now);
		};
		const auto callButton = AddButtonWithLabel(
			inner,
			tr::lng_mtslink_meetings_call_side(),
			callLeft->value() | rpl::map(sideName),
			st::settingsButtonNoIcon);
		callButton->addClickHandler([=] {
			controller->show(Box(SingleChoiceBox, SingleChoiceBoxArgs{
				.title = tr::lng_mtslink_meetings_call_side(),
				.options = { sideName(true), sideName(false) },
				.initialSelection = callLeft->current() ? 0 : 1,
				.callback = [=](int index) {
					*callLeft = (index == 0);
					MtsLink::setMeetingsCallLeft(index == 0);
				},
			}));
		});

		// The future: the number of the periods in the chats list, for
		// each length.
		Ui::AddSkip(inner);
		Ui::AddDivider(inner);
		Ui::AddSkip(inner);
		Ui::AddSubsectionTitle(inner, tr::lng_mtslink_meetings_future());
		const auto addCount = [=](
				MtsLink::MeetingsPeriod period,
				rpl::producer<QString> title) {
			const auto count = inner->lifetime().make_state<
				rpl::variable<int>>(MtsLink::meetingsPeriodsCount(period));
			const auto button = AddButtonWithLabel(
				inner,
				rpl::duplicate(title),
				count->value() | rpl::map([](int value) {
					return QString::number(value);
				}),
				st::settingsButtonNoIcon);
			const auto titleText = button->lifetime().make_state<QString>();
			std::move(title) | rpl::on_next([=](QString text) {
				*titleText = text;
			}, button->lifetime());
			button->addClickHandler([=] {
				const auto values = MtsLink::meetingsPeriodsCountOptions(
					period);
				auto options = std::vector<QString>();
				for (const auto value : values) {
					options.push_back(QString::number(value));
				}
				const auto found = ranges::find(values, count->current());
				controller->show(Box(SingleChoiceBox, SingleChoiceBoxArgs{
					.title = rpl::single(*titleText),
					.options = options,
					.initialSelection = (found != end(values))
						? int(found - begin(values))
						: 0,
					.callback = [=](int index) {
						*count = values[index];
						MtsLink::setMeetingsPeriodsCount(
							&controller->session(),
							period,
							values[index]);
					},
				}));
			});
		};
		addCount(
			MtsLink::MeetingsPeriod::Day,
			tr::lng_mtslink_meetings_count_days());
		addCount(
			MtsLink::MeetingsPeriod::Week,
			tr::lng_mtslink_meetings_count_weeks());
		addCount(
			MtsLink::MeetingsPeriod::Month,
			tr::lng_mtslink_meetings_count_months());
		addCount(
			MtsLink::MeetingsPeriod::Quarter,
			tr::lng_mtslink_meetings_count_quarters());

		AddRemindButtons(inner, controller);

		wrap->toggleOn(button->toggledValue());
		wrap->finishAnimating();

		return SectionBuilder::WidgetToAdd{};
	}, [] {
		return SearchEntry{
			.id = u"meetings/outlook"_q,
			.title = tr::lng_mtslink_settings_outlook_meetings(tr::now),
			.keywords = { u"outlook"_q, u"calendar"_q, u"meetings"_q },
		};
	});
}

class Meetings : public Section<Meetings> {
public:
	Meetings(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();

};

Meetings::Meetings(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

rpl::producer<QString> Meetings::title() {
	return tr::lng_settings_section_meetings();
}

void Meetings::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	const SectionBuildMethod buildMethod = [](
			not_null<Ui::VerticalLayout*> container,
			not_null<Window::SessionController*> controller,
			Fn<void(Type)> showOther,
			rpl::producer<> showFinished) {
		auto &lifetime = container->lifetime();
		const auto highlights = lifetime.make_state<HighlightRegistry>();
		auto builder = SectionBuilder(WidgetContext{
			.container = container,
			.controller = controller,
			.showOther = std::move(showOther),
			.isPaused = Window::PausedIn(
				controller,
				Window::GifPauseReason::Layer),
			.highlights = highlights,
		});

		BuildMeetingButtonSection(builder);
		BuildOutlookSection(builder);

		std::move(showFinished) | rpl::on_next([=] {
			for (const auto &[id, entry] : *highlights) {
				if (entry.widget) {
					controller->checkHighlightControl(
						id,
						entry.widget,
						base::duplicate(entry.args));
				}
			}
		}, lifetime);
	};

	build(content, buildMethod);

	Ui::ResizeFitChild(this, content);
}

const auto kMeta = BuildHelper({
	.id = Meetings::Id(),
	.parentId = MainId(),
	.title = &tr::lng_settings_section_meetings,
	.icon = &st::menuIconSchedule,
}, [](SectionBuilder &builder) {
	if (MtsLink::outlookSupported()) {
		builder.add(nullptr, [] {
			return SearchEntry{
				.id = u"meetings/outlook"_q,
				.title = tr::lng_mtslink_settings_outlook_meetings(tr::now),
				.keywords = { u"outlook"_q, u"calendar"_q, u"meetings"_q },
			};
		});
	}
});

} // namespace

Type MeetingsId() {
	return Meetings::Id();
}

} // namespace Settings

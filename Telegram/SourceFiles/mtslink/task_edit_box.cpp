/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.
*/
#include "mtslink/task_edit_box.h"

#include "mtslink/data_adapters.h"
#include "mtslink/env_config.h"
#include "mtslink/session.h"
#include "data/data_groups.h"
#include "history/history.h"
#include "history/history_item.h"
#include "base/timer.h"
#include "base/qt_signal_producer.h"
#include "core/click_handler_types.h"
#include "mtslink/api/api_users.h"
#include "base/unixtime.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "chat_helpers/message_field.h"
#include "spellcheck/spellcheck_highlight_syntax.h"
#include "ui/chat/chat_style.h"
#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"
#include "chat_helpers/tabbed_selector.h"
#include "ui/boxes/choose_date_time.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/menu/menu.h"
#include "ui/widgets/menu/menu_action.h"
#include "ui/widgets/menu/menu_common.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/basic_click_handlers.h"
#include "ui/text/text_utilities.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

namespace MtsLink {
namespace {

constexpr auto kTitleLimit = 255;
constexpr auto kDescriptionLimit = 1000;
constexpr auto kSearchDelay = crl::time(300);

[[nodiscard]] UserData *UserOf(
		not_null<Main::Session*> session,
		const QString &uuid) {
	return uuid.isEmpty()
		? nullptr
		: session->data().userLoaded(::UserId(uuidToBareId(uuid)));
}

[[nodiscard]] QString SelfUuid(not_null<Main::Session*> session) {
	const auto mts = session->account().mtsLinkSession();
	return mts ? mts->userId() : QString();
}

// The full name as in the chats (the order of the setting), mine too.
[[nodiscard]] QString AssigneeName(
		not_null<Main::Session*> session,
		const QString &uuid) {
	if (uuid.isEmpty()) {
		return tr::lng_mtslink_tasks_no_assignee(tr::now);
	}
	auto user = UserOf(session, uuid);
	if ((!user || user->name().isEmpty()) && uuid == SelfUuid(session)) {
		user = session->user();
	}
	return (user && !user->name().isEmpty())
		? chatListUserName(user)
		: tr::lng_mtslink_tasks_assignee_unknown(tr::now);
}

// The description is the markdown of MTS Link (as the text of the
// messages): shown formatted in the field, the formatting saved as it.
[[nodiscard]] TextWithTags DescriptionToField(
		not_null<Main::Session*> session,
		const QString &markdown) {
	if (markdown.isEmpty()) {
		return {};
	}
	const auto parsed = parseMentionedText(QString(), markdown, {}, session);
	return {
		parsed.text,
		TextUtilities::ConvertEntitiesToTextTags(parsed.entities),
	};
}

[[nodiscard]] QString FieldToDescription(
		not_null<Main::Session*> session,
		const TextWithTags &text) {
	return convertMentionsForSending(text, session).text.trimmed();
}

// "37 minutes ago", "yesterday at 12:00", "12 November at 12:00".
[[nodiscard]] QString RelativeTimeText(const QDateTime &value) {
	const auto local = value.toLocalTime();
	const auto now = QDateTime::currentDateTime();
	const auto seconds = local.secsTo(now);
	const auto time = QLocale().toString(local.time(), QLocale::ShortFormat);
	if (seconds < 60) {
		return tr::lng_mtslink_tasks_ago_now(tr::now);
	} else if (seconds < 3600) {
		return tr::lng_mtslink_tasks_ago_minutes(
			tr::now,
			lt_count,
			seconds / 60);
	} else if (local.date() == now.date()) {
		return tr::lng_mtslink_tasks_ago_hours(
			tr::now,
			lt_count,
			seconds / 3600);
	} else if (local.date() == now.date().addDays(-1)) {
		return tr::lng_mtslink_tasks_ago_yesterday(
			tr::now,
			lt_time,
			time);
	}
	return tr::lng_mtslink_tasks_ago_date(
		tr::now,
		lt_date,
		langDayOfMonthFull(local.date()),
		lt_time,
		time);
}

// The name, updated when the profile of the unknown person is loaded.
[[nodiscard]] rpl::producer<QString> PersonNameValue(
		not_null<Main::Session*> session,
		const QString &uuid) {
	const auto mts = session->account().mtsLinkSession();
	if (uuid.isEmpty() || !mts) {
		return rpl::single(AssigneeName(session, uuid));
	}
	const auto user = UserOf(session, uuid);
	if (!user || user->name().isEmpty()) {
		mts->users()->loadMember(uuid, mts->organizationId());
	}
	return rpl::single(
		rpl::empty_value()
	) | rpl::then(base::qt_signal_producer(
		mts->users(),
		&Api::Users::memberLoaded
	) | rpl::to_empty) | rpl::map([=] {
		return AssigneeName(session, uuid);
	}) | rpl::distinct_until_changed();
}

// The organization members by the name, the choice of one.
void ShowAssigneeBox(
		not_null<Window::SessionController*> controller,
		Fn<void(QString uuid)> chosen) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto session = &controller->session();
		box->setTitle(tr::lng_mtslink_tasks_assignee());
		box->setWidth(st::boxWideWidth);
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tr::lng_mtslink_tasks_assignee_search()));
		const auto list = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			style::margins());
		const auto choose = [=](QString uuid) {
			box->closeBox();
			chosen(uuid);
		};
		const auto show = [=](std::vector<not_null<UserData*>> users) {
			while (list->count() > 0) {
				delete list->widgetAt(0);
			}
			const auto add = [&](const QString &text, QString uuid) {
				const auto button = list->add(object_ptr<Ui::SettingsButton>(
					list,
					rpl::single(text),
					st::settingsButtonNoIcon));
				button->setClickedCallback([=] { choose(uuid); });
			};
			if (field->getLastText().trimmed().isEmpty()) {
				// Me first, by the full name.
				const auto self = SelfUuid(session);
				add(AssigneeName(session, self), self);
			}
			for (const auto &user : users) {
				const auto uuid = userBareIdToUuid(peerToUser(user->id).bare);
				if (!uuid.isEmpty()) {
					add(user->name(), uuid);
				}
			}
			list->resizeToWidth(list->width());
		};
		const auto generation = std::make_shared<int>(0);
		const auto search = [=] {
			const auto now = ++*generation;
			searchOrganizationMembers(
				session,
				field->getLastText().trimmed(),
				crl::guard(box, [=](std::vector<not_null<UserData*>> users) {
					if (*generation == now) {
						show(std::move(users));
					}
				}));
		};
		const auto timer = box->lifetime().make_state<base::Timer>(search);
		field->changes() | rpl::on_next([=] {
			timer->callOnce(kSearchDelay);
		}, field->lifetime());
		box->setFocusCallback([=] { field->setFocusFast(); });
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		show({});
		search();
	}));
}

} // namespace

QString taskDeadlineText(const QDateTime &deadline) {
	if (!deadline.isValid()) {
		return tr::lng_mtslink_tasks_no_deadline(tr::now);
	}
	const auto local = deadline.toLocalTime();
	const auto today = QDate::currentDate();
	const auto date = local.date();
	const auto time = QLocale().toString(local.time(), QLocale::ShortFormat);
	const auto day = (date == today)
		? tr::lng_mtslink_tasks_day_today(tr::now)
		: (date == today.addDays(1))
		? tr::lng_mtslink_tasks_day_tomorrow(tr::now)
		: (date == today.addDays(-1))
		? tr::lng_mtslink_tasks_day_yesterday(tr::now)
		: langDayOfMonthFull(date);
	return day + u", "_q + time;
}

void showTaskEditBox(
		not_null<Window::SessionController*> controller,
		std::optional<Api::Task> task,
		Api::TaskFields defaults) {
	const auto session = &controller->session();
	if (task) {
		defaults = Api::FieldsOf(*task);
	} else if (defaults.assigneeId.isEmpty()) {
		defaults.assigneeId = SelfUuid(session);
	}
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		struct State {
			rpl::variable<QDateTime> deadline;
			rpl::variable<QString> assignee;
			bool saving = false;
		};
		const auto state = box->lifetime().make_state<State>();
		state->deadline = defaults.deadline;
		state->assignee = defaults.assigneeId;

		box->setTitle(task
			? tr::lng_mtslink_tasks_edit_title()
			: tr::lng_mtslink_tasks_new_title());
		box->setWidth(st::boxWideWidth);

		const auto title = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::NoNewlines,
			tr::lng_mtslink_tasks_title_placeholder(),
			defaults.title));
		// The counter of the characters left, always shown; the excess is
		// red and not saved.
		Ui::AddLengthLimitLabel(title, kTitleLimit, {
			.customThreshold = kTitleLimit + 1,
		});
		Ui::AddSkip(box->verticalLayout());
		// Four lines at least, grows with the text up to fifteen.
		const auto descriptionSt = box->lifetime().make_state<
			style::InputField>(st::newGroupDescription);
		{
			const auto line = descriptionSt->style.font->height;
			const auto margins = descriptionSt->textMargins.top()
				+ descriptionSt->textMargins.bottom();
			descriptionSt->heightMin = margins + 4 * line;
			descriptionSt->heightMax = margins + 15 * line;
			descriptionSt->style = st::historyTextStyle;
		}
		// An existing description is shown as the text of a message, the
		// editor is shown by "Edit" (a new or an empty one: the editor).
		const auto showView = task.has_value()
			&& !defaults.description.isEmpty();
		const auto viewWrap = showView
			? box->addRow(object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				box,
				object_ptr<Ui::VerticalLayout>(box)))
			: nullptr;
		const auto editWrap = box->addRow(
			object_ptr<Ui::SlideWrap<Ui::InputField>>(
				box,
				object_ptr<Ui::InputField>(
					box,
					*descriptionSt,
					Ui::InputField::Mode::MultiLine,
					tr::lng_mtslink_tasks_description_placeholder())));
		const auto description = editWrap->entity();
		// The formatting as in the messages (the shortcuts, the menu).
		InitMessageFieldHandlers(
			controller,
			description,
			Window::GifPauseReason::Layer,
			[](not_null<DocumentData*>) { return true; });
		// Not a message: Enter is a new line (nothing is sent by it).
		description->setSubmitSettings(Ui::InputSubmitSettings::None);
		description->setTextWithTags(
			DescriptionToField(session, defaults.description));
		// The limit of the server is of the markdown (with the markers).
		Ui::AddLengthLimitLabel(description, kDescriptionLimit, {
			.customThreshold = kDescriptionLimit + 1,
			.customCharactersCount = [=] {
				return int(FieldToDescription(
					session,
					description->getTextWithAppliedMarkdown()).size());
			},
		});
		// The description of the view: the applied one (the saved one).
		const auto shownDescription = box->lifetime().make_state<QString>(
			defaults.description);
		const auto editing = box->lifetime().make_state<rpl::variable<bool>>(
			!viewWrap);
		if (viewWrap) {
			const auto view = viewWrap->entity();
			const auto labelSt = box->lifetime().make_state<
				style::FlatLabel>(st::boxLabel);
			labelSt->style = st::historyTextStyle;
			const auto label = view->add(object_ptr<Ui::FlatLabel>(
				view,
				*labelSt));
			label->setSelectable(true);
			const auto chatStyle = controller->chatStyle();
			label->setPreCache([=] {
				return chatStyle->messageStyle(false, false).preCache.get();
			});
			label->setBlockquoteCache([=] {
				return chatStyle->coloredQuoteCache(
					false,
					session->user()->colorIndex());
			});
			// The colors of the highlighted code, as in the messages.
			label->setColors(chatStyle->highlightColors());
			style::PaletteChanged(
			) | rpl::on_next([=] {
				label->setColors(chatStyle->highlightColors());
			}, label->lifetime());

			// The syntax of the code blocks is highlighted in the
			// background: the text is set again when it is ready.
			const auto highlightLifetime = label->lifetime().make_state<
				rpl::lifetime>();
			const auto show = [=](const QString &markdown) {
				const auto parsed = parseMentionedText(
					QString(),
					markdown,
					{},
					session);
				const auto apply = [=] {
					auto text = parsed;
					const auto processId = Spellchecker::TryHighlightSyntax(
						text);
					label->setMarkedText(text);
					return processId;
				};
				highlightLifetime->destroy();
				if (const auto processId = apply()) {
					Spellchecker::HighlightReady(
					) | rpl::filter(
						rpl::mappers::_1 == processId
					) | rpl::take(1) | rpl::on_next([=] {
						apply();
					}, *highlightLifetime);
				}
			};
			show(defaults.description);
			editWrap->hide(anim::type::instant);

			// "Edit" shows the editor, "Apply" shows the result again.
			const auto toggle = box->verticalLayout()->add(
				object_ptr<Ui::RoundButton>(
					box,
					editing->value() | rpl::map([](bool now) {
						return now
							? tr::lng_mtslink_tasks_apply_description()
							: tr::lng_mtslink_tasks_edit_description();
					}) | rpl::flatten_latest(),
					st::defaultLightButton),
				style::margins(
					st::boxRowPadding.left(),
					st::defaultVerticalListSkip,
					st::boxRowPadding.right(),
					0),
				style::al_left);
			toggle->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
			toggle->setClickedCallback([=] {
				if (editing->current()) {
					*shownDescription = FieldToDescription(
						session,
						description->getTextWithAppliedMarkdown());
					show(*shownDescription);
					editWrap->hide(anim::type::instant);
					viewWrap->show(anim::type::instant);
					*editing = false;
				} else {
					description->setTextWithTags(
						DescriptionToField(session, *shownDescription));
					viewWrap->hide(anim::type::instant);
					editWrap->show(anim::type::instant);
					description->setFocusFast();
					*editing = true;
				}
			});
		}
		Ui::AddSkip(box->verticalLayout());

		const auto container = box->verticalLayout();
		const auto deadline = Settings::AddButtonWithLabel(
			container,
			tr::lng_mtslink_tasks_deadline(),
			state->deadline.value() | rpl::map(taskDeadlineText),
			st::settingsButtonNoIcon);
		deadline->setClickedCallback([=] {
			const auto now = base::unixtime::now();
			const auto current = state->deadline.current().isValid()
				? TimeId(state->deadline.current().toSecsSinceEpoch())
				: (now + 24 * 3600);
			controller->show(Box([=](not_null<Ui::GenericBox*> dateBox) {
				Ui::ChooseDateTimeBox(dateBox, {
					.title = tr::lng_mtslink_tasks_deadline(),
					.submit = tr::lng_settings_save(),
					.done = [=](TimeId time) {
						state->deadline = QDateTime::fromSecsSinceEpoch(time);
						dateBox->closeBox();
					},
					.min = [] { return TimeId(0); },
					.time = current,
					.max = [=] {
						return base::unixtime::now() + 5 * 365 * 24 * 3600;
					},
				});
				dateBox->addLeftButton(
					tr::lng_mtslink_tasks_no_deadline(),
					[=] {
						state->deadline = QDateTime();
						dateBox->closeBox();
					});
			}));
		});

		const auto assignee = Settings::AddButtonWithLabel(
			container,
			tr::lng_mtslink_tasks_assignee(),
			state->assignee.value() | rpl::map([=](const QString &uuid) {
				return AssigneeName(session, uuid);
			}),
			st::settingsButtonNoIcon);
		assignee->setClickedCallback([=] {
			ShowAssigneeBox(controller, [=](QString uuid) {
				state->assignee = uuid;
			});
		});

		Ui::AddSkip(container);
		const auto important = box->addRow(object_ptr<Ui::Checkbox>(
			box,
			tr::lng_mtslink_tasks_important_mark(tr::now),
			defaults.important,
			st::defaultBoxCheckbox));
		if (task) {
			// Not editable: the text on the divider, as the notes of the
			// settings, under the fields (apart from them).
			Ui::AddSkip(container, st::defaultVerticalListSkip * 4);
			const auto url = task->messageUrl.isEmpty()
				? QString()
				: task->messageUrl.startsWith(u"http"_q)
				? task->messageUrl
				: (EnvConfig::instance().webinarHost()
					+ (task->messageUrl.startsWith(QChar('/'))
						? QString()
						: u"/"_q)
					+ task->messageUrl);
			const auto createdAt = task->createdAt;
			const auto updatedAt = task->updatedAt;
			const auto updatedBy = task->updatedBy.isEmpty()
				? task->authorId
				: task->updatedBy;
			const auto resolvedAt = task->resolvedAt;
			// Updated: not the creation itself (the same moment).
			const auto updated = updatedAt.isValid()
				&& (!createdAt.isValid()
					|| std::abs(createdAt.secsTo(updatedAt)) > 5);
			auto text = rpl::combine(
				PersonNameValue(session, task->authorId),
				PersonNameValue(session, updatedBy)
			) | rpl::map([=](const QString &author, const QString &editor) {
				// One paragraph of the sentences.
				auto result = TextWithEntities();
				const auto add = [&](TextWithEntities sentence) {
					if (!result.empty()) {
						result.append(QChar(' '));
					}
					result.append(std::move(sentence));
				};
				if (createdAt.isValid()) {
					add(tr::lng_mtslink_tasks_info_created(
						tr::now,
						lt_name,
						tr::marked(author),
						lt_when,
						tr::marked(RelativeTimeText(createdAt)),
						tr::marked));
				}
				if (updated) {
					add(tr::lng_mtslink_tasks_info_updated_by(
						tr::now,
						lt_name,
						tr::marked(editor),
						lt_when,
						tr::marked(RelativeTimeText(updatedAt)),
						tr::marked));
				}
				if (resolvedAt.isValid()) {
					add(tr::lng_mtslink_tasks_info_resolved(
						tr::now,
						lt_when,
						tr::marked(RelativeTimeText(resolvedAt)),
						tr::marked));
				}
				if (!url.isEmpty()) {
					add(tr::lng_mtslink_tasks_info_from_message(
						tr::now,
						lt_link,
						tr::link(
							tr::lng_mtslink_tasks_info_this_message(tr::now),
							1),
						tr::marked));
				}
				return result;
			});
			const auto label = Ui::AddDividerText(container, std::move(text));
			if (!url.isEmpty()) {
				// The message the task was created from: opened in the chat.
				label->setLink(1, std::make_shared<LambdaClickHandler>([=] {
					const auto weak = base::make_weak(controller);
					box->closeBox();
					handleMtsLinkUrl(url, QVariant::fromValue(
						ClickHandlerContext{ .sessionWindow = weak }));
				}));
			}
		}

		const auto fail = crl::guard(box, [=](QString) {
			state->saving = false;
			controller->showToast(tr::lng_mtslink_tasks_save_failed(tr::now));
		});
		const auto save = [=] {
			if (state->saving) {
				return;
			}
			auto fields = defaults;
			fields.title = title->getLastText().trimmed();
			// Not edited: the description as it is (not built again).
			fields.description = editing->current()
				? FieldToDescription(
					session,
					description->getTextWithAppliedMarkdown())
				: *shownDescription;
			fields.deadline = state->deadline.current();
			fields.assigneeId = state->assignee.current();
			fields.important = important->checked();
			if (fields.title.isEmpty() || fields.title.size() > kTitleLimit) {
				title->showError();
				return;
			} else if (fields.description.size() > kDescriptionLimit) {
				description->showError();
				return;
			}
			state->saving = true;
			const auto done = crl::guard(box, [=](Api::Task) {
				box->closeBox();
			});
			if (task) {
				Api::Tasks::Instance().update(task->id, fields, done, fail);
			} else {
				Api::Tasks::Instance().create(fields, done, fail);
			}
		};
		title->submits() | rpl::on_next(save, title->lifetime());
		box->setFocusCallback([=] { title->setFocusFast(); });

		box->addButton(
			task ? tr::lng_settings_save() : tr::lng_mtslink_tasks_create(),
			save);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		if (task) {
			const auto id = task->id;
			const auto resolved = task->resolved();
			box->addLeftButton(
				(resolved
					? tr::lng_mtslink_tasks_unresolve()
					: tr::lng_mtslink_tasks_resolve()),
				[=] {
					const auto close = crl::guard(box, [=] {
						box->closeBox();
					});
					Api::Tasks::Instance().setResolved(
						id,
						!resolved,
						[=](Api::Task) {
							// As the web client: the toast of the change.
							controller->showToast(resolved
								? tr::lng_mtslink_tasks_unresolved_toast(
									tr::now)
								: tr::lng_mtslink_tasks_resolved_toast(
									tr::now));
							close();
						},
						fail);
				});
			// The removal is in the menu of the title, as in the boxes of
			// Telegram (not a button next to "Save").
			const auto remove = [=] {
				confirmTaskRemove(
					controller,
					id,
					crl::guard(box, [=] { box->closeBox(); }));
			};
			const auto top = box->addTopButton(st::boxTitleMenu);
			const auto menu = top->lifetime().make_state<
				base::unique_qptr<Ui::PopupMenu>>();
			top->setClickedCallback([=] {
				*menu = base::make_unique_q<Ui::PopupMenu>(
					top,
					st::popupMenuWithIcons);
				const auto raw = menu->get();
				const auto addAction = Ui::Menu::CreateAddActionCallback(raw);
				addAction({
					.text = tr::lng_mtslink_tasks_delete(tr::now),
					.handler = remove,
					.icon = &st::menuIconDeleteAttention,
					.isAttention = true,
				});
				raw->setForcedOrigin(Ui::PanelAnimation::Origin::TopRight);
				top->setForceRippled(true);
				raw->setDestroyedCallback([=] {
					top->setForceRippled(false);
				});
				raw->popup(top->mapToGlobal(QPoint(
					top->width(),
					top->height() - st::lineWidth * 3)));
			});
		}
	}));
}

void confirmTaskRemove(
		not_null<Window::SessionController*> controller,
		const QString &id,
		Fn<void()> done) {
	controller->show(Ui::MakeConfirmBox({
		.text = tr::lng_mtslink_tasks_delete_sure(),
		.confirmed = [=](Fn<void()> close) {
			close();
			Api::Tasks::Instance().remove(id, done, [=](QString) {
				controller->showToast(
					tr::lng_mtslink_tasks_save_failed(tr::now));
			});
		},
		.confirmText = tr::lng_mtslink_tasks_delete(),
		.confirmStyle = &st::attentionBoxButton,
	}));
}

void addCreateTaskMenuAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item,
		bool inRepliesContext) {
	const auto itemId = item->fullId();
	const auto callback = [=] {
		if (const auto item = controller->session().data().message(itemId)) {
			showCreateTaskFromMessage(controller, item, inRepliesContext);
		}
	};
	const auto &actions = menu->actions();
	const auto after = [&](const QString &text) {
		for (auto i = 0; i != int(actions.size()); ++i) {
			if (actions[i]->text() == text) {
				return i + 1;
			}
		}
		return -1;
	};
	auto position = after(tr::lng_context_forward_msg(tr::now));
	if (position < 0) {
		position = after(tr::lng_context_reply_msg(tr::now));
	}
	if (position < 0) {
		position = int(actions.size()) / 2;
	}
	const auto text = tr::lng_mtslink_tasks_create_from_message(tr::now);
	const auto raw = menu->menu();
	menu->insertAction(position, base::make_unique_q<Ui::Menu::Action>(
		raw,
		menu->st().menu,
		Ui::Menu::CreateAction(raw, text, callback),
		&st::menuIconSchedule,
		&st::menuIconSchedule));
}

void showCreateTaskFromMessage(
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item,
		bool inRepliesContext) {
	// An album: the text is in one of its items.
	const auto textItem = item->history()->owner().groups().findItemToEdit(
		item);
	auto defaults = Api::TaskFields();
	// With the formatting of the message.
	const auto &original = textItem->originalText();
	defaults.description = FieldToDescription(&controller->session(), {
		original.text,
		TextUtilities::ConvertEntitiesToTextTags(original.entities),
	});
	// The link without the origin, as the web client sends it.
	auto url = buildMessageLink(item, inRepliesContext);
	const auto origin = EnvConfig::instance().webinarHost();
	if (!origin.isEmpty() && url.startsWith(origin)) {
		url = url.mid(origin.size());
	}
	defaults.messageUrl = url;
	showTaskEditBox(controller, std::nullopt, std::move(defaults));
}

} // namespace MtsLink

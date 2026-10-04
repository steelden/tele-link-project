/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.
*/
#include "mtslink/my_profile.h"

#include "data/data_user.h"

#include "mtslink/session.h"
#include "mtslink/rpc.h"
#include "mtslink/data_adapters.h"
#include "base/call_delayed.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/fields/input_field.h"
#include "window/window_session_controller.h"
#include "base/unixtime.h"
#include "chat_helpers/tabbed_panel.h"
#include "chat_helpers/tabbed_selector.h"
#include "ui/boxes/choose_date_time.h"
#include "ui/emoji_config.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_boxes.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_settings.h"
#include "styles/style_layers.h"

#include <QtCore/QBuffer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

namespace MtsLink {
namespace {

rpl::variable<CustomStatus> &MyStatusVariable();

rpl::variable<MyProfile> &MyProfileVariable() {
	static auto result = rpl::variable<MyProfile>();
	return result;
}

rpl::variable<std::vector<ProfileField>> &ProfileFieldsVariable() {
	static auto result = rpl::variable<std::vector<ProfileField>>();
	return result;
}

[[nodiscard]] bool Failed(const QJsonObject &result) {
	return result.value("type").toString().contains(
		u"error"_q,
		Qt::CaseInsensitive);
}

[[nodiscard]] MyProfile ParseProfile(
		const QJsonObject &profile,
		const QString &avatarFileId) {
	auto result = MyProfile{
		.firstName = profile.value("firstName").toString(),
		.lastName = profile.value("lastName").toString(),
		.patronymicName = profile.value("patronymicName").toString(),
		.displayName = profile.value("displayName").toString(),
		.department = profile.value("department").toString(),
		.position = profile.value("position").toString(),
		.phone = profile.value("phone").toString(),
		.timeZone = profile.value("timeZone").toString(),
		.avatarFileId = profile.value("avatarFileId").toString(
			avatarFileId),
		.loaded = true,
	};
	for (const auto &field : profile.value("additionalFields").toArray()) {
		const auto obj = field.toObject();
		result.additional.emplace_back(
			obj.value("profileFieldId").toString(),
			obj.value("value").toString());
	}
	return result;
}

void SaveWithRetry(
		not_null<Main::Session*> session,
		MyProfile profile,
		Fn<void(bool)> done,
		int attempt) {
	// A just uploaded avatar is processed for a few seconds, as with the
	// chat covers the request fails before that.
	constexpr auto kDelays = std::array<crl::time, 6>{
		1500, 2000, 3000, 4000, 6000, 8000 };
	const auto mts = session->account().mtsLinkSession();
	if (!mts) {
		if (done) {
			done(false);
		}
		return;
	}
	auto additional = QJsonArray();
	for (const auto &[id, value] : profile.additional) {
		additional.push_back(QJsonObject{
			{ "profileFieldId", id },
			{ "value", value },
		});
	}
	const auto param = QJsonObject{
		{ "organizationId", mts->organizationId() },
		{ "firstName", profile.firstName },
		{ "lastName", profile.lastName },
		{ "patronymicName", profile.patronymicName },
		{ "displayName", profile.displayName },
		{ "department", profile.department },
		{ "position", profile.position },
		{ "phone", profile.phone },
		{ "additionalFields", additional },
		{ "timeZone", profile.timeZone },
		{ "avatarFileId", profile.avatarFileId },
	};
	const auto weak = base::make_weak(session);
	mts->rpc()->call(
		"Organization.UpdateMemberProfileV5",
		param,
		[=](const QJsonObject &result) {
			const auto failed = Failed(result);
			LOG(("MtsLink Profile: update attempt %1 -> %2"
				).arg(attempt + 1
				).arg(failed
					? QString::fromUtf8(QJsonDocument(result).toJson(
						QJsonDocument::Compact)).left(300)
					: u"ok"_q));
			if (!weak) {
				return;
			} else if (!failed) {
				MyProfileVariable() = profile;
				if (done) {
					done(true);
				}
			} else if (attempt < int(kDelays.size())
				&& result.value("type").toString() == u"RpcError"_q) {
				base::call_delayed(kDelays[attempt], [=] {
					if (weak) {
						SaveWithRetry(weak.get(), profile, done, attempt + 1);
					}
				});
			} else if (done) {
				done(false);
			}
		},
		[=](const QString &error) {
			LOG(("MtsLink Profile: update failed: %1").arg(error));
			if (done) {
				done(false);
			}
		});
}

} // namespace

QString MyProfile::additionalValue(const QString &fieldId) const {
	for (const auto &[id, value] : additional) {
		if (id == fieldId) {
			return value;
		}
	}
	return QString();
}

void MyProfile::setAdditionalValue(
		const QString &fieldId,
		const QString &value) {
	for (auto &[id, existing] : additional) {
		if (id == fieldId) {
			existing = value;
			return;
		}
	}
	additional.emplace_back(fieldId, value);
}

void loadMyProfile(not_null<Main::Session*> session) {
	const auto mts = session->account().mtsLinkSession();
	if (!mts) {
		return;
	}
	const auto organizationId = mts->organizationId();
	const auto userId = mts->userId();
	const auto weak = base::make_weak(session);
	// GetMemberV2 has the full profile, but not the avatar id.
	mts->rpc()->call(
		"Member.GetMember",
		QJsonObject{
			{ "organizationId", organizationId },
			{ "userId", userId },
		},
		[=](const QJsonObject &member) {
			const auto avatarFileId = member.value("value").toObject()
				.value("profile").toObject()
				.value("avatarFileId").toString();
			const auto mts = weak
				? weak->account().mtsLinkSession()
				: nullptr;
			if (!mts) {
				return;
			}
			mts->rpc()->call(
				"Organization.GetMemberV2",
				QJsonObject{
					{ "organizationId", organizationId },
					{ "userId", userId },
				},
				[=](const QJsonObject &result) {
					if (Failed(result)) {
						return;
					}
					const auto value = result.value("value").toObject();
					MyProfileVariable() = ParseProfile(
						value.value("profile").toObject(),
						avatarFileId);
					MyStatusVariable() = ParseCustomStatus(
						value.value("customStatus").toObject());
					if (weak) {
						applyUserStatus(
							weak.get(),
							userId,
							value.value("customStatus").toObject());
					}
					LOG(("MtsLink Profile: loaded, avatar=%1"
						).arg(avatarFileId));
					if (weak) {
						// The own userpic may be restored from the local
						// storage, the viewer needs its full size photo.
						ensureUserpicFor(weak->user(), avatarFileId);
					}
				});
		});
	mts->rpc()->call(
		"Organization.GetProfileFields",
		QJsonObject{ { "organizationId", organizationId } },
		[=](const QJsonObject &result) {
			auto fields = std::vector<ProfileField>();
			const auto value = result.value("value").toObject();
			for (const auto group : { "personal", "contacts", "additional" }) {
				for (const auto &field : value.value(group).toArray()) {
					const auto obj = field.toObject();
					if (obj.value("type").toString() != u"Text"_q) {
						continue;
					}
					fields.push_back({
						.id = obj.value("id").toString(),
						.title = obj.value("title").toString(),
					});
				}
			}
			ProfileFieldsVariable() = std::move(fields);
		});
}

rpl::producer<MyProfile> myProfileValue() {
	return MyProfileVariable().value();
}

rpl::producer<std::vector<ProfileField>> profileFieldsValue() {
	return ProfileFieldsVariable().value();
}

void saveMyProfile(
		not_null<Main::Session*> session,
		MyProfile profile,
		Fn<void(bool)> done) {
	if (!profile.loaded) {
		// Without the loaded profile the other fields would be erased.
		LOG(("MtsLink Profile: not loaded, can't save"));
		if (done) {
			done(false);
		}
		return;
	}
	SaveWithRetry(session, std::move(profile), std::move(done), 0);
}

void setMyAvatar(not_null<Main::Session*> session, QImage image) {
	const auto mts = session->account().mtsLinkSession();
	if (!mts || image.isNull()) {
		return;
	}
	auto bytes = QByteArray();
	QBuffer buffer(&bytes);
	buffer.open(QIODevice::WriteOnly);
	image.save(&buffer, "PNG");
	const auto weak = base::make_weak(session);
	mts->files()->uploadAvatar(
		u"Avatar.png"_q,
		bytes,
		u"image/png"_q,
		[=](const Api::UploadResult &uploaded) {
			if (!weak) {
				return;
			}
			auto profile = MyProfileVariable().current();
			profile.avatarFileId = uploaded.id;
			LOG(("MtsLink Profile: avatar %1 uploaded").arg(uploaded.id));
			saveMyProfile(weak.get(), profile, [=](bool ok) {
				if (!ok && weak && !weak->windows().empty()) {
					weak->windows().front()->showToast(
						tr::lng_cant_do_this(tr::now));
				}
			});
		},
		[=](const QString &error) {
			LOG(("MtsLink Profile: avatar upload failed: %1").arg(error));
		});
}

void applyMyProfileChanged(
		not_null<Main::Session*> session,
		const QJsonObject &profile) {
	auto parsed = ParseProfile(
		profile,
		MyProfileVariable().current().avatarFileId);
	MyProfileVariable() = std::move(parsed);
}

void EditProfileFieldsBox(
		not_null<Ui::GenericBox*> box,
		EditProfileFieldsArgs args) {
	box->setTitle(rpl::single(args.title));
	auto fields = std::vector<not_null<Ui::InputField*>>();
	for (const auto &[label, value] : args.fields) {
		fields.push_back(box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(label),
			value)));
	}
	box->setFocusCallback([=] {
		if (!fields.empty()) {
			fields.front()->setFocusFast();
		}
	});
	const auto save = args.save;
	const auto submit = [=] {
		auto values = std::vector<QString>();
		for (const auto &field : fields) {
			values.push_back(field->getLastText().trimmed());
		}
		save(std::move(values), crl::guard(box, [=] { box->closeBox(); }));
	};
	for (const auto &field : fields) {
		field->submits() | rpl::on_next(submit, field->lifetime());
	}
	box->addButton(tr::lng_settings_save(), submit);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

namespace {

rpl::variable<CustomStatus> &MyStatusVariable() {
	static auto result = rpl::variable<CustomStatus>();
	return result;
}

struct StatusHistoryItem {
	QString id;
	CustomStatus status;
};

[[nodiscard]] TimeId EndOfToday() {
	const auto now = QDateTime::currentDateTime();
	return TimeId(QDateTime(now.date(), QTime(23, 59, 59)).toSecsSinceEpoch());
}

// The expiration for a setting chosen now, 0 for no expiration.
[[nodiscard]] TimeId ExpiresFor(const QString &setting, TimeId custom) {
	if (setting == u"OneHour"_q) {
		return base::unixtime::now() + 3600;
	} else if (setting == u"Today"_q) {
		return EndOfToday();
	} else if (setting == u"Custom"_q) {
		return custom;
	}
	return 0;
}

[[nodiscard]] QString SettingText(const QString &setting) {
	return (setting == u"OneHour"_q)
		? tr::lng_mtslink_status_hour(tr::now)
		: (setting == u"Today"_q)
		? tr::lng_mtslink_status_today(tr::now)
		: (setting == u"Custom"_q)
		? tr::lng_mtslink_status_custom(tr::now)
		: tr::lng_mtslink_status_forever(tr::now);
}

[[nodiscard]] QString ExpiresText(TimeId expiresAt) {
	if (!expiresAt) {
		return QString();
	}
	const auto when = base::unixtime::parse(expiresAt);
	const auto time = QLocale().toString(when.time(), QLocale::ShortFormat);
	return (when.date() == QDate::currentDate())
		? tr::lng_mtslink_status_until(tr::now, lt_date, time)
		: tr::lng_mtslink_status_until(
			tr::now,
			lt_date,
			QLocale().toString(when.date(), QLocale::ShortFormat)
				+ ' ' + time);
}

void SendStatus(
		not_null<Main::Session*> session,
		const CustomStatus &status,
		Fn<void(bool)> done) {
	const auto mts = session->account().mtsLinkSession();
	if (!mts) {
		done(false);
		return;
	}
	// An empty request clears the status.
	auto param = QJsonObject{ { "organizationId", mts->organizationId() } };
	if (!status.empty()) {
		param.insert("emoji", status.emojiId);
		param.insert("status", status.text);
		param.insert("setting", status.setting);
		if (status.expiresAt) {
			param.insert("expiresAt", double(status.expiresAt));
		}
	}
	LOG(("MtsLink Status: set emoji=%1 text='%2' setting=%3 expires=%4"
		).arg(status.emojiId, status.text, status.setting
		).arg(status.expiresAt));
	mts->rpc()->call(
		"Organization.SetCustomStatus",
		param,
		[=](const QJsonObject &result) {
			const auto ok = !Failed(result);
			LOG(("MtsLink Status: set -> %1").arg(ok ? "ok" : "fail"));
			if (ok) {
				MyStatusVariable() = status;
				auto object = QJsonObject();
				if (!status.empty()) {
					object.insert("emoji", status.emojiId);
					object.insert("expiresAt", double(status.expiresAt));
				}
				applyUserStatus(session, mts->userId(), object);
			}
			done(ok);
		},
		[=](const QString &) { done(false); });
}

void LoadHistory(
		not_null<Main::Session*> session,
		Fn<void(std::vector<StatusHistoryItem>)> done) {
	const auto mts = session->account().mtsLinkSession();
	if (!mts) {
		return;
	}
	mts->rpc()->call(
		"Organization.GetCustomStatusHistory",
		QJsonObject{ { "organizationId", mts->organizationId() } },
		[=](const QJsonObject &result) {
			auto list = std::vector<StatusHistoryItem>();
			const auto items = result.value("value").toObject()
				.value("items").toArray();
			for (const auto &item : items) {
				const auto obj = item.toObject();
				list.push_back({
					.id = obj.value("customStatusHistoryId").toString(),
					.status = ParseCustomStatus(obj),
				});
			}
			done(std::move(list));
		});
}

void DeleteHistory(
		not_null<Main::Session*> session,
		const QString &id,
		Fn<void()> done) {
	const auto mts = session->account().mtsLinkSession();
	if (!mts) {
		return;
	}
	mts->rpc()->call(
		"Organization.DeleteCustomStatusHistory",
		QJsonObject{
			{ "organizationId", mts->organizationId() },
			{ "customStatusHistoryId", id },
		},
		[=](const QJsonObject &result) {
			LOG(("MtsLink Status: delete history %1 -> %2"
				).arg(id).arg(Failed(result) ? "fail" : "ok"));
			done();
		});
}

} // namespace

CustomStatus ParseCustomStatus(const QJsonObject &object) {
	auto result = CustomStatus{
		.emojiId = object.value("emoji").toString(),
		.text = object.value("status").toString(),
		.setting = object.value("setting").toString(u"Constant"_q),
		.expiresAt = TimeId(object.value("expiresAt").toDouble()),
	};
	if (result.expiresAt && result.expiresAt <= base::unixtime::now()) {
		return CustomStatus();
	}
	return result;
}

QString CustomStatusText(const CustomStatus &status) {
	const auto emoji = idToEmoji(status.emojiId);
	return emoji.isEmpty()
		? status.text
		: status.text.isEmpty()
		? emoji
		: (emoji + ' ' + status.text);
}

QString CustomStatusEmoji(const CustomStatus &status) {
	return idToEmoji(status.emojiId);
}

QString CustomStatusUntilText(const CustomStatus &status) {
	return status.expiresAt
		? ExpiresText(status.expiresAt)
		: tr::lng_mtslink_status_no_end(tr::now);
}

rpl::producer<CustomStatus> myStatusValue() {
	return MyStatusVariable().value();
}

void applyMyStatusChanged(const QJsonObject &value) {
	MyStatusVariable() = ParseCustomStatus(value);
}

void SetStatusBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	box->setTitle(tr::lng_mtslink_status_title());
	box->setWidth(st::boxWideWidth);

	const auto settings = std::array<QString, 4>{
		u"Constant"_q, u"OneHour"_q, u"Today"_q, u"Custom"_q };
	const auto settingIndex = [=](const QString &setting) {
		for (auto i = 0; i != int(settings.size()); ++i) {
			if (settings[i] == setting) {
				return i;
			}
		}
		return 0;
	};
	struct State {
		QString emoji;
		TimeId customExpires = 0;
		std::shared_ptr<Ui::RadioenumGroup<int>> duration;
		rpl::variable<QString> customLabel;
		Ui::SettingsButton *selected = nullptr;
		Fn<void(const CustomStatus&)> fill;
		bool filling = false;
	};
	const auto state = box->lifetime().make_state<State>();
	state->duration = std::make_shared<Ui::RadioenumGroup<int>>(0);
	state->customLabel = tr::lng_mtslink_status_custom(tr::now);

	const auto close = crl::guard(box, [=] { box->closeBox(); });
	const auto apply = [=](CustomStatus status, bool closeAfter) {
		SendStatus(session, status, crl::guard(box, [=](bool ok) {
			if (!ok) {
				controller->showToast(tr::lng_cant_do_this(tr::now));
			} else if (closeAfter) {
				box->closeBox();
			}
		}));
	};
	const auto content = box->verticalLayout();

	// The current status with the remove button right after its text.
	const auto current = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)));
	{
		const auto inner = current->entity();
		const auto row = inner->add(
			object_ptr<Ui::RpWidget>(inner),
			st::boxRowPadding);
		const auto label = Ui::CreateChild<Ui::FlatLabel>(
			row,
			myStatusValue() | rpl::map([](const CustomStatus &status) {
				const auto until = ExpiresText(status.expiresAt);
				return CustomStatusText(status)
					+ (until.isEmpty() ? QString() : (u", "_q + until));
			}),
			st::boxLabel);
		const auto remove = Ui::CreateChild<Ui::IconButton>(
			row,
			st::stickersRemove);
		remove->setClickedCallback([=] {
			apply(CustomStatus(), false);
		});
		rpl::combine(
			row->widthValue(),
			label->naturalWidthValue()
		) | rpl::on_next([=](int width, int natural) {
			const auto available = width - remove->width();
			label->resizeToWidth(std::min(natural, available));
			const auto height = std::max(label->height(), remove->height());
			row->resize(width, height);
			label->moveToLeft(0, (height - label->height()) / 2, width);
			remove->moveToLeft(
				label->width(),
				(height - remove->height()) / 2,
				width);
		}, row->lifetime());
		Ui::AddSkip(inner);
		Ui::AddDivider(inner);
		Ui::AddSkip(inner);
		current->toggleOn(myStatusValue() | rpl::map([](const CustomStatus &s) {
			return !s.empty();
		}), anim::type::instant);
	}

	// A new status: the emoji button and the text.
	const auto row = box->addRow(object_ptr<Ui::RpWidget>(box));
	const auto emojiButton = Ui::CreateChild<Ui::RoundButton>(
		row,
		rpl::single(QString()),
		st::defaultLightButton);
	const auto text = Ui::CreateChild<Ui::InputField>(
		row,
		st::defaultInputField,
		tr::lng_mtslink_status_text());
	text->setMaxLength(100);
	const auto refreshEmoji = [=] {
		emojiButton->setText(rpl::single(state->emoji.isEmpty()
			? tr::lng_mtslink_status_emoji(tr::now)
			: state->emoji));
	};
	refreshEmoji();
	row->resize(row->width(), text->height());
	row->widthValue() | rpl::on_next([=](int width) {
		emojiButton->moveToLeft(0, (text->height() - emojiButton->height()) / 2);
		const auto left = emojiButton->width() + st::boxLittleSkip;
		text->resize(width - left, text->height());
		text->moveToLeft(left, 0);
	}, row->lifetime());

	// The emoji panel without premium emoji, as for the chat titles.
	{
		const auto container = box->getDelegate()->outerContainer();
		using Selector = ChatHelpers::TabbedSelector;
		const auto panel = box->lifetime().make_state<
			base::unique_qptr<ChatHelpers::TabbedPanel>>(
				base::make_unique_q<ChatHelpers::TabbedPanel>(
					container,
					ChatHelpers::TabbedPanelDescriptor{
						.ownedSelector = object_ptr<Selector>(
							nullptr,
							ChatHelpers::TabbedSelectorDescriptor{
								.show = controller->uiShow(),
								.st = st::defaultComposeControls.tabbed,
								.level = Window::GifPauseReason::Layer,
								.mode = Selector::Mode::PeerTitle,
							}),
					}))->get();
		panel->setDesiredHeightValues(
			1.,
			st::emojiPanMinHeight / 2,
			st::emojiPanMinHeight);
		panel->hide();
		panel->setDropDown(true);
		panel->selector()->emojiChosen(
		) | rpl::on_next([=](ChatHelpers::EmojiChosen data) {
			const auto emoji = data.emoji->text();
			if (emojiToId(emoji).isEmpty()) {
				controller->showToast(tr::lng_mtslink_status_no_emoji(tr::now));
				return;
			}
			state->emoji = emoji;
			refreshEmoji();
			panel->hideAnimated();
			text->setFocus();
		}, box->lifetime());
		emojiButton->setClickedCallback([=] {
			const auto global = emojiButton->mapToGlobal(
				QPoint(0, emojiButton->height()));
			const auto local = panel->parentWidget()->mapFromGlobal(global);
			panel->moveTopRight(local.y(), local.x() + panel->width());
			panel->toggleAnimated();
		});
	}

	// The duration.
	Ui::AddSkip(content);
	for (auto i = 0; i != int(settings.size()); ++i) {
		const auto radio = box->addRow(
			object_ptr<Ui::Radioenum<int>>(
				box,
				state->duration,
				i,
				SettingText(settings[i]),
				st::defaultBoxCheckbox),
			st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
		if (settings[i] == u"Custom"_q) {
			state->customLabel.value() | rpl::on_next([=](const QString &label) {
				radio->setText(label);
			}, radio->lifetime());
		}
	}
	const auto setCustomExpires = [=](TimeId time) {
		state->customExpires = time;
		state->customLabel = time
			? ExpiresText(time)
			: tr::lng_mtslink_status_custom(tr::now);
	};
	state->duration->setChangedCallback([=](int index) {
		if (settings[index] != u"Custom"_q || state->filling) {
			// Filling the controls shows the date, no picker for it.
			return;
		}
		const auto now = base::unixtime::now();
		controller->show(Box([=](not_null<Ui::GenericBox*> picker) {
			Ui::ChooseDateTimeBox(picker, {
				.title = tr::lng_mtslink_status_custom(),
				.submit = tr::lng_settings_save(),
				.done = crl::guard(box, [=](TimeId time) {
					setCustomExpires(time);
					picker->closeBox();
				}),
				.min = [=] { return base::unixtime::now() + 60; },
				.time = state->customExpires
					? state->customExpires
					: (now + 3600),
				.max = [=] { return base::unixtime::now() + 366 * 86400; },
			});
		}));
	});

	// Puts a status to the controls for editing.
	state->fill = [=](const CustomStatus &status) {
		state->emoji = idToEmoji(status.emojiId);
		refreshEmoji();
		text->setText(status.text);
		text->setCursorPosition(status.text.size());
		const auto index = settingIndex(status.setting);
		if (settings[index] == u"Custom"_q) {
			// Not opening the date picker, just showing the date.
			setCustomExpires(status.expiresAt);
		}
		state->filling = true;
		state->duration->setValue(index);
		state->filling = false;
	};

	// Recent statuses: a click puts one to the controls, the cross
	// removes it from the history.
	Ui::AddSkip(content);
	Ui::AddDivider(content);
	Ui::AddSubsectionTitle(content, tr::lng_mtslink_status_recent());
	const auto history = content->add(object_ptr<Ui::VerticalLayout>(content));
	const auto fillHistory = std::make_shared<Fn<void()>>();
	*fillHistory = [=] {
		LoadHistory(session, crl::guard(box, [=](
				std::vector<StatusHistoryItem> list) {
			state->selected = nullptr;
			while (history->count()) {
				delete history->widgetAt(0);
			}
			for (const auto &item : list) {
				const auto status = item.status;
				const auto id = item.id;
				const auto button = history->add(
					object_ptr<Ui::SettingsButton>(
						history,
						rpl::single(CustomStatusText(status)
							+ u" · "_q
							+ SettingText(status.setting)),
						st::settingsButtonNoIcon));
				button->setClickedCallback([=] {
					if (state->selected) {
						state->selected->setColorOverride(std::nullopt);
					}
					state->selected = button;
					button->setColorOverride(st::windowActiveTextFg->c);
					auto edited = status;
					if (edited.setting != u"Custom"_q) {
						edited.expiresAt = 0;
					}
					state->fill(edited);
				});
				const auto remove = Ui::CreateChild<Ui::IconButton>(
					button,
					st::stickersRemove);
				button->widthValue() | rpl::on_next([=](int width) {
					remove->moveToRight(
						0,
						(button->height() - remove->height()) / 2,
						width);
				}, remove->lifetime());
				remove->setClickedCallback([=] {
					DeleteHistory(session, id, crl::guard(box, [=] {
						(*fillHistory)();
					}));
				});
			}
			history->resizeToWidth(content->width());
		}));
	};
	(*fillHistory)();

	// The controls start with the current status.
	if (const auto now = MyStatusVariable().current(); !now.empty()) {
		state->fill(now);
	}

	box->setFocusCallback([=] { text->setFocusFast(); });
	const auto save = [=] {
		const auto emojiId = emojiToId(state->emoji);
		const auto value = text->getLastText().trimmed();
		if (emojiId.isEmpty() && value.isEmpty()) {
			text->showError();
			return;
		}
		const auto setting = settings[state->duration->current()];
		if (setting == u"Custom"_q && !state->customExpires) {
			controller->showToast(tr::lng_mtslink_status_custom(tr::now));
			return;
		}
		apply(CustomStatus{
			.emojiId = emojiId,
			.text = value,
			.setting = setting,
			.expiresAt = ExpiresFor(setting, state->customExpires),
		}, true);
	};
	text->submits() | rpl::on_next(save, text->lifetime());
	box->addButton(tr::lng_settings_save(), save);
	box->addButton(tr::lng_cancel(), close);
}

} // namespace MtsLink

/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.

The own MTS Link profile: fields, avatar and the custom status.
*/
#pragma once

#include <QtCore/QJsonObject>
#include <QtGui/QImage>

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace MtsLink {

// Organization.GetMemberV2 profile, UpdateMemberProfileV5 sends it all.
struct MyProfile {
	QString firstName;
	QString lastName;
	QString patronymicName;
	QString displayName;
	QString department;
	QString position;
	QString phone;
	QString timeZone;
	QString avatarFileId;
	std::vector<std::pair<QString, QString>> additional; // fieldId, value.
	bool loaded = false;

	[[nodiscard]] QString additionalValue(const QString &fieldId) const;
	friend inline bool operator==(
		const MyProfile &,
		const MyProfile &) = default;
	void setAdditionalValue(const QString &fieldId, const QString &value);
};

// Organization.GetProfileFields, the "additional" group.
struct ProfileField {
	QString id;
	QString title;

	friend inline bool operator==(
		const ProfileField &,
		const ProfileField &) = default;
};

// Requests the profile, the fields and the avatar id.
void loadMyProfile(not_null<Main::Session*> session);
[[nodiscard]] rpl::producer<MyProfile> myProfileValue();
[[nodiscard]] rpl::producer<std::vector<ProfileField>> profileFieldsValue();

// done(ok) after the server answer, the result comes with
// MemberProfileChanged too.
void saveMyProfile(
	not_null<Main::Session*> session,
	MyProfile profile,
	Fn<void(bool)> done = nullptr);
void setMyAvatar(not_null<Main::Session*> session, QImage image);

// OrganizationEvent "MemberProfileChanged" for me.
void applyMyProfileChanged(
	not_null<Main::Session*> session,
	const QJsonObject &profile);

// An edit box: labels with the current values, save(values).
struct EditProfileFieldsArgs {
	QString title;
	std::vector<std::pair<QString, QString>> fields; // label, value.
	Fn<void(std::vector<QString> values, Fn<void()> close)> save;
};
void EditProfileFieldsBox(
	not_null<Ui::GenericBox*> box,
	EditProfileFieldsArgs args);

// Organization custom status: emoji id from the MTS Link catalogue, text
// and the duration setting: Constant, OneHour, Today or Custom.
struct CustomStatus {
	QString emojiId;
	QString text;
	QString setting;
	TimeId expiresAt = 0;

	[[nodiscard]] bool empty() const {
		return emojiId.isEmpty() && text.isEmpty();
	}
	friend inline bool operator==(
		const CustomStatus &,
		const CustomStatus &) = default;
};

[[nodiscard]] CustomStatus ParseCustomStatus(const QJsonObject &object);
// "<emoji> <text>".
[[nodiscard]] QString CustomStatusText(const CustomStatus &status);
[[nodiscard]] rpl::producer<CustomStatus> myStatusValue();

// OrganizationEvent "CustomStatusChanged" for me.
void applyMyStatusChanged(const QJsonObject &value);

// "<emoji> " for the label, the emoji is not painted as a link.
[[nodiscard]] QString CustomStatusEmoji(const CustomStatus &status);
// "until <date>" or "no end".
[[nodiscard]] QString CustomStatusUntilText(const CustomStatus &status);

// "Set status": the current one, recent ones and a new one.
void SetStatusBox(
	not_null<Ui::GenericBox*> box,
	not_null<Window::SessionController*> controller);

} // namespace MtsLink

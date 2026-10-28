/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_wall_paper.h"

#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "data/data_session.h"
#include "storage/serialize_common.h"
#include "ui/chat/chat_theme.h"
#include "ui/image/image.h"
#include "ui/image/image_prepare.h"
#include "settings.h"
#include "base/zlib_help.h"

#include <QtCore/QRegularExpression>
#include "main/main_account.h"
#include "storage/storage_account.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include "ui/color_int_conversion.h"
#include "core/application.h"
#include "main/main_session.h"

namespace Ui {

QColor ColorFromSerialized(MTPint serialized) {
	return ColorFromSerialized(serialized.v);
}

std::optional<QColor> MaybeColorFromSerialized(
		const tl::conditional<MTPint> &mtp) {
	return mtp ? ColorFromSerialized(*mtp) : std::optional<QColor>();
}

} // namespace Ui

namespace Data {
namespace {

constexpr auto FromLegacyBackgroundId(int32 legacyId) -> WallPaperId {
	return uint64(0xFFFFFFFF00000000ULL) | uint64(uint32(legacyId));
}

constexpr auto kUninitializedBackground = FromLegacyBackgroundId(-999);
constexpr auto kTestingThemeBackground = FromLegacyBackgroundId(-666);
constexpr auto kTestingDefaultBackground = FromLegacyBackgroundId(-665);
constexpr auto kTestingEditorBackground = FromLegacyBackgroundId(-664);
constexpr auto kThemeBackground = FromLegacyBackgroundId(-2);
constexpr auto kCustomBackground = FromLegacyBackgroundId(-1);
constexpr auto kLegacy1DefaultBackground = FromLegacyBackgroundId(0);
constexpr auto kLegacy2DefaultBackground = 5947530738516623361;
constexpr auto kLegacy3DefaultBackground = 5778236420632084488;
constexpr auto kLegacy4DefaultBackground = 5945087215657811969;
constexpr auto kDefaultBackground = 5933856211186221059;
constexpr auto kIncorrectDefaultBackground = FromLegacyBackgroundId(105);

constexpr auto kVersionTag = qint32(0x7FFFFFFF);
constexpr auto kVersion = 1;

using Ui::MaybeColorFromSerialized;

[[nodiscard]] quint32 SerializeColor(const QColor &color) {
	return (quint32(std::clamp(color.red(), 0, 255)) << 16)
		| (quint32(std::clamp(color.green(), 0, 255)) << 8)
		| quint32(std::clamp(color.blue(), 0, 255));
}

[[nodiscard]] quint32 SerializeMaybeColor(std::optional<QColor> color) {
	return color ? SerializeColor(*color) : quint32(-1);
}

[[nodiscard]] std::vector<QColor> ColorsFromMTP(
		const MTPDwallPaperSettings &data) {
	auto result = std::vector<QColor>();
	const auto c1 = MaybeColorFromSerialized(data.vbackground_color());
	if (!c1) {
		return result;
	}
	result.reserve(4);
	result.push_back(*c1);
	const auto c2 = MaybeColorFromSerialized(
		data.vsecond_background_color());
	if (!c2) {
		return result;
	}
	result.push_back(*c2);
	const auto c3 = MaybeColorFromSerialized(data.vthird_background_color());
	if (!c3) {
		return result;
	}
	result.push_back(*c3);
	const auto c4 = MaybeColorFromSerialized(
		data.vfourth_background_color());
	if (!c4) {
		return result;
	}
	result.push_back(*c4);
	return result;
}

[[nodiscard]] std::optional<QColor> ColorFromString(QStringView string) {
	if (string.size() != 6) {
		return {};
	} else if (ranges::any_of(string, [](QChar ch) {
		return (ch < 'a' || ch > 'f')
			&& (ch < 'A' || ch > 'F')
			&& (ch < '0' || ch > '9');
	})) {
		return {};
	}
	const auto component = [](QStringView text, int index) {
		const auto decimal = [](QChar hex) {
			const auto code = hex.unicode();
			return (code >= '0' && code <= '9')
				? int(code - '0')
				: (code >= 'a' && code <= 'f')
				? int(code - 'a' + 0x0a)
				: int(code - 'A' + 0x0a);
		};
		index *= 2;
		return decimal(text[index]) * 0x10 + decimal(text[index + 1]);
	};
	return QColor(
		component(string, 0),
		component(string, 1),
		component(string, 2),
		255);
}

[[nodiscard]] std::vector<QColor> ColorsFromString(const QString &string) {
	constexpr auto kMaxColors = 4;
	const auto view = QStringView(string);
	const auto count = int(view.size() / 6);
	if (!count || count > kMaxColors || view.size() != count * 7 - 1) {
		return {};
	}
	auto result = std::vector<QColor>();
	result.reserve(count);
	for (auto i = 0; i != count; ++i) {
		if (i + 1 < count
			&& view[i * 7 + 6] != '~'
			&& (count > 2 || view[i * 7 + 6] != '-')) {
			return {};
		} else if (const auto parsed = ColorFromString(view.mid(i * 7, 6))) {
			result.push_back(*parsed);
		} else {
			return {};
		}
	}
	return result;
}

[[nodiscard]] QString StringFromColor(QColor color) {
	const auto component = [](int value) {
		const auto hex = [](int value) {
			value = std::clamp(value, 0, 15);
			return (value > 9)
				? QChar('a' + (value - 10))
				: QChar('0' + value);
		};
		return QString() + hex(value / 16) + hex(value % 16);
	};
	return component(color.red())
		+ component(color.green())
		+ component(color.blue());
}

[[nodiscard]] QString StringFromColors(const std::vector<QColor> &colors) {
	Expects(!colors.empty());

	auto strings = QStringList();
	strings.reserve(colors.size());
	for (const auto &color : colors) {
		strings.push_back(StringFromColor(color));
	}
	const auto separator = (colors.size() > 2) ? '~' : '-';
	return strings.join(separator);
}

[[nodiscard]] qint32 RawFromLegacyFlags(qint32 legacyFlags) {
	using Flag = WallPaperFlag;
	return ((legacyFlags & (1 << 0)) ? qint32(Flag::Creator) : 0)
		| ((legacyFlags & (1 << 1)) ? qint32(Flag::Default) : 0)
		| ((legacyFlags & (1 << 3)) ? qint32(Flag::Pattern) : 0)
		| ((legacyFlags & (1 << 4)) ? qint32(Flag::Dark) : 0);
}

} // namespace

WallPaper::WallPaper(WallPaperId id) : _id(id) {
}

void WallPaper::setLocalImageAsThumbnail(std::shared_ptr<Image> image) {
	Expects(IsDefaultWallPaper(*this)
		|| IsLegacy1DefaultWallPaper(*this)
		|| IsCustomWallPaper(*this)
		|| LocalWallPaperFileFor(*this).has_value());
	Expects(_thumbnail == nullptr);

	_thumbnail = std::move(image);
}

WallPaperId WallPaper::id() const {
	return _id;
}

QString WallPaper::emojiId() const {
	return _emojiId;
}

bool WallPaper::equals(const WallPaper &paper) const {
	return (_flags == paper._flags)
		&& (_slug == paper._slug)
		&& (_emojiId == paper._emojiId)
		&& (_backgroundColors == paper._backgroundColors)
		&& (_rotation == paper._rotation)
		&& (_intensity == paper._intensity)
		&& (_blurred == paper._blurred)
		&& (_document == paper._document);
}

const std::vector<QColor> WallPaper::backgroundColors() const {
	return _backgroundColors;
}

DocumentData *WallPaper::document() const {
	return _document;
}

Image *WallPaper::localThumbnail() const {
	return _thumbnail.get();
}

bool WallPaper::isPattern() const {
	return _flags & WallPaperFlag::Pattern;
}

bool WallPaper::isDefault() const {
	return _flags & WallPaperFlag::Default;
}

bool WallPaper::isCreator() const {
	return _flags & WallPaperFlag::Creator;
}

bool WallPaper::isDark() const {
	return _flags & WallPaperFlag::Dark;
}

bool WallPaper::isLocal() const {
	return !document() && _thumbnail;
}

bool WallPaper::isBlurred() const {
	return _blurred;
}

int WallPaper::patternIntensity() const {
	return _intensity;
}

float64 WallPaper::patternOpacity() const {
	return _intensity / 100.;
}

int WallPaper::gradientRotation() const {
	// In case of complex gradients rotation value is dynamic.
	return (_backgroundColors.size() < 3) ? _rotation : 0;
}

bool WallPaper::hasShareUrl() const {
	return !_slug.isEmpty();
}

QStringList WallPaper::collectShareParams() const {
	auto result = QStringList();
	if (isPattern()) {
		if (!backgroundColors().empty()) {
			result.push_back(
				"bg_color=" + StringFromColors(backgroundColors()));
		}
		if (_intensity) {
			result.push_back("intensity=" + QString::number(_intensity));
		}
	}
	if (_rotation && backgroundColors().size() == 2) {
		result.push_back("rotation=" + QString::number(_rotation));
	}
	auto mode = QStringList();
	if (_blurred) {
		mode.push_back("blur");
	}
	if (!mode.isEmpty()) {
		result.push_back("mode=" + mode.join('+'));
	}
	return result;
}

bool WallPaper::isNull() const {
	return !_id && _slug.isEmpty() && _backgroundColors.empty();
}

QString WallPaper::key() const {
	if (isNull()) {
		return QString();
	}
	const auto base = _slug.isEmpty()
		? (_id
			? QString::number(_id)
			: StringFromColors(backgroundColors()))
		: ("bg/" + _slug);
	auto params = collectShareParams();
	if (_document && !isPattern()) {
		params += u"&intensity="_q + QString::number(_intensity);
	}
	return params.isEmpty() ? base : (base + '?' + params.join('&'));
}

QString WallPaper::shareUrl(not_null<Main::Session*> session) const {
	if (!hasShareUrl()) {
		return QString();
	}
	const auto base = session->createInternalLinkFull("bg/" + _slug);
	const auto params = collectShareParams();
	return params.isEmpty() ? base : (base + '?' + params.join('&'));
}

void WallPaper::loadDocumentThumbnail() const {
	if (_document) {
		_document->loadThumbnail(fileOrigin());
	}
}

void WallPaper::loadDocument() const {
	if (_document) {
		_document->save(fileOrigin(), QString());
	}
}

FileOrigin WallPaper::fileOrigin() const {
	return FileOriginWallpaper(_id, _accessHash, _ownerId, _slug);
}

UserId WallPaper::ownerId() const {
	return _ownerId;
}

MTPInputWallPaper WallPaper::mtpInput(not_null<Main::Session*> session) const {
	return (_ownerId && _ownerId != session->userId() && !_slug.isEmpty())
		? MTP_inputWallPaperSlug(MTP_string(_slug))
		: MTP_inputWallPaper(MTP_long(_id), MTP_long(_accessHash));
}

MTPWallPaperSettings WallPaper::mtpSettings() const {
	const auto serializeForIndex = [&](int index) {
		return (_backgroundColors.size() > index)
			? MTP_int(SerializeColor(_backgroundColors[index]))
			: MTP_int(0);
	};
	using Flag = MTPDwallPaperSettings::Flag;
	const auto flagForIndex = [&](int index) {
		return (_backgroundColors.size() <= index)
			? Flag(0)
			: (index == 0)
			? Flag::f_background_color
			: (index == 1)
			? Flag::f_second_background_color
			: (index == 2)
			? Flag::f_third_background_color
			: Flag::f_fourth_background_color;
	};
	return MTP_wallPaperSettings(
		MTP_flags((_blurred ? Flag::f_blur : Flag(0))
			| Flag::f_intensity
			| Flag::f_rotation
			| (_emojiId.isEmpty() ? Flag() : Flag::f_emoticon)
			| flagForIndex(0)
			| flagForIndex(1)
			| flagForIndex(2)
			| flagForIndex(3)),
		serializeForIndex(0),
		serializeForIndex(1),
		serializeForIndex(2),
		serializeForIndex(3),
		MTP_int(_intensity),
		MTP_int(_rotation),
		MTP_string(_emojiId));
}

WallPaper WallPaper::withUrlParams(
		const QMap<QString, QString> &params) const {
	auto result = *this;
	result._blurred = false;
	result._backgroundColors = ColorsFromString(_slug);
	result._intensity = kDefaultIntensity;
	if (auto mode = params.value("mode"); !mode.isEmpty()) {
		const auto list = mode.replace('+', ' ').split(' ');
		for (const auto &change : list) {
			if (change == u"blur"_q) {
				result._blurred = true;
			}
		}
	}
	if (result._backgroundColors.empty()) {
		result._backgroundColors = ColorsFromString(params.value("bg_color"));
	}
	if (result._backgroundColors.empty()) {
		result._backgroundColors = ColorsFromString(params.value("gradient"));
	}
	if (result._backgroundColors.empty()) {
		result._backgroundColors = ColorsFromString(params.value("color"));
	}
	if (result._backgroundColors.empty()) {
		result._backgroundColors = ColorsFromString(params.value("slug"));
	}
	if (const auto string = params.value("intensity"); !string.isEmpty()) {
		auto ok = false;
		const auto intensity = string.toInt(&ok);
		if (ok && base::in_range(intensity, -100, 101)) {
			result._intensity = intensity;
		}
	}
	result._rotation = params.value("rotation").toInt();
	result._rotation = (std::clamp(result._rotation, 0, 315) / 45) * 45;

	return result;
}

WallPaper WallPaper::withBlurred(bool blurred) const {
	auto result = *this;
	result._blurred = blurred;
	return result;
}

WallPaper WallPaper::withPatternIntensity(int intensity) const {
	auto result = *this;
	result._intensity = intensity;
	return result;
}

WallPaper WallPaper::withGradientRotation(int rotation) const {
	auto result = *this;
	result._rotation = rotation;
	return result;
}

WallPaper WallPaper::withBackgroundColors(std::vector<QColor> colors) const {
	auto result = *this;
	result._backgroundColors = std::move(colors);
	if (!ColorsFromString(_slug).empty()) {
		result._slug = StringFromColors(result._backgroundColors);
	}
	return result;
}

WallPaper WallPaper::withParamsFrom(const WallPaper &other) const {
	auto result = *this;
	result._blurred = other._blurred;
	if (!other._backgroundColors.empty()) {
		result._backgroundColors = other._backgroundColors;
		if (!ColorsFromString(_slug).empty()) {
			result._slug = StringFromColors(result._backgroundColors);
		}
	}
	result._intensity = other._intensity;
	if (other.isPattern()) {
		result._flags |= WallPaperFlag::Pattern;
	}
	return result;
}

WallPaper WallPaper::withoutImageData() const {
	auto result = *this;
	result._thumbnail = nullptr;
	return result;
}

std::optional<WallPaper> WallPaper::Create(
		not_null<Main::Session*> session,
		const MTPWallPaper &data) {
	return data.match([&](const MTPDwallPaper &data) {
		return Create(session, data);
	}, [](const MTPDwallPaperNoFile &data) {
		return Create(data);
	});
}

std::optional<WallPaper> WallPaper::Create(
		not_null<Main::Session*> session,
		const MTPDwallPaper &data) {
	const auto document = session->data().processDocument(
		data.vdocument());
	if (!document->checkWallPaperProperties()) {
		return std::nullopt;
	}
	auto result = WallPaper(data.vid().v);
	result._accessHash = data.vaccess_hash().v;
	result._ownerId = session->userId();
	result._flags = (data.is_dark() ? WallPaperFlag::Dark : WallPaperFlag(0))
		| (data.is_pattern() ? WallPaperFlag::Pattern : WallPaperFlag(0))
		| (data.is_default() ? WallPaperFlag::Default : WallPaperFlag(0))
		| (data.is_creator() ? WallPaperFlag::Creator : WallPaperFlag(0));
	result._slug = qs(data.vslug());
	result._document = document;
	if (const auto settings = data.vsettings()) {
		settings->match([&](const MTPDwallPaperSettings &data) {
			result._blurred = data.is_blur();
			if (const auto intensity = data.vintensity()) {
				result._intensity = intensity->v;
			}
			if (result.isPattern()) {
				result._backgroundColors = ColorsFromMTP(data);
				if (const auto rotation = data.vrotation()) {
					result._rotation = rotation->v;
				}
			}
		});
	}
	return result;
}

std::optional<WallPaper> WallPaper::Create(const MTPDwallPaperNoFile &data) {
	auto result = WallPaper(data.vid().v);
	result._flags = (data.is_dark() ? WallPaperFlag::Dark : WallPaperFlag(0))
		| (data.is_default() ? WallPaperFlag::Default : WallPaperFlag(0));
	result._blurred = false;
	result._backgroundColors.clear();
	if (const auto settings = data.vsettings()) {
		settings->match([&](const MTPDwallPaperSettings &data) {
			result._blurred = data.is_blur();
			result._backgroundColors = ColorsFromMTP(data);
			if (const auto rotation = data.vrotation()) {
				result._rotation = rotation->v;
			}
			result._emojiId = qs(data.vemoticon().value_or_empty());
		});
	}
	return result;
}

QByteArray WallPaper::serialize() const {
	auto size = sizeof(quint64) // _id
		+ sizeof(quint64) // _accessHash
		+ sizeof(qint32) // version tag
		+ sizeof(qint32) // version
		+ sizeof(qint32) // _flags
		+ Serialize::stringSize(_slug)
		+ sizeof(qint32) // _settings
		+ sizeof(qint32) // _backgroundColors.size()
		+ (_backgroundColors.size() * sizeof(quint32)) // _backgroundColors
		+ sizeof(qint32) // _intensity
		+ sizeof(qint32) // _rotation
		+ sizeof(quint64); // ownerId

	auto result = QByteArray();
	result.reserve(size);
	{
		auto stream = QDataStream(&result, QIODevice::WriteOnly);
		stream.setVersion(QDataStream::Qt_5_1);
		stream
			<< quint64(_id)
			<< quint64(_accessHash)
			<< qint32(kVersionTag)
			<< qint32(kVersion)
			<< qint32(_flags)
			<< _slug
			<< qint32(_blurred ? 1 : 0)
			<< qint32(_backgroundColors.size());
		for (const auto &color : _backgroundColors) {
			stream << SerializeMaybeColor(color);
		}
		stream
			<< qint32(_intensity)
			<< qint32(_rotation)
			<< quint64(_ownerId.bare);
	}
	return result;
}

std::optional<WallPaper> WallPaper::FromSerialized(
		const QByteArray &serialized) {
	if (serialized.isEmpty()) {
		return std::nullopt;
	}

	auto id = quint64();
	auto accessHash = quint64();
	auto versionTag = qint32();
	auto version = qint32(0);

	auto stream = QDataStream(serialized);
	stream.setVersion(QDataStream::Qt_5_1);
	stream
		>> id
		>> accessHash
		>> versionTag;

	auto flags = qint32();
	auto ownerId = UserId();
	auto slug = QString();
	auto blurred = qint32();
	auto backgroundColors = std::vector<QColor>();
	auto intensity = qint32();
	auto rotation = qint32();
	if (versionTag == kVersionTag) {
		auto bareOwnerId = quint64();
		auto backgroundColorsCount = qint32();
		stream
			>> version
			>> flags
			>> slug
			>> blurred
			>> backgroundColorsCount;
		if (backgroundColorsCount < 0 || backgroundColorsCount > 4) {
			return std::nullopt;
		}
		backgroundColors.reserve(backgroundColorsCount);
		for (auto i = 0; i != backgroundColorsCount; ++i) {
			auto serialized = quint32();
			stream >> serialized;
			const auto color = MaybeColorFromSerialized(serialized);
			if (!color) {
				return std::nullopt;
			}
			backgroundColors.push_back(*color);
		}
		stream
			>> intensity
			>> rotation
			>> bareOwnerId;
		ownerId = UserId(BareId(bareOwnerId));
	} else {
		auto settings = qint32();
		auto backgroundColor = quint32();
		stream
			>> slug
			>> settings
			>> backgroundColor
			>> intensity;
		if (!stream.atEnd()) {
			auto field1 = qint32();
			auto field2 = qint32();
			stream >> field1;
			if (!stream.atEnd()) {
				stream >> field2;
			}
			ownerId = UserId(
				BareId(uint32(field1)) | (BareId(uint32(field2)) << 32));
		}
		flags = RawFromLegacyFlags(versionTag);
		blurred = (settings & qint32(1U << 1)) ? 1 : 0;
		if (const auto color = MaybeColorFromSerialized(backgroundColor)) {
			backgroundColors.push_back(*color);
		}
	}
	if (stream.status() != QDataStream::Ok) {
		return std::nullopt;
	} else if (intensity < -100 || intensity > 100) {
		return std::nullopt;
	}
	auto result = WallPaper(id);
	result._accessHash = accessHash;
	result._ownerId = ownerId;
	result._flags = WallPaperFlags::from_raw(flags);
	result._slug = slug;
	result._blurred = (blurred == 1);
	result._backgroundColors = std::move(backgroundColors);
	result._intensity = intensity;
	result._rotation = rotation;
	return result;
}

std::optional<WallPaper> WallPaper::FromLegacySerialized(
		quint64 id,
		quint64 accessHash,
		quint32 flags,
		QString slug) {
	auto result = WallPaper(id);
	result._accessHash = accessHash;
	result._flags = WallPaperFlags::from_raw(RawFromLegacyFlags(flags));
	result._slug = slug;
	if (const auto color = ColorFromString(slug)) {
		result._backgroundColors.push_back(*color);
	}
	return result;
}

std::optional<WallPaper> WallPaper::FromLegacyId(qint32 legacyId) {
	auto result = WallPaper(FromLegacyBackgroundId(legacyId));
	if (!IsCustomWallPaper(result)) {
		result._flags = WallPaperFlag::Default;
	}
	return result;
}

std::optional<WallPaper> WallPaper::FromColorsSlug(const QString &slug) {
	auto colors = ColorsFromString(slug);
	if (colors.empty()) {
		return std::nullopt;
	}
	auto result = CustomWallPaper();
	result._slug = slug;
	result._backgroundColors = std::move(colors);
	return result;
}

WallPaper WallPaper::FromEmojiId(const QString &emojiId) {
	auto result = WallPaper(0);
	result._emojiId = emojiId;
	return result;
}

WallPaper WallPaper::ConstructDefault() {
	auto result = WallPaper(
		kDefaultBackground
	).withPatternIntensity(50).withBackgroundColors({
		QColor(219, 221, 187),
		QColor(107, 165, 135),
		QColor(213, 216, 141),
		QColor(136, 184, 132),
	});
	result._flags |= WallPaperFlag::Default | WallPaperFlag::Pattern;
	return result;
}

WallPaper WallPaper::ConstructLocalPattern(
		WallPaperId id,
		std::vector<QColor> colors,
		int intensity) {
	auto result = WallPaper(id).withPatternIntensity(
		intensity
	).withBackgroundColors(std::move(colors));
	result._flags |= WallPaperFlag::Pattern;
	return result;
}

namespace {

using ImportedFile = Storage::Account::MtsLinkWallPaperFile;

// The ids of the imported: by the name of the file, the same in each run.
constexpr auto kImportedIdBase = WallPaperId(0x4D54'534C'1000'0000ULL);

base::flat_map<WallPaperId, LocalWallPaperFile> ImportedFiles;

// The thumbnails by the names: the list is built at each opening, the
// thumbnails of an import are prepared in the background.
base::flat_map<QString, std::shared_ptr<Image>> ImportedThumbnails;
constexpr auto kImportedThumbnailSize = QSize(360, 740);

// The imported files of the account (by its folder): read once.
QString ImportedBasePath;
std::vector<ImportedFile> ImportedList;

[[nodiscard]] WallPaperId ImportedId(const QString &name) {
	// FNV-1a: stable between the runs, unlike qHash.
	auto hash = uint32(2166136261U);
	for (const auto ch : name.toUtf8()) {
		hash = (hash ^ uint8(ch)) * 16777619U;
	}
	return kImportedIdBase | (hash & 0x0FFF'FFFFU);
}

[[nodiscard]] bool IsPatternFile(const QString &name) {
	return name.endsWith(u".tgv"_q, Qt::CaseInsensitive)
		|| name.endsWith(u".svg"_q, Qt::CaseInsensitive);
}

[[nodiscard]] bool IsPhotoFile(const QString &name) {
	return name.endsWith(u".jpg"_q, Qt::CaseInsensitive)
		|| name.endsWith(u".jpeg"_q, Qt::CaseInsensitive)
		|| name.endsWith(u".png"_q, Qt::CaseInsensitive)
		|| name.endsWith(u".webp"_q, Qt::CaseInsensitive);
}

[[nodiscard]] QByteArray GzipBytes(const QByteArray &bytes) {
	auto stream = z_stream();
	if (deflateInit2(
			&stream,
			Z_BEST_COMPRESSION,
			Z_DEFLATED,
			15 + 16, // The gzip header.
			8,
			Z_DEFAULT_STRATEGY) != Z_OK) {
		return QByteArray();
	}
	auto result = QByteArray(int(deflateBound(&stream, bytes.size())) + 32, 0);
	stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(bytes.data()));
	stream.avail_in = uInt(bytes.size());
	stream.next_out = reinterpret_cast<Bytef*>(result.data());
	stream.avail_out = uInt(result.size());
	const auto done = (deflate(&stream, Z_FINISH) == Z_STREAM_END);
	result.resize(int(stream.total_out));
	deflateEnd(&stream);
	return done ? result : QByteArray();
}

// The clipping of the svg (the parts over the edges cut by the clip paths
// and the masks of the rectangles): Qt Svg draws the rectangles of the clip
// as the filled ones, the squares over the edges of the pattern.
[[nodiscard]] QByteArray StripSvgClips(const QByteArray &svg) {
	if (!svg.contains("<clipPath") && !svg.contains("<mask")) {
		return svg;
	}
	static const auto kBlocks = QRegularExpression(
		u"<(clipPath|mask)\\b[\\s\\S]*?</\\1\\s*>"_q);
	static const auto kAttributes = QRegularExpression(
		u"\\s(clip-path|mask)=\"[^\"]*\""_q);
	auto text = QString::fromUtf8(svg);
	text.remove(kBlocks);
	text.remove(kAttributes);
	return text.toUtf8();
}

// The name of the imported file: the svg is kept as tgv.
[[nodiscard]] QString ImportedName(const QString &name) {
	return name.endsWith(u".svg"_q, Qt::CaseInsensitive)
		? (QFileInfo(name).completeBaseName() + u".tgv"_q)
		: name;
}

// The svg is kept gzipped, as the patterns of Telegram.
[[nodiscard]] std::optional<ImportedFile> PrepareImported(
		const QString &name,
		QByteArray bytes) {
	if (name.endsWith(u".tgv"_q, Qt::CaseInsensitive)) {
		const auto unpacked = Images::UnpackGzip(bytes);
		const auto stripped = StripSvgClips(unpacked);
		if (stripped.size() != unpacked.size()) {
			bytes = GzipBytes(stripped);
		}
		return bytes.isEmpty()
			? std::nullopt
			: std::make_optional(ImportedFile{
				.name = name,
				.content = std::move(bytes),
			});
	} else if (name.endsWith(u".svg"_q, Qt::CaseInsensitive)) {
		bytes = GzipBytes(StripSvgClips(bytes));
		if (bytes.isEmpty()) {
			return std::nullopt;
		}
		return ImportedFile{
			.name = ImportedName(name),
			.content = std::move(bytes),
		};
	} else if (IsPatternFile(name) || IsPhotoFile(name)) {
		return ImportedFile{ .name = name, .content = std::move(bytes) };
	}
	return std::nullopt;
}

[[nodiscard]] bool AddImported(ImportedFile file) {
	if (ranges::contains(ImportedList, file.name, &ImportedFile::name)) {
		return false;
	}
	ImportedList.push_back(std::move(file));
	return true;
}

std::vector<ImportedFile> &Imported(not_null<Main::Session*> session) {
	auto &local = session->account().local();
	if (ImportedBasePath == local.basePath()) {
		return ImportedList;
	}
	ImportedBasePath = local.basePath();
	ImportedList = local.readMtsLinkWallPapers();

	// The patterns imported before the clips were cut: fixed once.
	auto fixed = 0;
	for (auto &file : ImportedList) {
		if (file.name.endsWith(u".tgv"_q, Qt::CaseInsensitive)) {
			const auto unpacked = Images::UnpackGzip(file.content);
			const auto stripped = StripSvgClips(unpacked);
			if (stripped.size() != unpacked.size()) {
				if (auto packed = GzipBytes(stripped); !packed.isEmpty()) {
					file.content = std::move(packed);
					++fixed;
				}
			}
		}
	}
	if (fixed) {
		local.writeMtsLinkWallPapers(ImportedList);
		LOG(("MtsLink Wallpapers: the clips of %1 removed").arg(fixed));
	}

	// The files of the first versions of the import, in a folder.
	const auto folder = cWorkingDir() + u"tdata/mtslink_wallpapers/"_q;
	if (QDir(folder).exists()) {
		auto moved = 0;
		const auto files = QDir(folder).entryList(QDir::Files, QDir::Name);
		for (const auto &name : files) {
			auto file = QFile(folder + name);
			if (file.open(QIODevice::ReadOnly)) {
				if (const auto prepared = PrepareImported(name, file.readAll())) {
					moved += AddImported(*prepared) ? 1 : 0;
				}
			}
		}
		local.writeMtsLinkWallPapers(ImportedList);
		QDir(folder).removeRecursively();
		LOG(("MtsLink Wallpapers: %1 moved from the folder").arg(moved));
	}
	return ImportedList;
}

} // namespace

std::optional<LocalWallPaperFile> LocalWallPaperFileFor(
		const WallPaper &paper) {
	const auto i = ImportedFiles.find(paper.id());
	return (i != end(ImportedFiles))
		? std::make_optional(i->second)
		: std::nullopt;
}

namespace {

rpl::event_stream<> ThumbnailsReady;
bool ThumbnailsLoading = false;

// The files the thumbnails of which could not be read: not read again.
base::flat_set<QString> ThumbnailsFailed;

// The thumbnails missing are prepared in the background, sent in batches.
void LoadImportedThumbnails(not_null<Main::Session*> session) {
	if (ThumbnailsLoading) {
		return;
	}
	struct Task {
		QString name;
		QByteArray content;
		bool gzipSvg = false;
	};
	auto tasks = std::vector<Task>();
	for (const auto &file : Imported(session)) {
		if (!ImportedThumbnails.contains(file.name)
			&& !ThumbnailsFailed.contains(file.name)) {
			tasks.push_back({
				.name = file.name,
				.content = file.content,
				.gzipSvg = IsPatternFile(file.name),
			});
		}
	}
	if (tasks.empty()) {
		return;
	}
	ThumbnailsLoading = true;
	const auto weak = base::make_weak(session);
	const auto maxSize = kImportedThumbnailSize * style::DevicePixelRatio();
	crl::async([=, tasks = std::move(tasks)] {
		constexpr auto kBatchDelay = crl::time(300);
		auto batch = std::vector<std::pair<QString, QImage>>();
		auto sent = crl::now();
		const auto send = [&](bool last) {
			crl::on_main(weak, [=, batch = std::move(batch)]() mutable {
				for (auto &[name, image] : batch) {
					if (image.isNull()) {
						LOG(("MtsLink Wallpapers: could not read %1").arg(name));
						ThumbnailsFailed.emplace(name);
					} else {
						ImportedThumbnails[name] = std::make_shared<Image>(
							std::move(image));
					}
				}
				if (last) {
					ThumbnailsLoading = false;
				}
				ThumbnailsReady.fire({});
			});
			batch = {};
			sent = crl::now();
		};
		for (const auto &task : tasks) {
			batch.emplace_back(task.name, Images::Read({
				.content = task.content,
				.maxSize = maxSize,
				.gzipSvg = task.gzipSvg,
			}).image);
			if (crl::now() - sent >= kBatchDelay) {
				send(false);
			}
		}
		send(true);
	});
}

} // namespace

rpl::producer<> ImportedWallPaperThumbnailsReady() {
	return ThumbnailsReady.events();
}

std::vector<WallPaper> ImportedWallPapers(
		not_null<Main::Session*> session,
		bool dark) {
	// The colors of the imported patterns, in turn.
	static const auto kColors = std::array{
		u"dbddbb~6ba587~d5d88d~88b884"_q,
		u"8adbf2~888dec~e39fea~679ced"_q,
		u"fec496~dd6cb9~962fbf~4f5bd5"_q,
		u"b0cdeb~9fb0ea~bbead5~b2e3dd"_q,
		u"efb7dc~f6d3b5~d7c2e8~c0dbf2"_q,
		u"a1c4fd~c2e9fb~f5e3b3~8ec5fc"_q,
		u"fccb90~d57eeb~f8a5c2~f9d976"_q,
		u"b5e2a0~6fc28b~e4e6a3~9ad4a8"_q,
	};
	auto result = std::vector<WallPaper>();
	auto patterns = 0;
	ImportedFiles.clear();
	for (const auto &imported : Imported(session)) {
		const auto pattern = IsPatternFile(imported.name);
		const auto file = LocalWallPaperFile{
			.path = imported.name,
			.content = imported.content,
			.gzipSvg = pattern,
		};
		if (ThumbnailsFailed.contains(imported.name)) {
			continue;
		}
		const auto id = ImportedId(imported.name);
		ImportedFiles[id] = file;
		auto paper = pattern
			? WallPaper::ConstructLocalPattern(
				id,
				ColorsFromString(kColors[patterns++ % kColors.size()]),
				dark ? -40 : 50)
			: WallPaper(id);
		// Without the thumbnail yet: the colors of a pattern are shown.
		const auto i = ImportedThumbnails.find(imported.name);
		if (i != end(ImportedThumbnails)) {
			paper.setLocalImageAsThumbnail(i->second);
		}
		result.push_back(std::move(paper));
	}
	LoadImportedThumbnails(session);
	return result;
}

bool IsImportedWallPaper(const WallPaper &paper) {
	return ImportedFiles.contains(paper.id());
}

bool HasImportedWallPapers(not_null<Main::Session*> session) {
	return !Imported(session).empty();
}

int RemoveAllImportedWallPapers(not_null<Main::Session*> session) {
	auto &list = Imported(session);
	const auto result = int(list.size());
	list.clear();
	ImportedFiles.clear();
	ImportedThumbnails.clear();
	session->account().local().writeMtsLinkWallPapers(list);
	return result;
}

void RemoveImportedWallPaper(
		not_null<Main::Session*> session,
		const WallPaper &paper) {
	const auto i = ImportedFiles.find(paper.id());
	if (i == end(ImportedFiles)) {
		return;
	}
	auto &list = Imported(session);
	list.erase(
		ranges::remove(list, i->second.path, &ImportedFile::name),
		end(list));
	ImportedFiles.erase(i);
	session->account().local().writeMtsLinkWallPapers(list);
}

void ImportWallPapers(
		not_null<Main::Session*> session,
		const QString &zipPath,
		std::shared_ptr<std::atomic<bool>> cancelled,
		Fn<void(int ready, int total)> progress,
		Fn<void(int imported)> done) {
	struct Prepared {
		ImportedFile file;
		QImage thumbnail;
	};
	const auto weak = base::make_weak(session);
	const auto maxSize = kImportedThumbnailSize * style::DevicePixelRatio();
	auto existing = base::flat_set<QString>();
	for (const auto &file : Imported(session)) {
		existing.emplace(file.name);
	}
	crl::async([=] {
		constexpr auto kMaxFileSize = 20 * 1024 * 1024;
		auto prepared = std::vector<Prepared>();
		const auto finish = [&] {
			crl::on_main(weak, [=, prepared = std::move(prepared)]() mutable {
				const auto strong = weak.get();
				Imported(strong);
				auto imported = 0;
				for (auto &[file, thumbnail] : prepared) {
					const auto name = file.name;
					if (AddImported(std::move(file))) {
						++imported;
						ImportedThumbnails[name] = std::make_shared<Image>(
							std::move(thumbnail));
					}
				}
				if (imported) {
					strong->account().local().writeMtsLinkWallPapers(
						ImportedList);
				}
				LOG(("MtsLink Wallpapers: imported %1 from %2"
					).arg(imported
					).arg(zipPath));
				done(imported);
			});
		};
		auto file = QFile(zipPath);
		if (!file.open(QIODevice::ReadOnly)) {
			LOG(("MtsLink Wallpapers: could not open %1").arg(zipPath));
			finish();
			return;
		}
		const auto content = file.readAll();
		const auto good = [](const QString &name) {
			return !name.startsWith('.')
				&& (IsPatternFile(name) || IsPhotoFile(name));
		};
		auto total = 0;
		{
			// The error of the reader stays after the end of the list: the
			// counting reader is apart.
			auto count = zlib::FileToRead(content);
			if (count.goToFirstFile() == UNZ_OK) {
				do {
					const auto name = count.getCurrentFileName();
					total += good(QFileInfo(name).fileName()) ? 1 : 0;
				} while (count.goToNextFile() == UNZ_OK);
			}
		}
		auto zip = zlib::FileToRead(content);
		if (!total || zip.goToFirstFile() != UNZ_OK) {
			LOG(("MtsLink Wallpapers: no wallpapers in %1").arg(zipPath));
			finish();
			return;
		}
		crl::on_main(weak, [=] { progress(0, total); });
		auto ready = 0;
		do {
			if (*cancelled) {
				LOG(("MtsLink Wallpapers: the import cancelled"));
				prepared.clear();
				break;
			}
			const auto name = QFileInfo(zip.getCurrentFileName()).fileName();
			if (!good(name)) {
				continue;
			} else if (existing.contains(ImportedName(name))) {
				++ready;
				crl::on_main(weak, [=] { progress(ready, total); });
				continue;
			}
			auto bytes = zip.readCurrentFileContent(kMaxFileSize);
			if (!bytes.isEmpty() && zip.error() == UNZ_OK) {
				if (auto ok = PrepareImported(name, std::move(bytes))) {
					auto thumbnail = Images::Read({
						.content = ok->content,
						.maxSize = maxSize,
						.gzipSvg = IsPatternFile(ok->name),
					}).image;
					if (!thumbnail.isNull()) {
						prepared.push_back({
							std::move(*ok),
							std::move(thumbnail),
						});
					}
				}
			}
			++ready;
			crl::on_main(weak, [=] { progress(ready, total); });
		} while (zip.goToNextFile() == UNZ_OK);
		finish();
	});
}

WallPaper ThemeWallPaper() {
	return WallPaper(kThemeBackground);
}

bool IsThemeWallPaper(const WallPaper &paper) {
	return (paper.id() == kThemeBackground);
}

WallPaper CustomWallPaper() {
	return WallPaper(kCustomBackground);
}

bool IsCustomWallPaper(const WallPaper &paper) {
	return (paper.id() == kCustomBackground);
}

WallPaper Legacy1DefaultWallPaper() {
	return WallPaper(kLegacy1DefaultBackground);
}

bool IsLegacy1DefaultWallPaper(const WallPaper &paper) {
	return (paper.id() == kLegacy1DefaultBackground);
}

bool IsLegacy2DefaultWallPaper(const WallPaper &paper) {
	return (paper.id() == kLegacy2DefaultBackground)
		|| (paper.id() == kIncorrectDefaultBackground);
}

bool IsLegacy3DefaultWallPaper(const WallPaper &paper) {
	return (paper.id() == kLegacy3DefaultBackground);
}

bool IsLegacy4DefaultWallPaper(const WallPaper &paper) {
	return (paper.id() == kLegacy4DefaultBackground);
}

WallPaper DefaultWallPaper() {
	return WallPaper::ConstructDefault();
}

bool IsDefaultWallPaper(const WallPaper &paper) {
	return (paper.id() == kDefaultBackground);
}

bool IsCloudWallPaper(const WallPaper &paper) {
	return (paper.id() != kIncorrectDefaultBackground)
		&& !IsThemeWallPaper(paper)
		&& !IsCustomWallPaper(paper)
		&& !IsLegacy1DefaultWallPaper(paper)
		&& !details::IsUninitializedWallPaper(paper)
		&& !details::IsTestingThemeWallPaper(paper)
		&& !details::IsTestingDefaultWallPaper(paper)
		&& !details::IsTestingEditorWallPaper(paper);
}

QImage GenerateDitheredGradient(const Data::WallPaper &paper) {
	return Ui::GenerateDitheredGradient(
		paper.backgroundColors(),
		paper.gradientRotation());
}

namespace details {

WallPaper UninitializedWallPaper() {
	return WallPaper(kUninitializedBackground);
}

bool IsUninitializedWallPaper(const WallPaper &paper) {
	return (paper.id() == kUninitializedBackground);
}

WallPaper TestingThemeWallPaper() {
	return WallPaper(kTestingThemeBackground);
}

bool IsTestingThemeWallPaper(const WallPaper &paper) {
	return (paper.id() == kTestingThemeBackground);
}

WallPaper TestingDefaultWallPaper() {
	return WallPaper(
		kTestingDefaultBackground
	).withParamsFrom(DefaultWallPaper());
}

bool IsTestingDefaultWallPaper(const WallPaper &paper) {
	return (paper.id() == kTestingDefaultBackground);
}

WallPaper TestingEditorWallPaper() {
	return WallPaper(kTestingEditorBackground);
}

bool IsTestingEditorWallPaper(const WallPaper &paper) {
	return (paper.id() == kTestingEditorBackground);
}

} // namespace details
} // namespace Data

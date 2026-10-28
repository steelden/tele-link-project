/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flags.h"

class Image;

namespace Main {
class Session;
} // namespace Main

namespace Ui {

[[nodiscard]] QColor ColorFromSerialized(MTPint serialized);
[[nodiscard]] std::optional<QColor> MaybeColorFromSerialized(
	const tl::conditional<MTPint> &mtp);

} // namespace Ui

namespace Data {

struct FileOrigin;

enum class WallPaperFlag {
	Pattern = (1 << 0),
	Default = (1 << 1),
	Creator = (1 << 2),
	Dark = (1 << 3),
};
inline constexpr bool is_flag_type(WallPaperFlag) { return true; };
using WallPaperFlags = base::flags<WallPaperFlag>;

class WallPaper {
public:
	explicit WallPaper(WallPaperId id);

	void setLocalImageAsThumbnail(std::shared_ptr<Image> image);

	[[nodiscard]] bool equals(const WallPaper &paper) const;

	[[nodiscard]] WallPaperId id() const;
	[[nodiscard]] QString emojiId() const;
	[[nodiscard]] bool isNull() const;
	[[nodiscard]] QString key() const;
	[[nodiscard]] const std::vector<QColor> backgroundColors() const;
	[[nodiscard]] DocumentData *document() const;
	[[nodiscard]] Image *localThumbnail() const;
	[[nodiscard]] bool isPattern() const;
	[[nodiscard]] bool isDefault() const;
	[[nodiscard]] bool isCreator() const;
	[[nodiscard]] bool isDark() const;
	[[nodiscard]] bool isLocal() const;
	[[nodiscard]] bool isBlurred() const;
	[[nodiscard]] int patternIntensity() const;
	[[nodiscard]] float64 patternOpacity() const;
	[[nodiscard]] int gradientRotation() const;
	[[nodiscard]] bool hasShareUrl() const;
	[[nodiscard]] QString shareUrl(not_null<Main::Session*> session) const;

	void loadDocument() const;
	void loadDocumentThumbnail() const;
	[[nodiscard]] FileOrigin fileOrigin() const;

	[[nodiscard]] UserId ownerId() const;
	[[nodiscard]] MTPInputWallPaper mtpInput(
		not_null<Main::Session*> session) const;
	[[nodiscard]] MTPWallPaperSettings mtpSettings() const;

	[[nodiscard]] WallPaper withUrlParams(
		const QMap<QString, QString> &params) const;
	[[nodiscard]] WallPaper withBlurred(bool blurred) const;
	[[nodiscard]] WallPaper withPatternIntensity(int intensity) const;
	[[nodiscard]] WallPaper withGradientRotation(int rotation) const;
	[[nodiscard]] WallPaper withBackgroundColors(
		std::vector<QColor> colors) const;
	[[nodiscard]] WallPaper withParamsFrom(const WallPaper &other) const;
	[[nodiscard]] WallPaper withoutImageData() const;

	[[nodiscard]] static std::optional<WallPaper> Create(
		not_null<Main::Session*> session,
		const MTPWallPaper &data);
	[[nodiscard]] static std::optional<WallPaper> Create(
		not_null<Main::Session*> session,
		const MTPDwallPaper &data);
	[[nodiscard]] static std::optional<WallPaper> Create(
		const MTPDwallPaperNoFile &data);

	[[nodiscard]] QByteArray serialize() const;
	[[nodiscard]] static std::optional<WallPaper> FromSerialized(
		const QByteArray &serialized);
	[[nodiscard]] static std::optional<WallPaper> FromLegacySerialized(
		quint64 id,
		quint64 accessHash,
		quint32 flags,
		QString slug);
	[[nodiscard]] static std::optional<WallPaper> FromLegacyId(
		qint32 legacyId);
	[[nodiscard]] static std::optional<WallPaper> FromColorsSlug(
		const QString &slug);
	[[nodiscard]] static WallPaper FromEmojiId(const QString &emojiId);
	[[nodiscard]] static WallPaper ConstructDefault();
	// MTS Link: an imported pattern over the colors.
	[[nodiscard]] static WallPaper ConstructLocalPattern(
		WallPaperId id,
		std::vector<QColor> colors,
		int intensity);

private:
	static constexpr auto kDefaultIntensity = 50;

	[[nodiscard]] QStringList collectShareParams() const;

	WallPaperId _id = WallPaperId();
	uint64 _accessHash = 0;
	UserId _ownerId = 0;
	WallPaperFlags _flags;
	QString _slug;
	QString _emojiId;

	std::vector<QColor> _backgroundColors;
	int _rotation = 0;
	int _intensity = kDefaultIntensity;
	bool _blurred = false;

	DocumentData *_document = nullptr;
	std::shared_ptr<Image> _thumbnail;

};

[[nodiscard]] WallPaper ThemeWallPaper();
[[nodiscard]] bool IsThemeWallPaper(const WallPaper &paper);
[[nodiscard]] WallPaper CustomWallPaper();
[[nodiscard]] bool IsCustomWallPaper(const WallPaper &paper);
[[nodiscard]] WallPaper Legacy1DefaultWallPaper();
[[nodiscard]] bool IsLegacy1DefaultWallPaper(const WallPaper &paper);
[[nodiscard]] bool IsLegacy2DefaultWallPaper(const WallPaper &paper);
[[nodiscard]] bool IsLegacy3DefaultWallPaper(const WallPaper &paper);
[[nodiscard]] bool IsLegacy4DefaultWallPaper(const WallPaper &paper);
[[nodiscard]] WallPaper DefaultWallPaper();
[[nodiscard]] bool IsDefaultWallPaper(const WallPaper &paper);

// MTS Link: the wallpapers imported by the user (tdata/mtslink_wallpapers):
// the patterns (svg, tgv) over the colors and the photos (jpg, png, webp).
struct LocalWallPaperFile {
	QString path; // The name of the imported.
	QByteArray content; // The imported: kept in the storage of the account.
	bool gzipSvg = false;
};
// The file of an imported wallpaper.
[[nodiscard]] std::optional<LocalWallPaperFile> LocalWallPaperFileFor(
	const WallPaper &paper);
// The patterns over the colors in the light mode, the colors through the
// patterns over the black in the dark one (a negative intensity).
[[nodiscard]] std::vector<WallPaper> ImportedWallPapers(
	not_null<Main::Session*> session,
	bool dark);
// Imports the wallpapers of a zip archive in the background: the progress
// (the files read of the files), then the count of the imported; nothing is
// imported when cancelled.
void ImportWallPapers(
	not_null<Main::Session*> session,
	const QString &zipPath,
	std::shared_ptr<std::atomic<bool>> cancelled,
	Fn<void(int ready, int total)> progress,
	Fn<void(int imported)> done);
[[nodiscard]] bool IsImportedWallPaper(const WallPaper &paper);
// The thumbnails of the imported are prepared in the background: the
// gallery is built again when some of them are ready.
[[nodiscard]] rpl::producer<> ImportedWallPaperThumbnailsReady();
[[nodiscard]] bool HasImportedWallPapers(not_null<Main::Session*> session);
// Removes all the imported, the count of the removed.
int RemoveAllImportedWallPapers(not_null<Main::Session*> session);
void RemoveImportedWallPaper(
	not_null<Main::Session*> session,
	const WallPaper &paper);
[[nodiscard]] bool IsCloudWallPaper(const WallPaper &paper);

[[nodiscard]] QImage GenerateDitheredGradient(const WallPaper &paper);

namespace details {

[[nodiscard]] WallPaper UninitializedWallPaper();
[[nodiscard]] bool IsUninitializedWallPaper(const WallPaper &paper);
[[nodiscard]] WallPaper TestingThemeWallPaper();
[[nodiscard]] bool IsTestingThemeWallPaper(const WallPaper &paper);
[[nodiscard]] WallPaper TestingDefaultWallPaper();
[[nodiscard]] bool IsTestingDefaultWallPaper(const WallPaper &paper);
[[nodiscard]] WallPaper TestingEditorWallPaper();
[[nodiscard]] bool IsTestingEditorWallPaper(const WallPaper &paper);

} // namespace details
} // namespace Data

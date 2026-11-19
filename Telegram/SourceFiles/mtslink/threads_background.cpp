/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.
*/
#include "mtslink/threads_background.h"

#include "mtslink/call_window.h"
#include "data/data_wall_paper.h"
#include "ui/chat/chat_theme.h"
#include "ui/effects/animation_value.h"
#include "ui/image/image.h"
#include "window/themes/window_theme.h"
#include "window/window_session_controller.h"
#include "styles/style_widgets.h"

#include <QtCore/QFile>
#include <QtCore/QJsonObject>
#include <QtGui/QPainterPath>

namespace MtsLink {
namespace {

constexpr auto kPaperKey = "threadsWallPaper";
constexpr auto kFileKey = "threadsWallPaperFile";
constexpr auto kGzipKey = "threadsWallPaperGzipSvg";

struct State {
	bool loaded = false;
	std::optional<Data::WallPaper> paper;
	QString file;
	bool gzipSvg = false;
	rpl::event_stream<> changes;
	base::flat_map<
		not_null<Window::SessionController*>,
		std::shared_ptr<Ui::ChatTheme>> themes;
	int generation = 0;
	bool subscribed = false;
	rpl::lifetime lifetime;
};

[[nodiscard]] State &Current() {
	static auto result = State();
	return result;
}

[[nodiscard]] QString Folder() {
	return cWorkingDir() + u"tdata/"_q;
}

void Load() {
	auto &state = Current();
	if (state.loaded) {
		return;
	}
	state.loaded = true;
	const auto settings = readLocalSettings();
	const auto serialized = QByteArray::fromBase64(
		settings.value(QString::fromLatin1(kPaperKey)).toString().toLatin1());
	if (serialized.isEmpty()) {
		return;
	}
	state.paper = Data::WallPaper::FromSerialized(serialized);
	state.file = settings.value(QString::fromLatin1(kFileKey)).toString();
	state.gzipSvg = settings.value(QString::fromLatin1(kGzipKey)).toBool();
	if (!state.file.isEmpty() && !QFile::exists(Folder() + state.file)) {
		LOG(("MtsLink ThreadsBackground: no file '%1'").arg(state.file));
		state.file = QString();
	}
}

void Save() {
	const auto &state = Current();
	auto settings = readLocalSettings();
	if (state.paper) {
		settings.insert(
			QString::fromLatin1(kPaperKey),
			QString::fromLatin1(state.paper->serialize().toBase64()));
		settings.insert(QString::fromLatin1(kFileKey), state.file);
		settings.insert(QString::fromLatin1(kGzipKey), state.gzipSvg);
	} else {
		settings.remove(QString::fromLatin1(kPaperKey));
		settings.remove(QString::fromLatin1(kFileKey));
		settings.remove(QString::fromLatin1(kGzipKey));
	}
	writeLocalSettings(settings);
}

// The wallpaper of the chats as it is (the pattern is painted over it).
[[nodiscard]] Ui::ChatThemeBackground ChatsBackground() {
	const auto background = Window::Theme::Background();
	const auto &paper = background->paper();
	return {
		.key = paper.key() + u"-threads"_q,
		.prepared = background->prepared(),
		.preparedForTiled = background->preparedForTiled(),
		.gradientForFill = background->gradientForFill(),
		.colorForFill = background->colorForFill(),
		.colors = paper.backgroundColors(),
		.patternOpacity = paper.patternOpacity(),
		.gradientRotation = paper.gradientRotation(),
		.isPattern = paper.isPattern(),
		.tile = background->tile(),
	};
}

[[nodiscard]] Ui::ChatThemeBackgroundData OwnBackgroundData() {
	const auto &state = Current();
	const auto &paper = *state.paper;
	const auto isPattern = paper.isPattern();
	return {
		.key = paper.key() + u"-threads-own"_q,
		.path = state.file.isEmpty() ? QString() : (Folder() + state.file),
		.gzipSvg = state.gzipSvg,
		.colors = paper.backgroundColors(),
		.isPattern = isPattern,
		.patternOpacity = paper.patternOpacity(),
		.darkModeDimming = (isPattern
			? 100
			: std::clamp(paper.patternIntensity(), 0, 100)),
		.isBlurred = paper.isBlurred(),
		.forDarkMode = Window::Theme::IsNightMode(),
		.generateGradient = true,
		.gradientRotation = paper.gradientRotation(),
	};
}

void PushAll() {
	Load();
	auto &state = Current();
	const auto generation = ++state.generation;
	if (!state.paper) {
		const auto background = ChatsBackground();
		for (const auto &[controller, theme] : state.themes) {
			auto copy = background;
			theme->setBackground(std::move(copy));
		}
		return;
	}
	crl::async([=, data = OwnBackgroundData()] {
		auto prepared = Ui::PrepareBackgroundImage(data);
		crl::on_main([=, prepared = std::move(prepared)]() mutable {
			auto &state = Current();
			if (state.generation != generation) {
				return;
			}
			for (const auto &[controller, theme] : state.themes) {
				auto copy = prepared;
				theme->setBackground(std::move(copy));
			}
		});
	});
}

// The pattern of the threads, over the background of the chats. Dark
// lines in both modes: the background stays as dark as the chats one.
constexpr auto kPatternKey = "threadsPattern";
constexpr auto kPatternColorKey = "threadsPatternColor";
constexpr auto kPatternCustomKey = "threadsPatternCustomColor";
constexpr auto kOverlayDayAlpha = 0.09;
constexpr auto kOverlayNightAlpha = 0.24;
constexpr auto kOverlayLightDayAlpha = 0.45;
constexpr auto kOverlayLightNightAlpha = 0.08;
constexpr auto kOverlayAccentDayAlpha = 0.22;
constexpr auto kOverlayAccentNightAlpha = 0.30;

struct PatternColorState {
	ThreadsPatternColor mode = ThreadsPatternColor::Dark;
	QColor custom = QColor(0, 0, 0, 40);
};

[[nodiscard]] PatternColorState &CurrentPatternColor() {
	static auto result = [] {
		const auto settings = readLocalSettings();
		auto state = PatternColorState();
		const auto mode = settings.value(
			QString::fromLatin1(kPatternColorKey)).toInt(0);
		if (mode >= 0 && mode < int(ThreadsPatternColor::Count)) {
			state.mode = ThreadsPatternColor(mode);
		}
		const auto custom = settings.value(
			QString::fromLatin1(kPatternCustomKey));
		if (!custom.isUndefined()) {
			state.custom = QColor::fromRgba(QRgb(uint32(custom.toDouble())));
		}
		return state;
	}();
	return result;
}

[[nodiscard]] QColor ResolvePatternColor() {
	const auto &state = CurrentPatternColor();
	const auto dark = Window::Theme::IsNightMode();
	const auto withAlpha = [](QColor color, float64 alpha) {
		color.setAlpha(int(255 * alpha));
		return color;
	};
	switch (state.mode) {
	case ThreadsPatternColor::Light:
		return withAlpha(
			QColor(255, 255, 255),
			dark ? kOverlayLightNightAlpha : kOverlayLightDayAlpha);
	case ThreadsPatternColor::Accent:
		return withAlpha(
			st::windowActiveTextFg->c,
			dark ? kOverlayAccentNightAlpha : kOverlayAccentDayAlpha);
	case ThreadsPatternColor::Custom:
		return state.custom;
	case ThreadsPatternColor::Dark:
	case ThreadsPatternColor::Count:
		break;
	}
	return withAlpha(
		QColor(0, 0, 0),
		dark ? kOverlayNightAlpha : kOverlayDayAlpha);
}

[[nodiscard]] ThreadsPattern &CurrentPattern() {
	static auto result = [] {
		const auto value = readLocalSettings().value(
			QString::fromLatin1(kPatternKey)).toInt(
				int(ThreadsPattern::None));
		// By default no pattern: the threads look as the chats.
		return (value >= 0 && value < int(ThreadsPattern::Count))
			? ThreadsPattern(value)
			: ThreadsPattern::None;
	}();
	return result;
}

[[nodiscard]] QSize PatternSize(ThreadsPattern pattern) {
	switch (pattern) {
	case ThreadsPattern::Lines: return { 14, 14 };
	case ThreadsPattern::Dots: return { 12, 12 };
	case ThreadsPattern::Grid: return { 16, 16 };
	case ThreadsPattern::Diamonds: return { 16, 16 };
	case ThreadsPattern::Waves: return { 24, 14 };
	case ThreadsPattern::Zigzag: return { 16, 10 };
	case ThreadsPattern::Circles: return { 20, 20 };
	case ThreadsPattern::CelticRings: return { 24, 24 };
	case ThreadsPattern::CelticKnot: return { 24, 24 };
	case ThreadsPattern::CelticSpiral: return { 28, 28 };
	case ThreadsPattern::SlavicRhombus: return { 24, 24 };
	case ThreadsPattern::SlavicStitch: return { 24, 24 };
	case ThreadsPattern::Herringbone: return { 16, 10 };
	case ThreadsPattern::None:
	case ThreadsPattern::Count: break;
	}
	return {};
}

void PaintPattern(QPainter &p, ThreadsPattern pattern, QColor color) {
	const auto size = PatternSize(pattern);
	const auto w = size.width();
	const auto h = size.height();
	auto pen = QPen(color);
	pen.setWidthF(1.3);
	p.setPen(pen);
	// A motif crossing the edges of the tile: drawn in the neighbours too,
	// so the tiles join without seams.
	const auto wrapped = [&](auto &&draw) {
		for (const auto dx : { -w, 0, w }) {
			for (const auto dy : { -h, 0, h }) {
				p.save();
				p.translate(dx, dy);
				draw();
				p.restore();
			}
		}
	};
	const auto polygon = [&](QPointF center, float64 radius, int count, float64 rotation) {
		auto points = QPolygonF();
		for (auto i = 0; i != count; ++i) {
			const auto angle = rotation + (2 * M_PI * i / count);
			points.push_back(center + QPointF(
				radius * std::cos(angle),
				radius * std::sin(angle)));
		}
		return points;
	};
	p.setBrush(Qt::NoBrush);
	switch (pattern) {
	case ThreadsPattern::Lines:
		// The neighbours too: the lines continue in the next tiles.
		for (const auto shift : { -w, 0, w }) {
			p.drawLine(QPointF(shift, h), QPointF(shift + w, 0));
		}
		break;
	case ThreadsPattern::Dots:
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawEllipse(QPointF(w / 2., h / 2.), 1.5, 1.5);
		break;
	case ThreadsPattern::Grid:
		pen.setWidthF(1.);
		p.setPen(pen);
		p.drawLine(QPointF(0.5, 0), QPointF(0.5, h));
		p.drawLine(QPointF(0, 0.5), QPointF(w, 0.5));
		break;
	case ThreadsPattern::Diamonds:
		for (const auto shift : { -w, 0, w }) {
			p.drawLine(QPointF(shift, h), QPointF(shift + w, 0));
			p.drawLine(QPointF(shift, 0), QPointF(shift + w, h));
		}
		break;
	case ThreadsPattern::Waves: {
		auto path = QPainterPath(QPointF(0, h / 2.));
		path.cubicTo(w / 6., 2., w / 3., 2., w / 2., h / 2.);
		path.cubicTo(w * 2 / 3., h - 2., w * 5 / 6., h - 2., w, h / 2.);
		p.drawPath(path);
	} break;
	case ThreadsPattern::Zigzag: {
		const auto points = std::array{
			QPointF(0, h - 2.),
			QPointF(w / 2., 2.),
			QPointF(w, h - 2.),
		};
		p.drawPolyline(points.data(), int(points.size()));
	} break;
	case ThreadsPattern::Circles:
		pen.setWidthF(1.);
		p.setPen(pen);
		p.drawEllipse(QPointF(w / 2., h / 2.), 4., 4.);
		break;
	case ThreadsPattern::CelticRings:
		// The interlocking rings: in the corners and in the middle.
		pen.setWidthF(1.1);
		p.setPen(pen);
		wrapped([&] {
			p.drawEllipse(QPointF(0, 0), 9., 9.);
			p.drawEllipse(QPointF(w / 2., h / 2.), 9., 9.);
		});
		break;
	case ThreadsPattern::CelticKnot: {
		// A plait: the bands go over and under in turn (a gap is "under").
		pen.setWidthF(2.);
		pen.setCapStyle(Qt::FlatCap);
		p.setPen(pen);
		const auto gap = 3.5;
		// "\" goes under in the corners, "/" goes under in the middle.
		wrapped([&] {
			p.drawLine(QPointF(gap, gap), QPointF(w - gap, h - gap));
			p.drawLine(
				QPointF(0, h),
				QPointF(w / 2. - gap, h / 2. + gap));
			p.drawLine(
				QPointF(w / 2. + gap, h / 2. - gap),
				QPointF(w, 0));
		});
	} break;
	case ThreadsPattern::CelticSpiral: {
		pen.setWidthF(1.1);
		p.setPen(pen);
		auto path = QPainterPath();
		const auto center = QPointF(w / 2., h / 2.);
		const auto turns = 2.5;
		const auto steps = 80;
		for (auto i = 0; i <= steps; ++i) {
			const auto t = i / float64(steps);
			const auto angle = t * turns * 2 * M_PI;
			const auto radius = 1. + t * 9.;
			const auto point = center + QPointF(
				radius * std::cos(angle),
				radius * std::sin(angle));
			if (!i) {
				path.moveTo(point);
			} else {
				path.lineTo(point);
			}
		}
		p.drawPath(path);
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		wrapped([&] {
			p.drawEllipse(QPointF(0, 0), 1.3, 1.3);
		});
	} break;
	case ThreadsPattern::SlavicRhombus: {
		// The rhombus with a dot (a field), with the small ones between.
		pen.setWidthF(1.2);
		p.setPen(pen);
		const auto c = QPointF(w / 2., h / 2.);
		p.drawPolygon(polygon(c, 8., 4, 0.));
		p.drawPolygon(polygon(c, 4., 4, 0.));
		wrapped([&] {
			p.drawPolygon(polygon(QPointF(0, 0), 3., 4, 0.));
		});
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawEllipse(c, 1.2, 1.2);
	} break;
	case ThreadsPattern::SlavicStitch: {
		// The cross stitch: the crosses in a rhombus, as an embroidery.
		pen.setWidthF(1.);
		p.setPen(pen);
		const auto cell = w / 8.;
		const auto cross = [&](int i, int j) {
			const auto rect = QRectF(
				i * cell + 0.6,
				j * cell + 0.6,
				cell - 1.2,
				cell - 1.2);
			p.drawLine(rect.topLeft(), rect.bottomRight());
			p.drawLine(rect.topRight(), rect.bottomLeft());
		};
		for (auto i = 0; i != 8; ++i) {
			for (auto j = 0; j != 8; ++j) {
				const auto distance = std::abs(i - 4) + std::abs(j - 4);
				if (distance == 3 || distance == 0) {
					cross(i, j);
				}
			}
		}
	} break;
	case ThreadsPattern::Herringbone:
		// "The fir": the slanted stitches in turn.
		pen.setWidthF(1.2);
		p.setPen(pen);
		wrapped([&] {
			p.drawLine(QPointF(1, h - 1.), QPointF(w / 2. - 1., 1));
			p.drawLine(QPointF(w / 2. + 1., 1), QPointF(w - 1., h - 1.));
		});
		break;
	case ThreadsPattern::None:
	case ThreadsPattern::Count: break;
	}
}

[[nodiscard]] const QPixmap &OverlayTile() {
	static auto cached = QPixmap();
	static auto cachedColor = QColor();
	static auto cachedRatio = 0;
	static auto cachedPattern = ThreadsPattern::None;
	const auto color = ResolvePatternColor();
	const auto ratio = style::DevicePixelRatio();
	const auto pattern = CurrentPattern();
	if (!cached.isNull()
		&& cachedColor == color
		&& cachedRatio == ratio
		&& cachedPattern == pattern) {
		return cached;
	}
	const auto size = PatternSize(pattern);
	auto image = QImage(
		size * ratio,
		QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	image.setDevicePixelRatio(ratio);
	{
		auto p = QPainter(&image);
		p.setRenderHint(QPainter::Antialiasing);
		PaintPattern(p, pattern, color);
	}
	cached = QPixmap::fromImage(std::move(image));
	cachedColor = color;
	cachedRatio = ratio;
	cachedPattern = pattern;
	return cached;
}

} // namespace

bool isThreadsChatTheme(not_null<Ui::ChatTheme*> theme) {
	// The pattern is over the own wallpaper of the threads too.
	const auto &state = Current();
	for (const auto &[controller, threads] : state.themes) {
		if (threads.get() == theme.get()) {
			return true;
		}
	}
	return false;
}

void paintThreadsOverlay(QPainter &p, QRect clip) {
	if (CurrentPattern() == ThreadsPattern::None) {
		return;
	}
	p.save();
	p.setBrushOrigin(0, 0);
	p.fillRect(clip, QBrush(OverlayTile()));
	p.restore();
}

std::shared_ptr<Ui::ChatTheme> threadsChatTheme(
		not_null<Window::SessionController*> controller) {
	auto &state = Current();
	const auto i = state.themes.find(controller);
	if (i != end(state.themes)) {
		return i->second;
	}
	if (!state.subscribed) {
		state.subscribed = true;
		using Update = Window::Theme::BackgroundUpdate;
		Window::Theme::Background()->updates(
		) | rpl::filter([](const Update &update) {
			return (update.type == Update::Type::New)
				|| (update.type == Update::Type::Changed);
		}) | rpl::on_next([] {
			PushAll();
		}, state.lifetime);
	}
	auto theme = std::make_shared<Ui::ChatTheme>();
	state.themes.emplace(controller, theme);
	controller->lifetime().add([=] {
		Current().themes.remove(controller);
	});
	PushAll();
	return theme;
}

bool hasThreadsWallPaper() {
	Load();
	return Current().paper.has_value();
}

void setThreadsWallPaper(const Data::WallPaper &paper, QImage full) {
	Load();
	auto &state = Current();
	if (!state.file.isEmpty()) {
		QFile::remove(Folder() + state.file);
		state.file = QString();
	}
	state.gzipSvg = false;
	// The full image of the preview may be not ready yet: the imported
	// file itself (a pattern or a photo) or the chosen picture.
	if (const auto local = Data::LocalWallPaperFileFor(paper)) {
		auto bytes = local->content;
		if (bytes.isEmpty() && !local->path.isEmpty()) {
			auto file = QFile(local->path);
			if (file.open(QIODevice::ReadOnly)) {
				bytes = file.readAll();
			}
		}
		const auto name = u"threads_wallpaper.bin"_q;
		auto file = QFile(Folder() + name);
		if (!bytes.isEmpty()
			&& file.open(QIODevice::WriteOnly)
			&& file.write(bytes) == bytes.size()) {
			state.file = name;
			state.gzipSvg = local->gzipSvg;
			full = QImage();
		} else {
			LOG(("MtsLink ThreadsBackground: can't copy the imported '%1'"
				).arg(local->path));
		}
	} else if (full.isNull() && paper.localThumbnail()) {
		full = paper.localThumbnail()->original();
	}
	if (!full.isNull()) {
		// A pattern keeps its alpha, a photo is a jpeg.
		const auto png = paper.isPattern() || full.hasAlphaChannel();
		const auto name = png
			? u"threads_wallpaper.png"_q
			: u"threads_wallpaper.jpg"_q;
		if (full.save(Folder() + name, png ? "PNG" : "JPG", png ? -1 : 92)) {
			state.file = name;
		} else {
			LOG(("MtsLink ThreadsBackground: can't save '%1'").arg(name));
		}
	}
	state.paper = paper;
	// A new wallpaper: without a pattern over it (chosen again if needed).
	if (CurrentPattern() != ThreadsPattern::None) {
		CurrentPattern() = ThreadsPattern::None;
		auto settings = readLocalSettings();
		settings.insert(
			QString::fromLatin1(kPatternKey),
			int(ThreadsPattern::None));
		writeLocalSettings(settings);
	}
	Save();
	PushAll();
	state.changes.fire({});
}

void resetThreadsWallPaper() {
	Load();
	auto &state = Current();
	if (!state.file.isEmpty()) {
		QFile::remove(Folder() + state.file);
		state.file = QString();
	}
	state.paper = std::nullopt;
	Save();
	PushAll();
	state.changes.fire({});
}

rpl::producer<> threadsWallPaperChanges() {
	return Current().changes.events();
}

ThreadsPatternColor threadsPatternColor() {
	return CurrentPatternColor().mode;
}

QColor threadsPatternCustomColor() {
	return CurrentPatternColor().custom;
}

void setThreadsPatternColor(ThreadsPatternColor mode, QColor custom) {
	auto &state = CurrentPatternColor();
	state.mode = mode;
	if (custom.isValid()) {
		state.custom = custom;
	}
	auto settings = readLocalSettings();
	settings.insert(QString::fromLatin1(kPatternColorKey), int(mode));
	settings.insert(
		QString::fromLatin1(kPatternCustomKey),
		double(uint32(state.custom.rgba())));
	writeLocalSettings(settings);
	PushAll();
	Current().changes.fire({});
}

ThreadsPattern threadsPattern() {
	return CurrentPattern();
}

void setThreadsPattern(ThreadsPattern pattern) {
	if (CurrentPattern() == pattern) {
		return;
	}
	CurrentPattern() = pattern;
	auto settings = readLocalSettings();
	settings.insert(QString::fromLatin1(kPatternKey), int(pattern));
	writeLocalSettings(settings);
	// Repainted with the new pattern.
	PushAll();
	Current().changes.fire({});
}

} // namespace MtsLink

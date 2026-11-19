/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.
*/
#pragma once

namespace Data {
class WallPaper;
} // namespace Data

namespace Ui {
class ChatTheme;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace MtsLink {

// The background of the threads (the discussions): a separate wallpaper
// chosen in the settings, or the wallpaper of the chats slightly tinted.

// The theme of the threads of the window (for the default chat theme).
[[nodiscard]] std::shared_ptr<Ui::ChatTheme> threadsChatTheme(
	not_null<Window::SessionController*> controller);

// The pattern over the background of the threads (the wallpaper of the
// chats or the own one of the threads).
[[nodiscard]] bool isThreadsChatTheme(not_null<Ui::ChatTheme*> theme);
void paintThreadsOverlay(QPainter &p, QRect clip);

// The pattern over the background of the chats.
enum class ThreadsPattern {
	None,
	Lines,
	Dots,
	Grid,
	Diamonds,
	Waves,
	Zigzag,
	Circles,
	CelticRings,
	CelticKnot,
	CelticSpiral,
	SlavicRhombus,
	SlavicStitch,
	Herringbone,

	Count,
};
[[nodiscard]] ThreadsPattern threadsPattern();
void setThreadsPattern(ThreadsPattern pattern);

// The color of the pattern: the dark and the light ones with the alpha of
// the mode (day / night), the accent of the theme, or any (with its alpha).
enum class ThreadsPatternColor {
	Dark,
	Light,
	Accent,
	Custom,

	Count,
};
[[nodiscard]] ThreadsPatternColor threadsPatternColor();
[[nodiscard]] QColor threadsPatternCustomColor();
void setThreadsPatternColor(
	ThreadsPatternColor mode,
	QColor custom = QColor());

[[nodiscard]] bool hasThreadsWallPaper();
// The wallpaper and its full image (empty for the colors only).
void setThreadsWallPaper(const Data::WallPaper &paper, QImage full);
void resetThreadsWallPaper();
[[nodiscard]] rpl::producer<> threadsWallPaperChanges();

} // namespace MtsLink

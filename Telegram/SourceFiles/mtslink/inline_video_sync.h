/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.
*/
#pragma once

namespace MtsLink {

// The position of a video when the media viewer is closed: the inline
// video of the chat continues from it.
struct ViewerClosedPosition {
	DocumentId documentId = 0;
	crl::time position = 0;
};

[[nodiscard]] inline rpl::event_stream<ViewerClosedPosition> &ViewerClosed() {
	static auto result = rpl::event_stream<ViewerClosedPosition>();
	return result;
}

// The inline video is moved to a separate window (picture in picture):
// the viewer opens it and switches to the window at once.
[[nodiscard]] inline DocumentId &PipRequested() {
	static auto result = DocumentId();
	return result;
}

} // namespace MtsLink

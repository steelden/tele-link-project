/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.
*/
#pragma once

namespace Main {
class Session;
} // namespace Main

namespace MtsLink {
namespace Api {
enum class TaskSection;
} // namespace Api

// The sections of the tasks ("My tasks", "Important", ...) as the chats:
// a new type of the chats, as the threads, only in the folders (of the
// "Tasks" type). A click shows the tasks of the section.

[[nodiscard]] bool isTaskPeer(PeerId peerId);
[[nodiscard]] std::optional<Api::TaskSection> taskSectionForPeer(
	PeerId peerId);
[[nodiscard]] PeerId taskPeerId(Api::TaskSection section);
[[nodiscard]] std::vector<PeerId> taskPeerIds();

// After the session start: the rows, the "Tasks" folder (once).
void initTaskPeers(not_null<Main::Session*> session);
// The previews (the count, the nearest task) and the counter.
void refreshTaskPeers(not_null<Main::Session*> session);

} // namespace MtsLink

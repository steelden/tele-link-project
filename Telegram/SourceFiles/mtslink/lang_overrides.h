/*
This file is part of TeleLink,
a desktop application based on Telegram Desktop.
*/
#pragma once

namespace MtsLink {

void applyLangOverrides();

// The language is applied at once, but texts taken when widgets were
// created change only after a restart: a "restart" button is shown.
void setLangRestartRequired();
[[nodiscard]] rpl::producer<bool> langRestartRequiredValue();

} // namespace MtsLink

/*
This file is part of MTS Link Desktop,
based on Telegram Desktop.

The language of a code block by its content.
*/
#pragma once

namespace MtsLink {

// The language of a code block without one (the code from an IDE has no
// language in the clipboard, the code blocks of MTS Link have none): guessed
// by the content, empty if unsure.
[[nodiscard]] QString GuessCodeLanguage(const QString &code);

} // namespace MtsLink

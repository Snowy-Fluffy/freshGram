#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <vector>

namespace FreshLang {

using Strings = std::vector<std::pair<QByteArray, QByteArray>>;

// Translations of the freshGram strings shipped inside the application.
// Returns an empty list if there is no translation for the language.
[[nodiscard]] const Strings &BuiltinStrings(const QString &languageId);

} // namespace FreshLang

// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include <QtCore/QString>

namespace FreshDebugLogging {

// Reads the saved choice before the regular settings are loaded.
void Init(const QString &workingDir);
void SetEnabled(bool enabled);

[[nodiscard]] bool Enabled();

} // namespace FreshDebugLogging

// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/utils/debug_logging.h"

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <atomic>

namespace FreshDebugLogging {
namespace {

std::atomic<bool> Value = false;

} // namespace

void Init(const QString &workingDir) {
	auto file = QFile(workingDir + u"tdata/ayu_settings.json"_q);
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	Value = document.object().value(u"enableDebugLogging"_q).toBool(false);
}

void SetEnabled(bool enabled) {
	Value = enabled;
}

bool Enabled() {
	return Value;
}

} // namespace FreshDebugLogging

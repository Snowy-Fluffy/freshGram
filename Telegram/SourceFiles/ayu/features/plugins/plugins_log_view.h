// This is the source code of freshGram.
#pragma once

#include "base/basic_types.h"
#include "rpl/producer.h"

namespace Ui {
class GenericBox;
} // namespace Ui

namespace FreshPlugins {

enum class ConsoleColor : uchar {
	Normal,
	Dim,
	Good,
	Warning,
	Bad,
};

struct ConsolePart {
	QString text;
	ConsoleColor color = ConsoleColor::Normal;
};

struct ConsoleLine {
	std::vector<ConsolePart> parts;
	bool problem = false;
};

struct ConsoleDescriptor {
	rpl::producer<QString> title;
	QString problemsTab;
	QString empty;
	Fn<std::vector<ConsoleLine>()> lines;
	Fn<void()> clear;
};

void FillConsoleBox(
	not_null<Ui::GenericBox*> box,
	ConsoleDescriptor descriptor);

[[nodiscard]] QString PadLeft(const QString &text, int width);
[[nodiscard]] QString PadRight(const QString &text, int width);

} // namespace FreshPlugins

// This is the source code of freshGram.
#pragma once

namespace Window {
class SessionController;
} // namespace Window

namespace AyuUi {

void ShowPluginInstallBox(
	not_null<Window::SessionController*> controller,
	QByteArray archive);

void ChoosePluginFile(not_null<Window::SessionController*> controller);

} // namespace AyuUi

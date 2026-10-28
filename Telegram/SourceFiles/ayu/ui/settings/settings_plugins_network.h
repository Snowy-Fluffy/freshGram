// This is the source code of freshGram.
#pragma once

#include "settings/settings_common.h"
#include "settings/settings_common_session.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Settings {

class FreshPluginsNetworkSection : public Section<FreshPluginsNetworkSection> {
public:
	FreshPluginsNetworkSection(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();
};

} // namespace Settings

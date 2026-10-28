// This is the source code of freshGram.
#include "ayu/ui/settings/settings_plugins_network.h"

#include "lang_auto.h"
#include "ayu/ui/settings/plugins_settings.h"
#include "ayu/ui/settings/settings_plugins.h"
#include "settings/settings_builder.h"
#include "styles/style_menu_icons.h"
#include "ui/wrap/vertical_layout.h"

namespace Settings {

using namespace Builder;

namespace {

void BuildPluginsNetwork(SectionBuilder &builder) {
	builder.add([](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			AyuUi::AddPluginsNetworkSettings(wctx.container, wctx.controller);
		}, [&](const SearchContext &sctx) {
			sctx.entries->push_back({
				.id = u"ayu/plugins-network"_q,
				.title = tr::fresh_PluginsNetwork(tr::now),
				.section = sctx.sectionId,
			});
		});
	});
}

const auto kMeta = BuildHelper({
	.id = FreshPluginsNetworkSection::Id(),
	.parentId = FreshPluginsSection::Id(),
	.title = &tr::fresh_PluginsNetwork,
	.icon = &st::menuIconNetwork,
}, [](SectionBuilder &builder) {
	BuildPluginsNetwork(builder);
});

} // namespace

rpl::producer<QString> FreshPluginsNetworkSection::title() {
	return tr::fresh_PluginsNetwork();
}

FreshPluginsNetworkSection::FreshPluginsNetworkSection(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

void FreshPluginsNetworkSection::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

} // namespace Settings

// This is the source code of freshGram.
#include "ayu/ui/settings/settings_plugins.h"

#include "lang_auto.h"
#include "ayu/ui/settings/plugins_settings.h"
#include "ayu/ui/settings/settings_main.h"
#include "settings/settings_builder.h"
#include "styles/style_menu_icons.h"
#include "ui/wrap/vertical_layout.h"

namespace Settings {

using namespace Builder;

namespace {

void BuildPlugins(SectionBuilder &builder) {
	builder.add([](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			AyuUi::AddPluginsSettings(wctx.container, wctx.controller);
		}, [&](const SearchContext &sctx) {
			sctx.entries->push_back({
				.id = u"ayu/plugins"_q,
				.title = tr::ayu_Plugins(tr::now),
				.section = sctx.sectionId,
			});
		});
	});
}

const auto kMeta = BuildHelper({
	.id = AyuPluginsSection::Id(),
	.parentId = AyuMain::Id(),
	.title = &tr::ayu_Plugins,
	.icon = &st::menuIconCustomize,
}, [](SectionBuilder &builder) {
	builder.addSkip();
	BuildPlugins(builder);
});

} // namespace

rpl::producer<QString> AyuPluginsSection::title() {
	return tr::ayu_Plugins();
}

AyuPluginsSection::AyuPluginsSection(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

void AyuPluginsSection::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

} // namespace Settings

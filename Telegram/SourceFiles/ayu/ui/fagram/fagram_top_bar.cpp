/*
This file is part of FAgram Desktop,
the unofficial desktop client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/fagramdesktop/fadesktop/blob/dev/LEGAL
*/
#include "ayu/ui/fagram/fagram_top_bar.h"

#include "ayu/ui/fagram/fagram_svg_assets.h"
#include "ui/painter.h"
#include "ui/style/style_core_direction.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/fade_wrap.h"
#include "styles/style_ayu_styles.h"
#include "styles/style_basic.h"

#include <QtSvg/QSvgRenderer>

#include <cmath>

namespace FA::Ui {
namespace {

bool IsButtonVisible(const ::Ui::RpWidget *button) {
	if (!button || button->isHidden()) {
		return false;
	}
	if (const auto fade = dynamic_cast<const ::Ui::FadeWrap<::Ui::RpWidget>*>(button)) {
		return fade->toggled();
	}
	return true;
}

QImage PrepareMaterialShapeImage(int size, QColor color, int ratio) {
	const auto deviceSize = QSize(size * ratio, size * ratio);
	auto result = QImage(deviceSize, QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::transparent);

	auto svg = QSvgRenderer(QByteArray::fromRawData(
		fa::svg::material_shape2.data(),
		fa::svg::material_shape2.size()));

	const auto margin = float(deviceSize.width()) * 0.04f;
	const auto targetRect = QRectF(
		margin,
		margin,
		deviceSize.width() - 2 * margin,
		deviceSize.height() - 2 * margin);

	QPainter p(&result);
	p.setRenderHint(QPainter::Antialiasing);
	svg.render(&p, targetRect);
	p.setCompositionMode(QPainter::CompositionMode_SourceIn);
	p.fillRect(result.rect(), color);
	p.end();

	result.setDevicePixelRatio(ratio);
	return result;
}

void PaintMaterialShape(
		QPainter &p,
		const QRectF &targetRect,
		QColor color,
		float64 opacity) {
	if (opacity <= 0.) {
		return;
	}
	const auto size = int(std::round(targetRect.width()));
	const auto ratio = style::DevicePixelRatio();

	struct Cache {
		QColor color;
		int size = 0;
		int ratio = 0;
		QImage image;
	};
	static auto cache = Cache();
	if (cache.color != color || cache.size != size || cache.ratio != ratio) {
		cache = { color, size, ratio, PrepareMaterialShapeImage(size, color, ratio) };
	}

	const auto prevOpacity = p.opacity();
	if (opacity < 1.0) {
		p.setOpacity(prevOpacity * opacity);
	}
	p.drawImage(targetRect, cache.image);
	if (opacity < 1.0) {
		p.setOpacity(prevOpacity);
	}
}

// The icon buttons keep the icon position of their own wide style, so the
// button is moved to put the icon in the middle of its place in the pill.
void PlaceInSlot(not_null<::Ui::RpWidget*> wrapped) {
	const auto buttonWidth = st::fagramTopBarButtonWidth;
	const auto pillHeight = st::fagramTopBarPillHeight;
	const auto button = dynamic_cast<::Ui::IconButton*>(wrapped.get());
	if (!button) {
		wrapped->setGeometry(0, 0, buttonWidth, pillHeight);
		return;
	}
	// The default ripple has the color of the pill itself.
	button->setRippleColorOverride(&st::fagramTopBarRippleBg);
	const auto &st = button->st();
	const auto position = st.iconPosition;
	const auto left = (position.x() < 0)
		? 0
		: ((buttonWidth - st.icon.width()) / 2 - position.x());
	const auto top = (position.y() < 0)
		? 0
		: ((pillHeight - st.icon.height()) / 2 - position.y());
	wrapped->setGeometry(
		left,
		top,
		buttonWidth - left,
		pillHeight - top);
}

} // namespace

int TopBarBackPillSkip() {
	return st::fagramTopBarPillMargin
		+ st::fagramTopBarButtonWidth
		+ st::fagramTopBarContentSpacing;
}

int LayoutTopBarBackButton(
		int newWidth,
		int topBarHeight,
		::Ui::RpWidget *backButton) {
	if (!IsButtonVisible(backButton)) {
		return 0;
	}
	const auto pillMargin = st::fagramTopBarPillMargin;
	const auto buttonWidth = st::fagramTopBarButtonWidth;
	const auto pillHeight = st::fagramTopBarPillHeight;
	const auto pillTop = (topBarHeight - pillHeight) / 2;

	backButton->setGeometryToLeft(
		pillMargin,
		pillTop,
		buttonWidth,
		pillHeight,
		newWidth);
	if (const auto wrap = dynamic_cast<::Ui::Wrap<::Ui::RpWidget>*>(backButton)) {
		if (const auto wrapped = wrap->wrapped()) {
			PlaceInSlot(wrapped);
		}
	}
	return TopBarBackPillSkip();
}

int LayoutTopBarPillButtons(
		int newWidth,
		int topBarHeight,
		const std::vector<base::unique_qptr<::Ui::RpWidget>> &buttons) {
	const auto pillMargin = st::fagramTopBarPillMargin;
	const auto pillPadding = st::fagramTopBarPillPadding;
	const auto buttonWidth = st::fagramTopBarButtonWidth;
	const auto pillHeight = st::fagramTopBarPillHeight;
	const auto contentSpacing = st::fagramTopBarContentSpacing;
	auto visibleButtonsCount = 0;
	for (const auto &button : buttons) {
		if (IsButtonVisible(button.get())) {
			++visibleButtonsCount;
		}
	}
	const auto pillTop = (topBarHeight - pillHeight) / 2;
	const auto totalPillWidth = (visibleButtonsCount > 0)
		? (visibleButtonsCount * buttonWidth + pillPadding * 2)
		: 0;

	auto currentRight = pillMargin + pillPadding;
	for (const auto &button : buttons) {
		if (IsButtonVisible(button.get())) {
			button->setGeometryToRight(
				currentRight,
				pillTop,
				buttonWidth,
				pillHeight,
				newWidth);
			if (const auto wrap = dynamic_cast<::Ui::Wrap<::Ui::RpWidget>*>(button.get())) {
				if (const auto wrapped = wrap->wrapped()) {
					PlaceInSlot(wrapped);
				}
			}
			currentRight += buttonWidth;
		}
	}
	return (visibleButtonsCount > 0)
		? (pillMargin + totalPillWidth + contentSpacing)
		: 0;
}

void PaintTopBarPill(
		QPainter &p,
		int widgetWidth,
		int topBarHeight,
		const std::vector<base::unique_qptr<::Ui::RpWidget>> &buttons,
		const ::Ui::RpWidget *backButton,
		bool searchModeEnabled,
		bool selectionMode) {
	if (selectionMode) {
		return;
	}

	const auto pillMargin = st::fagramTopBarPillMargin;
	const auto pillPadding = st::fagramTopBarPillPadding;
	const auto buttonWidth = st::fagramTopBarButtonWidth;
	const auto pillHeight = st::fagramTopBarPillHeight;
	const auto rtl = style::RightToLeft();
	const auto pillTop = (topBarHeight - pillHeight) / 2;

	if (IsButtonVisible(backButton)) {
		const auto fade = dynamic_cast<const ::Ui::FadeWrap<::Ui::RpWidget>*>(backButton);
		const auto opacity = fade ? fade->shownProgress() : 1.0;
		if (opacity > 0.) {
			const auto backLeft = rtl
				? (widgetWidth - pillMargin - buttonWidth)
				: pillMargin;
			const auto backRect = QRectF(
				backLeft,
				pillTop,
				buttonWidth,
				pillHeight);

			PaintMaterialShape(
				p,
				backRect,
				st::fagramTopBarPillBg->c,
				opacity);
		}
	}

	if (!searchModeEnabled) {
		auto visibleButtonsCount = 0;
		for (const auto &button : buttons) {
			if (IsButtonVisible(button.get())) {
				++visibleButtonsCount;
			}
		}
		if (visibleButtonsCount > 0) {
			const auto totalPillWidth = visibleButtonsCount * buttonWidth + pillPadding * 2;
			const auto rightPillLeft = rtl
				? pillMargin
				: (widgetWidth - pillMargin - totalPillWidth);
			const auto rightPillRect = QRectF(
				rightPillLeft,
				pillTop,
				totalPillWidth,
				pillHeight);

			PainterHighQualityEnabler hq(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::fagramTopBarPillBg);
			p.drawRoundedRect(rightPillRect, pillHeight / 2.0, pillHeight / 2.0);
		}
	}
}

} // namespace FA::Ui

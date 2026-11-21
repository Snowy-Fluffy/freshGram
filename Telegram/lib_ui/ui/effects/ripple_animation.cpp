// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/effects/ripple_animation.h"

#include "ayu/ayu_ui_settings.h"
#include "base/random.h"
#include "ui/effects/animations.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/image/image_prepare.h"
#include "styles/style_widgets.h"

namespace Ui {

class RippleAnimation::Ripple {
public:
	Ripple(
		const style::RippleAnimation &st,
		QPoint origin,
		int startRadius,
		const QPixmap &mask,
		Fn<void()> update);
	Ripple(
		const style::RippleAnimation &st,
		const QPixmap &mask,
		Fn<void()> update);

	void paint(
		QPainter &p,
		const QPixmap &mask,
		const QColor *colorOverride);

	void stop();
	void unstop();
	void finish();
	void clearCache();
	bool finished() const {
		return _hiding && !_hide.animating();
	}

private:
	const style::RippleAnimation &_st;
	Fn<void()> _update;

	QPoint _origin;
	int _radiusFrom = 0;
	int _radiusTo = 0;

	bool _hiding = false;
	Ui::Animations::Simple _show;
	Ui::Animations::Simple _hide;
	QPixmap _cache;
	QImage _frame;
	bool _material = !AyuUiSettings::isTelegramDesign();
	std::vector<uchar> _noisePattern;
	int _noiseBlockW = 0;
	int _noiseBlockH = 0;

};

RippleAnimation::Ripple::Ripple(
	const style::RippleAnimation &st,
	QPoint origin,
	int startRadius,
	const QPixmap &mask,
	Fn<void()> update)
: _st(st)
, _update(std::move(update))
, _origin(origin)
, _radiusFrom(startRadius)
, _frame(mask.size(), QImage::Format_ARGB32_Premultiplied) {
	_frame.setDevicePixelRatio(mask.devicePixelRatio());

	const auto pixelRatio = style::DevicePixelRatio();
	QPoint points[] = {
		{ 0, 0 },
		{ _frame.width() / pixelRatio, 0 },
		{ _frame.width() / pixelRatio, _frame.height() / pixelRatio },
		{ 0, _frame.height() / pixelRatio },
	};
	for (auto point : points) {
		accumulate_max(
			_radiusTo,
			style::point::dotProduct(_origin - point, _origin - point));
	}
	if (!_material) {
		_radiusTo = qRound(sqrt(float64(_radiusTo)));
		_show.start(_update, 0., 1., _st.showDuration, anim::easeOutQuint);
		return;
	}
	_radiusTo = qRound(sqrt(float64(_radiusTo)) / 0.55);

	const auto w = _frame.width();
	const auto h = _frame.height();
	_noiseBlockW = (w + 1) / 2;
	_noiseBlockH = (h + 1) / 2;
	_noisePattern.resize(_noiseBlockW * _noiseBlockH);
	base::RandomFill(_noisePattern.data(), _noisePattern.size());

	_show.start(_update, 0., 1., _st.showDuration, anim::easeOutQuint);
}

RippleAnimation::Ripple::Ripple(const style::RippleAnimation &st, const QPixmap &mask, Fn<void()> update)
: _st(st)
, _update(std::move(update))
, _origin(
	mask.width() / (2 * style::DevicePixelRatio()),
	mask.height() / (2 * style::DevicePixelRatio()))
, _radiusFrom(mask.width() + mask.height())
, _frame(mask.size(), QImage::Format_ARGB32_Premultiplied) {
	_frame.setDevicePixelRatio(mask.devicePixelRatio());
	_radiusTo = _radiusFrom;
	_hide.start(_update, 0., 1., _st.hideDuration);
}

void RippleAnimation::Ripple::paint(
		QPainter &p,
		const QPixmap &mask,
		const QColor *colorOverride) {
	auto opacity = _hide.value(_hiding ? 0. : 1.);
	if (opacity == 0.) {
		return;
	}

	if (_cache.isNull() || colorOverride != nullptr) {
		const auto shown = _show.value(1.);
		Assert(!std::isnan(shown));
		const auto diff = float64(_radiusTo - _radiusFrom);
		Assert(!std::isnan(diff));
		const auto mult = diff * shown;
		Assert(!std::isnan(mult));
		if (_frame.isNull()) {
			// _frame is moved into _cache once the ripple is fully shown,
			// but a repaint with a color override regenerates it.
			_frame = QImage(mask.size(), QImage::Format_ARGB32_Premultiplied);
			_frame.setDevicePixelRatio(mask.devicePixelRatio());
		}
		const auto interpolated = _radiusFrom + mult;
		//anim::interpolateF(_radiusFrom, _radiusTo, shown);
		Assert(!std::isnan(interpolated));
		auto radius = int(base::SafeRound(interpolated));
		_frame.fill(Qt::transparent);
		{
			QPainter p(&_frame);
			p.setPen(Qt::NoPen);
			const auto color = colorOverride
				? *colorOverride
				: QColor(_st.color->c);
			if (!_material) {
				p.setBrush(color);
				PainterHighQualityEnabler hq(p);
				p.drawEllipse(_origin, radius, radius);
			} else {
				const auto safeRadius = std::max(radius, 1);
				const auto edgeWidth = 56.0;
				const auto innerStop = std::max(
					0.0,
					1.0 - edgeWidth / safeRadius);
				auto gradient = QRadialGradient(
					QPointF(_origin),
					safeRadius);
				gradient.setColorAt(0.0, color);
				gradient.setColorAt(innerStop, color);
				gradient.setColorAt(1.0, QColor(
					color.red(),
					color.green(),
					color.blue(),
					0));
				p.setBrush(gradient);
				{
					PainterHighQualityEnabler hq(p);
					p.drawEllipse(_origin, radius, radius);
				}
				auto noise = QImage(
					_frame.size(),
					QImage::Format_ARGB32_Premultiplied);
				noise.fill(Qt::transparent);
				noise.setDevicePixelRatio(_frame.devicePixelRatio());
				const auto light = QColor(
					color.red() + (255 - color.red()) * 3 / 10,
					color.green() + (255 - color.green()) * 3 / 10,
					color.blue() + (255 - color.blue()) * 3 / 10);
				auto noisePixels = reinterpret_cast<uint32_t*>(noise.bits());
				const auto bpl = noise.bytesPerLine() / 4;
				const auto w = _frame.width();
				const auto h = _frame.height();
				const auto blocksH = noisePixels ? _noiseBlockH : 0;
				for (auto by = 0; by < blocksH; ++by) {
					for (auto bx = 0; bx < _noiseBlockW; ++bx) {
						const auto random = _noisePattern[by * _noiseBlockW + bx];
						if (random > 230) {
							const auto alpha = (random - 230) * 80 / 25;
							const auto pixel = qPremultiply(qRgba(
								light.red(),
								light.green(),
								light.blue(),
								alpha));
							const auto y0 = by * 2;
							const auto x0 = bx * 2;
							noisePixels[y0 * bpl + x0] = pixel;
							if (x0 + 1 < w) {
								noisePixels[y0 * bpl + x0 + 1] = pixel;
							}
							if (y0 + 1 < h) {
								noisePixels[(y0 + 1) * bpl + x0] = pixel;
								if (x0 + 1 < w) {
									noisePixels[(y0 + 1) * bpl + x0 + 1] = pixel;
								}
							}
						}
					}
				}
				{
					const auto ratio = _frame.devicePixelRatio();
					const auto logicalW = w / ratio;
					const auto logicalH = h / ratio;
					QPainter noisePainter(&noise);
					const auto ringInward = 16.0;
					const auto ringFade = 8.0;
					const auto ringFullStop = std::max(
						0.01,
						innerStop - ringInward / safeRadius);
					const auto ringFadeStop = std::max(
						0.0,
						innerStop - (ringInward + ringFade) / safeRadius);
					auto noiseMask = QRadialGradient(
						QPointF(_origin),
						safeRadius);
					noiseMask.setColorAt(0.0, QColor(0, 0, 0, 0));
					if (ringFadeStop + 0.001 < ringFullStop) {
						noiseMask.setColorAt(
							ringFadeStop,
							QColor(0, 0, 0, 0));
					}
					noiseMask.setColorAt(ringFullStop, QColor(0, 0, 0, 255));
					noiseMask.setColorAt(1.0, QColor(0, 0, 0, 255));
					noisePainter.setCompositionMode(
						QPainter::CompositionMode_DestinationIn);
					noisePainter.setPen(Qt::NoPen);
					noisePainter.setBrush(noiseMask);
					noisePainter.drawRect(
						QRectF(0, 0, logicalW, logicalH));
				}
				p.setCompositionMode(QPainter::CompositionMode_SourceAtop);
				p.drawImage(0, 0, noise);
			}
			p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
			p.drawPixmap(0, 0, mask);
		}
		if (radius == _radiusTo && colorOverride == nullptr) {
			_cache = PixmapFromImage(std::move(_frame));
		}
	}
	auto saved = p.opacity();
	if (opacity != 1.) p.setOpacity(saved * opacity);
	if (_cache.isNull()) {
		p.drawImage(0, 0, _frame);
	} else {
		p.drawPixmap(0, 0, _cache);
	}
	if (opacity != 1.) p.setOpacity(saved);
}

void RippleAnimation::Ripple::stop() {
	_hiding = true;
	_hide.start(_update, 1., 0., _st.hideDuration);
}

void RippleAnimation::Ripple::unstop() {
	if (_hiding) {
		if (_hide.animating()) {
			_hide.start(_update, 0., 1., _st.hideDuration);
		}
		_hiding = false;
	}
}

void RippleAnimation::Ripple::finish() {
	if (_update) {
		_update();
	}
	_show.stop();
	_hide.stop();
}

void RippleAnimation::Ripple::clearCache() {
	_cache = QPixmap();
}

RippleAnimation::RippleAnimation(
	const style::RippleAnimation &st,
	QImage mask,
	Fn<void()> callback)
: _st(st)
, _mask(PixmapFromImage(std::move(mask)))
, _update(std::move(callback)) {
}


void RippleAnimation::add(QPoint origin, int startRadius) {
	lastStop();
	_ripples.push_back(
		std::make_unique<Ripple>(_st, origin, startRadius, _mask, _update));
}

void RippleAnimation::addFading() {
	lastStop();
	_ripples.push_back(std::make_unique<Ripple>(_st, _mask, _update));
}

void RippleAnimation::lastStop() {
	if (!_ripples.empty()) {
		_ripples.back()->stop();
	}
}

void RippleAnimation::lastUnstop() {
	if (!_ripples.empty()) {
		_ripples.back()->unstop();
	}
}

void RippleAnimation::lastFinish() {
	if (!_ripples.empty()) {
		_ripples.back()->finish();
	}
}

void RippleAnimation::forceRepaint() {
	for (const auto &ripple : _ripples) {
		ripple->clearCache();
	}
	if (_update) {
		_update();
	}
}

void RippleAnimation::paint(
		QPainter &p,
		int x,
		int y,
		int outerWidth,
		const QColor *colorOverride) {
	if (_ripples.empty()) {
		return;
	}

	if (style::RightToLeft()) {
		x = outerWidth - x - (_mask.width() / style::DevicePixelRatio());
	}
	p.translate(x, y);
	for (const auto &ripple : _ripples) {
		ripple->paint(p, _mask, colorOverride);
	}
	p.translate(-x, -y);
	clearFinished();
}

QImage RippleAnimation::MaskByDrawer(
		QSize size,
		bool filled,
		Fn<void(QPainter &p)> drawer) {
	auto result = QImage(
		size * style::DevicePixelRatio(),
		QImage::Format_ARGB32_Premultiplied);
	result.setDevicePixelRatio(style::DevicePixelRatio());
	result.fill(filled ? QColor(255, 255, 255) : Qt::transparent);
	if (drawer) {
		Painter p(&result);
		PainterHighQualityEnabler hq(p);

		p.setPen(Qt::NoPen);
		p.setBrush(QColor(255, 255, 255));
		drawer(p);
	}
	return result;
}

QImage RippleAnimation::RectMask(QSize size) {
	return MaskByDrawer(size, true, nullptr);
}

QImage RippleAnimation::RoundRectMask(QSize size, int radius) {
	return MaskByDrawer(size, false, [&](QPainter &p) {
		p.drawRoundedRect(0, 0, size.width(), size.height(), radius, radius);
	});
}

QImage RippleAnimation::RoundRectMask(
		QSize size,
		Images::CornersMaskRef corners) {
	return MaskByDrawer(size, true, [&](QPainter &p) {
		p.setCompositionMode(QPainter::CompositionMode_Source);
		const auto ratio = style::DevicePixelRatio();
		const auto corner = [&](int index, bool right, bool bottom) {
			if (const auto image = corners.p[index]) {
				if (!image->isNull()) {
					const auto width = image->width() / ratio;
					const auto height = image->height() / ratio;
					p.drawImage(
						QRect(
							right ? (size.width() - width) : 0,
							bottom ? (size.height() - height) : 0,
							width,
							height),
						*image);
				}
			}
		};
		corner(0, false, false);
		corner(1, true, false);
		corner(2, false, true);
		corner(3, true, true);
	});
}

QImage RippleAnimation::EllipseMask(QSize size) {
	return MaskByDrawer(size, false, [&](QPainter &p) {
		p.drawEllipse(0, 0, size.width(), size.height());
	});
}

void RippleAnimation::clearFinished() {
	while (!_ripples.empty() && _ripples.front()->finished()) {
		_ripples.pop_front();
	}
}

void RippleAnimation::clear() {
	_ripples.clear();
}

RippleAnimation::~RippleAnimation() = default;

} // namespace Ui

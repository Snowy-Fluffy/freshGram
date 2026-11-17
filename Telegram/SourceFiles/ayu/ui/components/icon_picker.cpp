// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/components/icon_picker.h"

#include "tray.h"
#include "ayu/ayu_settings.h"
#include "ayu/ui/ayu_logo.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "main/main_domain.h"
#include "styles/style_ayu_styles.h"
#include "styles/style_basic.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"
#include "ui/painter.h"
#include "ui/toast/toast.h"
#include "ui/widgets/popup_menu.h"
#include "window/main_window.h"

#include <QtGui/QContextMenuEvent>

#ifdef Q_OS_WIN
#include "ayu/utils/windows_utils.h"
#endif

#if !defined Q_OS_WIN && !defined Q_OS_MAC
#include "platform/platform_specific.h"
#endif // !Q_OS_WIN && !Q_OS_MAC

namespace {

const QVector<QString> builtInIcons{
	AyuAssets::DEFAULT_ICON,
	AyuAssets::ALT_ICON,
	AyuAssets::DISCORD_ICON,
	AyuAssets::SPOTIFY_ICON,
	AyuAssets::EXTERA_ICON,
	AyuAssets::NOTHING_ICON,
	AyuAssets::BARD_ICON,
	AyuAssets::YAPLUS_ICON,
	AyuAssets::WIN95_ICON,
	AyuAssets::CHIBI_ICON,
	AyuAssets::CHIBI2_ICON,
	AyuAssets::EXTERA2_ICON,
	AyuAssets::PLANE_ICON,
	AyuAssets::WHEELOLD_ICON,
	AyuAssets::SEGMENT_ICON,
};

[[nodiscard]] QString CustomIconFilter() {
	return u"Image files (*.png *.jpg *.jpeg *.webp *.bmp *.gif *.svg)"_q
		+ u";;"_q
		+ FileDialog::AllFilesFilter();
}

void applyIcon() {
#ifdef Q_OS_WIN
	AyuAssets::loadAppIco();
	reloadAppIconFromTaskBar();
#endif

#if !defined Q_OS_WIN && !defined Q_OS_MAC
	Platform::UpdateLauncherIcon();
#endif // !Q_OS_WIN && !Q_OS_MAC

	Window::OverrideApplicationIcon(AyuAssets::currentAppLogo());
	Core::App().refreshApplicationIcon();
	Core::App().tray().updateIconCounters();
	Core::App().domain().notifyUnreadBadgeChanged();
}

} // namespace

IconPicker::IconPicker(QWidget *parent)
	: RpWidget(parent) {
	setMouseTracking(true);
	refreshIcons();
	widthValue() | rpl::on_next([=](int w) {
		updateHeight(w);
	}, lifetime());
}

IconPicker::~IconPicker() = default;

void IconPicker::refreshIcons() {
	_icons.assign(builtInIcons.begin(), builtInIcons.end());
	for (const auto &name : AyuAssets::customIcons()) {
		_icons.push_back(name);
	}
	updateHeight(width());
	update();
}

int IconPicker::rows() const {
	const auto cells = static_cast<int>(_icons.size()) + 1;
	return (cells + kColumns - 1) / kColumns;
}

void IconPicker::updateHeight(int w) {
	const auto cell = w / kColumns;
	const auto iconSize = st::iconPickerIconSize;
	const auto contentSize = iconSize + st::iconPickerImagePadding * 2;
	const auto h = rows() * cell - (cell - contentSize);
	if (height() != h) {
		resize(w, h);
	}
}

bool IconPicker::isAddCell(int index) const {
	return (index == static_cast<int>(_icons.size()));
}

void IconPicker::drawIcon(QPainter &p, const QImage &icon, int x, int y, float strokeOpacity) {
	{
		PainterHighQualityEnabler hq(p);
		p.save();
		p.setPen(QPen(st::boxDividerBg, 0));
		p.setBrush(QBrush(st::boxDividerBg));
		p.setOpacity(strokeOpacity);
		p.drawRoundedRect(
			x + st::iconPickerSelectedPadding,
			y + st::iconPickerSelectedPadding,
			st::iconPickerIconSize + st::iconPickerSelectedPadding * 2,
			st::iconPickerIconSize + st::iconPickerSelectedPadding * 2,
			st::iconPickerSelectedRounding,
			st::iconPickerSelectedRounding
		);
		p.restore();
	}

	const auto rect = QRect(
		x + st::iconPickerImagePadding,
		y + st::iconPickerImagePadding,
		st::iconPickerIconSize,
		st::iconPickerIconSize
	);
	p.drawImage(rect, icon);
}

void IconPicker::drawAddButton(QPainter &p, int x, int y) {
	const auto inner = st::iconPickerImagePadding * 2;
	const auto rect = QRect(
		x + inner,
		y + inner,
		st::iconPickerIconSize - inner * 2 + st::iconPickerImagePadding * 2,
		st::iconPickerIconSize - inner * 2 + st::iconPickerImagePadding * 2);

	PainterHighQualityEnabler hq(p);
	p.save();
	p.setPen(Qt::NoPen);
	p.setBrush(st::boxDividerBg);
	p.drawEllipse(rect);

	const auto half = rect.width() / 6;
	const auto center = rect.center();
	auto pen = st::windowSubTextFg->p;
	pen.setWidth(st::lineWidth * 3);
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	p.drawLine(center.x() - half, center.y(), center.x() + half, center.y());
	p.drawLine(center.x(), center.y() - half, center.x(), center.y() + half);
	p.restore();
}

int IconPicker::cellWidth() const {
	return width() / kColumns;
}

int IconPicker::cellAt(QPoint position) const {
	const auto cell = cellWidth();
	const auto iconSize = st::iconPickerIconSize;
	const auto cells = static_cast<int>(_icons.size()) + 1;

	for (auto idx = 0; idx < cells; ++idx) {
		const auto x = (idx % kColumns) * cell + (cell - iconSize) / 2;
		const auto y = (idx / kColumns) * cell;
		if (position.x() >= x && position.x() <= x + iconSize
			&& position.y() >= y && position.y() <= y + iconSize) {
			return idx;
		}
	}
	return -1;
}

void IconPicker::paintEvent(QPaintEvent *e) {
	Painter p(this);

	const auto cell = cellWidth();
	const auto iconSize = st::iconPickerIconSize;
	const auto cells = static_cast<int>(_icons.size()) + 1;

	for (auto idx = 0; idx < cells; ++idx) {
		const auto x = (idx % kColumns) * cell + (cell - iconSize) / 2;
		const auto y = (idx / kColumns) * cell;

		if (isAddCell(idx)) {
			drawAddButton(p, x, y);
			continue;
		}

		const auto &iconName = _icons[idx];
		QImage icon;
		if (const auto cached = _cachedIcons.find(iconName); cached != _cachedIcons.end()) {
			icon = cached->second;
		} else {
			icon = _cachedIcons[iconName] = AyuAssets::loadPreview(iconName);
		}
		auto opacity = 0.0f;
		if (iconName == _wasSelected) {
			opacity = 1.0f - _animation.value(1.0f);
		} else if (iconName == AyuAssets::currentAppLogoName()) {
			opacity = _wasSelected.isEmpty() ? 1.0f : _animation.value(1.0f);
		}

		drawIcon(p, icon, x, y, opacity);
	}
}

void IconPicker::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto idx = cellAt(e->pos());
	if (idx < 0) {
		return;
	} else if (isAddCell(idx)) {
		chooseCustomIcon();
	} else {
		selectIcon(_icons[idx]);
	}
}

void IconPicker::mouseMoveEvent(QMouseEvent *e) {
	setCursor((cellAt(e->pos()) >= 0)
		? style::cur_pointer
		: style::cur_default);
}

void IconPicker::contextMenuEvent(QContextMenuEvent *e) {
	const auto idx = cellAt(e->pos());
	if (idx < 0 || isAddCell(idx)) {
		return;
	}
	const auto name = _icons[idx];
	if (!AyuAssets::isCustomIcon(name)) {
		return;
	}
	_menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuWithIcons);
	_menu->addAction(
		tr::ayu_AppIconRemoveCustom(tr::now),
		crl::guard(this, [=] { removeCustomIcon(name); }),
		&st::menuIconDelete);
	_menu->popup(e->globalPos());
}

void IconPicker::selectIcon(const QString &name) {
	auto &settings = AyuSettings::getInstance();
	if (settings.appIcon() == name) {
		return;
	}
	_wasSelected = settings.appIcon();
	_animation.start(
		[=]
		{
			update();
		},
		0.0,
		1.0,
		200,
		anim::easeOutCubic
	);

	settings.setAppIcon(name);
	applyIcon();

	repaint();
}

void IconPicker::chooseCustomIcon() {
	const auto callback = crl::guard(this, [=](
			FileDialog::OpenResult &&result) {
		const auto path = result.paths.isEmpty()
			? QString()
			: result.paths.front();
		const auto name = AyuAssets::importCustomIcon(
			path,
			result.remoteContent);
		if (name.isEmpty()) {
			Ui::Toast::Show(tr::ayu_AppIconLoadFailed(tr::now));
			return;
		}
		refreshIcons();
		selectIcon(name);
	});
	FileDialog::GetOpenPath(
		this,
		tr::ayu_AppIconChooseFile(tr::now),
		CustomIconFilter(),
		callback);
}

void IconPicker::removeCustomIcon(const QString &name) {
	auto &settings = AyuSettings::getInstance();
	if (settings.appIcon() == name) {
		_wasSelected = QString();
		_animation.stop();
		settings.setAppIcon(AyuAssets::DEFAULT_ICON);
		applyIcon();
	}
	AyuAssets::removeCustomIcon(name);
	_cachedIcons.erase(name);
	if (_wasSelected == name) {
		_wasSelected = QString();
	}
	refreshIcons();
}

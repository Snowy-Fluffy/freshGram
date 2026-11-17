// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "base/unique_qptr.h"
#include "ui/rp_widget.h"
#include "ui/effects/animations.h"

namespace Ui {
class PopupMenu;
}

class IconPicker : public Ui::RpWidget
{
public:
	IconPicker(QWidget *parent);
	~IconPicker();

	static constexpr int kColumns = 4;

protected:
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;

private:
	void drawIcon(QPainter &p, const QImage &icon, int x, int y, float strokeOpacity);
	void drawAddButton(QPainter &p, int x, int y);
	[[nodiscard]] int cellWidth() const;
	[[nodiscard]] int rows() const;
	[[nodiscard]] int cellAt(QPoint position) const;
	[[nodiscard]] bool isAddCell(int index) const;

	void refreshIcons();
	void updateHeight(int width);
	void selectIcon(const QString &name);
	void chooseCustomIcon();
	void removeCustomIcon(const QString &name);

	std::vector<QString> _icons;
	Ui::Animations::Simple _animation;
	QString _wasSelected;
	std::unordered_map<QString, QImage> _cachedIcons;
	base::unique_qptr<Ui::PopupMenu> _menu;
};

/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/rp_widget.h"
#include "ui/effects/animations.h"
#include "ui/layers/box_content.h"
#include "base/bytes.h"
#include "base/timer.h"

namespace base {
enum class SystemUnlockResult;
} // namespace base

namespace Ui {
class PasswordInput;
class LinkButton;
class FlatLabel;
class RoundButton;
class IconButton;
class CheckView;
} // namespace Ui

namespace Main {
class Session;
} // namespace Main

namespace Window {

class Controller;
class SlideAnimation;

enum class PasscodeAttempt : uchar {
	Empty,
	Flood,
	Wrong,
	Wiped,
	Unavailable,
	Correct,
};

// Bumps the bad tries counters on a wrong passcode. Does not unlock:
// Core::App().unlockPasscode() destroys the main lock screen widget, so
// the caller runs it as its own last statement.
[[nodiscard]] PasscodeAttempt TryPasscode(const QString &passcode);

// Derives the keys on a thread, then checks them on the main thread.
void TryPasscodeAsync(
	const QString &passcode,
	Fn<void(PasscodeAttempt)> done);

class LockWidget : public Ui::RpWidget {
public:
	LockWidget(QWidget *parent, not_null<Controller*> window);
	~LockWidget();

	[[nodiscard]] not_null<Controller*> window() const;

	virtual void setInnerFocus();

	void showAnimated(QPixmap oldContentCache);
	void showFinished();

protected:
	void paintEvent(QPaintEvent *e) override;
	virtual void paintContent(QPainter &p);
	virtual void restoreChildrenVisibility() {
	}

private:
	const not_null<Controller*> _window;
	std::unique_ptr<SlideAnimation> _showAnimation;

};

class PasscodeLockWidget : public LockWidget {
public:
	PasscodeLockWidget(
		QWidget *parent,
		not_null<Controller*> window,
		bool preview = false);

	void setInnerFocus() override;

protected:
	void resizeEvent(QResizeEvent *e) override;
	bool eventFilter(QObject *o, QEvent *e) override;

private:
	enum class SystemUnlockType : uchar {
		None,
		Default,
		Biometrics,
		Companion,
	};

	void paintContent(QPainter &p) override;
	void restoreChildrenVisibility() override;
	void paintBackground(QPainter &p);
	void paintCardContent(QPainter &p);
	void paintTelegramContent(QPainter &p);
	void paintCentered(
		QPainter &p,
		const style::font &font,
		QRect area,
		const QString &text);

	void setupSettings();
	void applySettings();
	void setupSystemUnlockInfo();
	void setupSystemUnlock();
	void suggestSystemUnlock();
	void systemUnlockDone(base::SystemUnlockResult result);
	void changed();
	void submit();
	void error();
	void setupEye();
	void updateLayout();
	void updateCardLayout();
	void updateTelegramLayout();
	void updateClock();
	void updateStatus();
	void updateCapsLock();
	void setCapsLock(bool on);
	void setBusy(bool busy);
	void shake();
	void paintStatus(QPainter &p, const style::font &font, int top, int height);
	void updateStatusScroll();

	rpl::variable<SystemUnlockType> _systemUnlockAvailable;
	rpl::variable<SystemUnlockType> _systemUnlockAllowed;
	object_ptr<Ui::PasswordInput> _passcode;
	object_ptr<Ui::RoundButton> _submit;
	object_ptr<Ui::LinkButton> _logout;
	object_ptr<Ui::FlatLabel> _text;
	Ui::IconButton *_eye = nullptr;
	QString _error;
	QString _status;
	Ui::Animations::Basic _statusScroll;
	int _statusTextWidth = 0;
	QString _clock;
	QString _title;
	style::font _clockFont;
	QString _date;
	QRect _card;
	rpl::variable<int> _contentBottom = 0;
	int _clockTop = 0;
	int _dateTop = 0;
	int _lockTop = 0;
	int _titleTop = 0;
	int _statusTop = 0;
	int _shift = 0;
	bool _statusError = false;
	bool _capsLock = false;
	bool _busy = false;
	bool _clockShown = false;
	bool _dateShown = false;
	bool _imageShown = false;
	bool _eyeShown = true;
	std::optional<QColor> _backdropText;
	bool _telegramStyle = false;
	bool _logoutShown = true;
	bool _textShown = false;
	Ui::Animations::Simple _shakeAnimation;
	base::Timer _clockTimer;
	base::Timer _statusTimer;

	rpl::lifetime _systemUnlockSuggested;
	base::Timer _systemUnlockCooldown;
	const bool _preview = false;

};

struct TermsLock {
	bytes::vector id;
	TextWithEntities text;
	std::optional<int> minAge;
	bool popup = false;

	inline bool operator==(const TermsLock &other) const {
		return (id == other.id);
	}
	inline bool operator!=(const TermsLock &other) const {
		return !(*this == other);
	}

	static TermsLock FromMTP(
		Main::Session *session,
		const MTPDhelp_termsOfService &data);

};

class TermsBox : public Ui::BoxContent {
public:
	TermsBox(
		QWidget*,
		const TermsLock &data,
		rpl::producer<QString> agree,
		rpl::producer<QString> cancel);
	TermsBox(
		QWidget*,
		const TextWithEntities &text,
		rpl::producer<QString> agree,
		rpl::producer<QString> cancel,
		bool attentionAgree = false);

	rpl::producer<> agreeClicks() const;
	rpl::producer<> cancelClicks() const;
	QString lastClickedMention() const;

protected:
	void prepare() override;

	void keyPressEvent(QKeyEvent *e) override;

private:
	TermsLock _data;
	rpl::producer<QString> _agree;
	rpl::producer<QString> _cancel;
	rpl::event_stream<> _agreeClicks;
	rpl::event_stream<> _cancelClicks;
	QString _lastClickedMention;
	bool _attentionAgree = false;

	bool _ageErrorShown = false;
	Ui::Animations::Simple _ageErrorAnimation;

};

} // namespace Window

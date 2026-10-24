// This is the source code of freshGram.
//
// The list of downloadable recognition models, a sibling of the
// "Manage dictionaries" box of the spellchecker.
#include "ayu/ui/boxes/recognition_models_box.h"

#include "ayu/ayu_settings.h"
#include "base/event_filter.h"
#include "lang/lang_keys.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/multi_select.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/effects/animations.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_menu_icons.h"

namespace AyuUi {
namespace {

using namespace FreshRecognition;
using namespace Storage::CloudBlob;

constexpr auto kMaxQueryLength = 15;

[[nodiscard]] QString ModelTitle(const Model &model) {
	return (model.kind == ModelKind::Whisper)
		? (u"Whisper "_q + model.name)
		: model.name;
}

[[nodiscard]] BlobState ComputeState(const Model &model, bool enabled) {
	if (!ModelExists(model)) {
		return Available{ model.size };
	}
	return enabled ? BlobState(Active()) : BlobState(Ready());
}

struct Selection {
	QString value;
	std::vector<Model> pending;
};

class Inner final : public Ui::RpWidget {
public:
	Inner(
		QWidget *parent,
		not_null<Main::Session*> session,
		ModelKind kind);

	[[nodiscard]] Selection selection() const;
	[[nodiscard]] bool changed() const;
	[[nodiscard]] Fn<void(const QString &)> queryCallback() const;

private:
	void setupContent(not_null<Main::Session*> session);
	void rebuildRows(
		not_null<Main::Session*> session,
		const QStringList &selected);
	void addRow(
		not_null<Main::Session*> session,
		const Model &model,
		bool enabled,
		bool automatic = false);
	[[nodiscard]] QString keyOf(const Model &model) const;
	[[nodiscard]] QStringList currentKeys() const;
	[[nodiscard]] QString savedValue() const;

	const ModelKind _kind;
	Ui::VerticalLayout *_content = nullptr;
	Ui::SlideWrap<Ui::FlatLabel> *_loading = nullptr;
	std::vector<int> _builtIds;
	std::vector<int> _enabledIds;
	std::vector<Model> _models;
	std::optional<Model> _aux;
	int _lastSingleId = 0;
	bool _changed = false;
	rpl::event_stream<QStringView> _query;
	rpl::event_stream<int> _selected;
	Fn<void(const QString &)> _queryCallback;

};

Inner::Inner(
	QWidget *parent,
	not_null<Main::Session*> session,
	ModelKind kind)
: RpWidget(parent)
, _kind(kind) {
	setupContent(session);
}

bool Inner::changed() const {
	return _changed;
}

Fn<void(const QString &)> Inner::queryCallback() const {
	return _queryCallback;
}

QString Inner::keyOf(const Model &model) const {
	return (model.kind == ModelKind::Whisper)
		? model.file
		: (model.kind == ModelKind::TesseractAux)
		? kAutoOcrLanguages.utf16()
		: model.code;
}

QString Inner::savedValue() const {
	const auto &settings = AyuSettings::getInstance();
	return (_kind == ModelKind::Whisper)
		? settings.whisperModel()
		: (_kind == ModelKind::Translate)
		? QString()
		: settings.ocrLanguages();
}

QStringList Inner::currentKeys() const {
	auto result = QStringList();
	if (_aux && ranges::contains(_enabledIds, _aux->id)) {
		result.push_back(keyOf(*_aux));
	}
	for (const auto &model : _models) {
		if (ranges::contains(_enabledIds, model.id)) {
			result.push_back(keyOf(model));
		}
	}
	return result;
}

Selection Inner::selection() const {
	auto result = Selection();
	const auto enabled = [&](const Model &model) {
		return ranges::contains(_enabledIds, model.id);
	};
	if (_kind == ModelKind::Whisper) {
		const Model *chosen = nullptr;
		const Model *pending = nullptr;
		for (const auto &model : _models) {
			if (!enabled(model)) {
				continue;
			} else if (!ModelExists(model)) {
				pending = &model;
			} else if (!chosen || model.id == _lastSingleId) {
				chosen = &model;
			}
		}
		if (chosen) {
			result.value = chosen->file;
		} else if (pending) {
			result.value = savedValue();
			result.pending.push_back(*pending);
		}
		return result;
	}
	auto codes = QStringList();
	for (const auto &model : _models) {
		if (!enabled(model)) {
			continue;
		} else if (ModelExists(model)) {
			codes.push_back(model.code);
		} else {
			result.pending.push_back(model);
		}
	}
	if (_aux && enabled(*_aux)) {
		// The data of the detection comes later, see ApplyDownloaded.
		if (ModelExists(*_aux)) {
			codes.push_front(kAutoOcrLanguages.utf16());
		} else {
			result.pending.push_back(*_aux);
		}
	}
	result.value = codes.join('+');
	return result;
}

void Inner::addRow(
		not_null<Main::Session*> session,
		const Model &model,
		bool enabled,
		bool automatic) {
	const auto content = _content;
	const auto id = model.id;
	const auto title = automatic
		? tr::fresh_OcrAuto(tr::now)
		: ModelTitle(model);
	const auto single = (_kind == ModelKind::Whisper);
	enabled = (enabled && ModelExists(model)) || (ModelDownload(id) != nullptr);
	if (enabled) {
		_enabledIds.push_back(id);
	}

	const auto wrap = content->add(
		object_ptr<Ui::SlideWrap<Ui::SettingsButton>>(
			content,
			object_ptr<Ui::SettingsButton>(
				content,
				rpl::single(title),
				st::dictionariesSectionButton)));
	const auto button = wrap->entity();

	const auto index = std::vector<QString>{ title, model.name, model.code };
	_query.events(
	) | rpl::on_next([=](QStringView string) {
		wrap->toggle(
			ranges::any_of(index, [&](const QString &s) {
				return s.startsWith(string, Qt::CaseInsensitive);
			}),
			anim::type::instant);
	}, button->lifetime());

	struct State {
		rpl::variable<BlobState> state;
		rpl::event_stream<bool> forced;
		rpl::lifetime loaderLifetime;
	};
	const auto state = button->lifetime().make_state<State>();
	state->state = ComputeState(model, enabled);

	const auto label = Ui::CreateChild<Ui::FlatLabel>(
		button,
		state->state.value() | rpl::map([](const BlobState &value) {
			return StateDescription(value, tr::fresh_ModelEnabled);
		}),
		st::settingsUpdateState);
	label->setAttribute(Qt::WA_TransparentForMouseEvents);
	label->show();
	label->moveToLeft(
		st::settingsUpdateStatePosition.x(),
		st::settingsUpdateStatePosition.y());

	state->state.value(
	) | rpl::on_next([=](const BlobState &value) {
		const auto toggled = v::is<Active>(value) ? 1. : 0.;
		const auto over = !button->isDisabled()
			&& (button->isDown() || button->isOver());
		if (toggled == 0. && !over) {
			label->setTextColorOverride(std::nullopt);
		} else {
			label->setTextColorOverride(anim::color(
				over ? st::contactsStatusFgOver : st::contactsStatusFg,
				st::contactsStatusFgOnline,
				toggled));
		}
	}, label->lifetime());

	button->toggleOn(
		rpl::single(enabled) | rpl::then(state->forced.events()));
	button->clicks(
	) | rpl::on_next([=] {
		_changed = true;
	}, button->lifetime());

	const auto setEnabled = [=](bool value) {
		if (value) {
			if (!ranges::contains(_enabledIds, id)) {
				_enabledIds.push_back(id);
			}
		} else {
			_enabledIds.erase(
				ranges::remove(_enabledIds, id),
				end(_enabledIds));
		}
	};
	const auto attach = [=] {
		state->loaderLifetime.destroy();
		if (const auto loader = ModelDownload(id)) {
			loader->state(
			) | rpl::on_next([=](const BlobState &value) {
				state->state = value;
			}, state->loaderLifetime);
		}
	};

	button->toggledValue(
	) | rpl::on_next([=](bool toggled) {
		setEnabled(toggled);
		if (toggled) {
			if (single) {
				_lastSingleId = id;
				_selected.fire_copy(id);
			}
			if (ModelExists(model)) {
				state->state = ComputeState(model, true);
				return;
			}
			StartModelDownload(session, model);
			attach();
		} else {
			if (ModelDownload(id)) {
				CancelModelDownload(id);
			} else if (_kind == ModelKind::Translate && ModelExists(model)) {
				RemoveModel(model);
			}
			state->loaderLifetime.destroy();
			if (!v::is<Failed>(state->state.current())) {
				state->state = ComputeState(model, false);
			}
		}
	}, button->lifetime());

	ModelDownloadFinished(
	) | rpl::filter([=](int finished) {
		return finished == id;
	}) | rpl::on_next([=] {
		state->loaderLifetime.destroy();
		if (ModelExists(model)) {
			state->state = ComputeState(model, button->toggled());
		} else {
			state->state = BlobState(Failed());
			state->forced.fire(false);
		}
	}, button->lifetime());

	if (single) {
		_selected.events(
		) | rpl::on_next([=](int selectedId) {
			if (selectedId != id && button->toggled()) {
				state->forced.fire(false);
			}
		}, button->lifetime());
	}

	const auto contextMenu = button->lifetime()
		.make_state<base::unique_qptr<Ui::PopupMenu>>();
	const auto showMenu = [=] {
		if (!ModelExists(model)) {
			return false;
		}
		*contextMenu = base::make_unique_q<Ui::PopupMenu>(
			button,
			st::popupMenuWithIcons);
		contextMenu->get()->addAction(
			tr::fresh_ModelRemove(tr::now),
			[=] {
				_changed = true;
				RemoveModel(model);
				setEnabled(false);
				state->state = ComputeState(model, false);
				state->forced.fire(false);
			},
			&st::menuIconDelete);
		contextMenu->get()->popup(QCursor::pos());
		return true;
	};
	base::install_event_filter(button, [=](not_null<QEvent*> e) {
		if (e->type() == QEvent::ContextMenu && showMenu()) {
			return base::EventFilterResult::Cancel;
		}
		return base::EventFilterResult::Continue;
	});
}

void Inner::rebuildRows(
		not_null<Main::Session*> session,
		const QStringList &selected) {
	auto models = Models(_kind);
	if (models.empty()) {
		return;
	}
	auto aux = std::optional<Model>();
	if (_kind == ModelKind::Tesseract) {
		const auto list = Models(ModelKind::TesseractAux);
		if (!list.empty()) {
			aux = list.front();
		}
	}
	auto ids = std::vector<int>();
	if (aux) {
		ids.push_back(aux->id);
	}
	for (const auto &model : models) {
		ids.push_back(model.id);
	}
	if (ids == _builtIds) {
		return;
	}
	_builtIds = std::move(ids);
	_loading->hide(anim::type::instant);
	while (_content->count() > 1) {
		delete _content->widgetAt(1);
	}
	_enabledIds.clear();
	_models = std::move(models);
	_aux = std::move(aux);

	const auto autoChosen = _aux
		&& selected.contains(kAutoOcrLanguages.utf16());
	if (_aux) {
		addRow(session, *_aux, autoChosen, true);
	}
	for (const auto &model : _models) {
		addRow(
			session,
			model,
			selected.contains(keyOf(model)));
	}
	_content->resizeToWidth(st::boxWidth);
}

void Inner::setupContent(not_null<Main::Session*> session) {
	_content = Ui::CreateChild<Ui::VerticalLayout>(this);

	// Called when the list of models is known, the first time.
	const auto selected = [=] {
		const auto &settings = AyuSettings::getInstance();
		if (_kind == ModelKind::Translate) {
			auto codes = QStringList();
			for (const auto &model : InstalledTranslateModels()) {
				codes.push_back(model.code);
			}
			return codes;
		}
		if (_kind == ModelKind::Whisper) {
			return QStringList{ settings.whisperModel() };
		}
		const auto &value = settings.ocrLanguages();
		if (value == kAutoOcrLanguages.utf16()) {
			// Just "auto" means all the downloaded languages.
			return QStringList{ value } + InstalledOcrLanguages();
		}
		return value.split('+', Qt::SkipEmptyParts);
	};

	_loading = _content->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			_content,
			object_ptr<Ui::FlatLabel>(
				_content,
				tr::fresh_ModelsLoading(),
				st::defaultFlatLabel)),
		st::boxRowPadding);
	const auto rebuild = [=] {
		rebuildRows(session, _builtIds.empty() ? selected() : currentKeys());
	};
	rebuild();
	ModelsChanged(
	) | rpl::on_next(rebuild, _content->lifetime());
	ManifestFinished(
	) | rpl::on_next([=](bool loaded) {
		if (!loaded && _builtIds.empty()) {
			_loading->entity()->setText(tr::fresh_ModelsLoadFailed(tr::now));
		}
	}, _content->lifetime());
	RefreshManifest(session);

	_queryCallback = [=](const QString &query) {
		if (query.size() >= kMaxQueryLength) {
			return;
		}
		_query.fire_copy(query);
	};

	_content->resizeToWidth(st::boxWidth);
	Ui::ResizeFitChild(this, _content);
}

} // namespace

RecognitionModelsBox::RecognitionModelsBox(
	QWidget*,
	not_null<Main::Session*> session,
	ModelKind kind)
: _session(session)
, _kind(kind) {
}

void RecognitionModelsBox::setInnerFocus() {
	if (_setInnerFocus) {
		_setInnerFocus();
	}
}

void RecognitionModelsBox::prepare() {
	const auto multiSelect = Ui::CreateChild<Ui::MultiSelect>(
		this,
		st::defaultMultiSelect,
		tr::lng_participant_filter());
	multiSelect->resizeToWidth(st::boxWidth);
	multiSelect->moveToLeft(0, 0);

	const auto inner = setInnerWidget(
		object_ptr<Inner>(this, _session, _kind),
		st::boxScroll,
		multiSelect->height());
	multiSelect->setQueryChangedCallback(inner->queryCallback());
	_setInnerFocus = [=] {
		multiSelect->setInnerFocus();
	};

	setTitle((_kind == ModelKind::Whisper)
		? tr::fresh_ManageWhisperModels()
		: (_kind == ModelKind::Translate)
		? tr::fresh_ManageTranslateModels()
		: tr::fresh_ManageOcrLanguages());

	// WHY: the toggles look applied at once, so any way of closing saves.
	const auto kind = _kind;
	boxClosing() | rpl::on_next([=] {
		if (!inner->changed()) {
			return;
		}
		const auto selection = inner->selection();
		auto &settings = AyuSettings::getInstance();
		if (kind == ModelKind::Whisper) {
			settings.setWhisperModel(selection.value);
		} else if (kind == ModelKind::Tesseract) {
			settings.setOcrLanguages(selection.value);
		}
		for (const auto &model : selection.pending) {
			WantModel(model);
		}
	}, lifetime());
	addButton(tr::lng_box_done(), [=] { closeBox(); });

	setDimensionsToContent(st::boxWidth, inner);

	using namespace rpl::mappers;
	const auto max = lifetime().make_state<int>(0);
	rpl::combine(
		inner->heightValue(),
		multiSelect->heightValue(),
		_1 + _2
	) | rpl::on_next([=](int height) {
		accumulate_max(*max, height);
		setDimensions(
			st::boxWidth,
			std::min(*max, st::boxMaxListHeight),
			true);
	}, inner->lifetime());
}

} // namespace AyuUi

// This is the source code of freshGram.
#pragma once

#include "ayu/features/recognition/recognition_models.h"
#include "ui/layers/box_content.h"

namespace Main {
class Session;
} // namespace Main

namespace AyuUi {

class RecognitionModelsBox : public Ui::BoxContent {
public:
	RecognitionModelsBox(
		QWidget*,
		not_null<Main::Session*> session,
		FreshRecognition::ModelKind kind);

protected:
	void prepare() override;
	void setInnerFocus() override;

private:
	const not_null<Main::Session*> _session;
	const FreshRecognition::ModelKind _kind;
	Fn<void()> _setInnerFocus;

};

} // namespace AyuUi

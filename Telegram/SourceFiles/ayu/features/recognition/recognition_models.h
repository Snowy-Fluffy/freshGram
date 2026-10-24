// This is the source code of freshGram.
//
// Models for the local speech and text recognition are downloaded from
// a Telegram channel, like the spellchecker dictionaries.
#pragma once

#include "storage/storage_cloud_blob.h"

#include <QtCore/QHash>

#include <optional>

namespace Main {
class Session;
} // namespace Main

namespace FreshRecognition {

enum class RecognitionError {
	None,
	Unavailable, // Not compiled in this build.
	NoLanguages,
	UnsupportedCpu,
	Integrity,
	LoadFailed,
	Decode,
	TooLarge,
	NoData,
	Failed,
};

[[nodiscard]] QString RecognitionErrorText(RecognitionError error);

enum class ModelKind {
	Whisper,
	Tesseract,
	TesseractAux, // The data for the automatic language detection.
	Translate, // An Opus-MT model of one direction, like "ru-en".
};

struct Model : Storage::CloudBlob::Blob {
	ModelKind kind = ModelKind::Whisper;
	QString file; // The file name, both in the channel and on disk.
	QString code; // Language code of a Tesseract model.
	QString sha256;
	QString channel;
};

[[nodiscard]] QString ModelsPath();
[[nodiscard]] QString TessdataPath();
[[nodiscard]] QString TranslatePath();
[[nodiscard]] QString ModelPath(const Model &model);
[[nodiscard]] bool ModelExists(const Model &model);
bool RemoveModel(const Model &model);

[[nodiscard]] std::vector<Model> Models(ModelKind kind);
[[nodiscard]] rpl::producer<> ModelsChanged();
void RefreshManifest(not_null<Main::Session*> session);

// The list of models is loaded, from the cache or from the channel.
[[nodiscard]] bool ModelsKnown();

// Loads the list of models from the channel if it is not known yet.
// The callback is called in the main thread, whatever the result.
void LoadModelsList(not_null<Main::Session*> session, Fn<void()> done);

[[nodiscard]] MTP::DedicatedLoader::Location ModelLocation(
	const Model &model);

[[nodiscard]] std::optional<Model> SelectedWhisperModel();

// Checks the SHA-256 of a file, remembered until it changes; thread-safe.
[[nodiscard]] bool ModelFileIntact(
	const QString &path,
	const QString &sha256);

[[nodiscard]] QHash<QString, QString> TesseractHashes();

// The part of the setting meaning "choose the languages automatically".
// The setting is "auto" (among all downloaded languages), "auto+rus+eng"
// (among these ones) or just the languages, like "rus+eng".
inline constexpr auto kAutoOcrLanguages = "auto"_cs;

[[nodiscard]] bool IsAutoOcr(const QString &value);
[[nodiscard]] QStringList OcrCodes(const QString &value);

// The languages to recognize with, in the format of the setting, only
// with the downloaded data. Falls back to the downloaded languages if
// the setting has none of them, empty only if nothing is downloaded.
[[nodiscard]] QString EnabledOcrLanguages();

// Up to three of the languages, English is the last one.
[[nodiscard]] QString DefaultOcrLanguages(const QStringList &installed);

// All downloaded Tesseract languages.
[[nodiscard]] QStringList InstalledOcrLanguages();

// Downloaded translation models by their direction code, like "ru-en".
[[nodiscard]] std::vector<Model> InstalledTranslateModels();

class ModelLoader : public Storage::CloudBlob::BlobLoader {
public:
	ModelLoader(
		QObject *parent,
		not_null<Main::Session*> session,
		const Model &model,
		Fn<void()> destroyCallback);

	void destroy() override;

	rpl::lifetime &lifetime() {
		return _lifetime;
	}

private:
	void unpack(const QString &path) override;
	void fail() override;

	const Model _model;
	// Be sure to always call it in the main thread.
	Fn<void()> _destroyCallback;
	rpl::lifetime _lifetime;

};

// The downloads go on after the models box is closed.
[[nodiscard]] ModelLoader *ModelDownload(int id);
void StartModelDownload(
	not_null<Main::Session*> session,
	const Model &model);
void CancelModelDownload(int id);
[[nodiscard]] rpl::producer<int> ModelDownloadFinished();

// Selects the model when its download finishes.
void WantModel(const Model &model);

[[nodiscard]] rpl::producer<bool> ManifestFinished();

} // namespace FreshRecognition

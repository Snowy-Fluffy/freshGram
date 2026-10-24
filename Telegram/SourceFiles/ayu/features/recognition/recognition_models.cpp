// This is the source code of freshGram.
#include "ayu/features/recognition/recognition_models.h"

#include "ayu/ayu_settings.h"
#include "ayu/data/fresh_vault.h"
#include "base/unique_qptr.h"
#include "base/weak_ptr.h"
#include "lang/lang_keys.h"
#include "settings.h"
#include "logs.h"
#include "main/main_session.h"
#include "mtproto/dedicated_file_loader.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>

#include <map>
#include <mutex>

namespace FreshRecognition {
namespace {

constexpr auto kModelsChannel = "freshgram_models"_cs;
constexpr auto kFallbackManifestPostId = 22;
constexpr auto kMaxWhisperSize = int64(1200) * 1024 * 1024;
constexpr auto kMaxTesseractSize = int64(200) * 1024 * 1024;
constexpr auto kMaxTranslateSize = int64(200) * 1024 * 1024;
constexpr auto kSha256Length = 64;

std::vector<Model> ModelsList;
bool ModelsLoadedFromCache = false;
rpl::event_stream<> ModelsListChanged;
rpl::event_stream<bool> ManifestFinishedStream;
std::vector<Fn<void()>> ManifestWaiters;

struct Stamp {
	int64 size = 0;
	int64 modified = 0;
	QString sha256;

	friend inline bool operator==(const Stamp&, const Stamp&) = default;
};

std::mutex IntactMutex;
std::map<QString, Stamp> IntactFiles;

struct Download {
	base::unique_qptr<ModelLoader> loader;
	std::shared_ptr<bool> active;
};

std::map<int, Download> Downloads;
std::map<int, Model> WantedModels;
rpl::event_stream<int> DownloadFinishedStream;

[[nodiscard]] QString ManifestCachePath() {
	return ModelsPath() + u"/manifest.json"_q;
}

[[nodiscard]] bool ParseLocation(
		const QString &text,
		QString &channel,
		int &postId) {
	const auto sep = text.indexOf('#');
	if (sep <= 0 || sep == text.size() - 1) {
		return false;
	}
	auto ok = false;
	const auto parsed = text.mid(sep + 1).toInt(&ok);
	if (!ok || parsed <= 0) {
		return false;
	}
	channel = text.left(sep);
	postId = parsed;
	return true;
}

[[nodiscard]] bool IsSha256(const QString &value) {
	if (value.size() != kSha256Length) {
		return false;
	}
	for (const auto ch : value) {
		if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool IsTranslateCode(const QString &code) {
	static const auto pattern = QRegularExpression(
		u"^[a-z]{2,3}-[a-z]{2,3}$"_q);
	return pattern.match(code).hasMatch();
}

[[nodiscard]] bool IsSafeFileName(const QString &name) {
	return !name.isEmpty()
		&& !name.contains('/')
		&& !name.contains('\\')
		&& !name.startsWith('.');
}

[[nodiscard]] std::vector<Model> ParseManifest(const QByteArray &bytes) {
	auto result = std::vector<Model>();
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(bytes, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		LOG(("Recognition Error: manifest parse failed: %1"
			).arg(error.errorString()));
		return result;
	}
	const auto list = document.object().value(u"models"_q).toArray();
	result.reserve(list.size());
	for (const auto &value : list) {
		const auto object = value.toObject();
		auto model = Model();
		model.id = object.value(u"id"_q).toInt();
		model.size = int64(object.value(u"size"_q).toDouble());
		model.name = object.value(u"name"_q).toString();
		model.file = object.value(u"file"_q).toString();
		model.code = object.value(u"code"_q).toString();
		model.sha256 = object.value(u"sha256"_q).toString().toLower();
		const auto kind = object.value(u"kind"_q).toString();
		if (kind == u"whisper"_q) {
			model.kind = ModelKind::Whisper;
		} else if (kind == u"tesseract"_q) {
			model.kind = ModelKind::Tesseract;
		} else if (kind == u"tesseract-aux"_q) {
			model.kind = ModelKind::TesseractAux;
		} else if (kind == u"translate"_q) {
			model.kind = ModelKind::Translate;
		} else {
			continue;
		}
		const auto maxSize = (model.kind == ModelKind::Whisper)
			? kMaxWhisperSize
			: (model.kind == ModelKind::Translate)
			? kMaxTranslateSize
			: kMaxTesseractSize;
		if (!model.id
			|| model.size <= 0
			|| model.size > maxSize
			|| !IsSha256(model.sha256)
			|| model.name.isEmpty()
			|| !IsSafeFileName(model.file)
			|| (model.kind != ModelKind::Whisper
				&& (!IsSafeFileName(model.code)
					|| model.code.contains('+')))
			|| (model.kind == ModelKind::Translate
				&& !IsTranslateCode(model.code))
			|| !ParseLocation(
				object.value(u"location"_q).toString(),
				model.channel,
				model.postId)
			|| model.channel.compare(
				kModelsChannel.utf16(),
				Qt::CaseInsensitive) != 0) {
			continue;
		}
		result.push_back(std::move(model));
	}
	return result;
}

void ApplyManifest(std::vector<Model> parsed) {
	if (parsed.empty()) {
		return;
	}
	ModelsList = std::move(parsed);
	ModelsListChanged.fire({});
}

void LoadCachedManifest() {
	if (ModelsLoadedFromCache) {
		return;
	}
	if (!FreshVault::HasKey()) {
		return;
	}
	ModelsLoadedFromCache = true;
	// A manifest that is not sealed with the local key could be replaced.
	if (const auto bytes = FreshVault::ReadFile(ManifestCachePath(), true)) {
		auto parsed = ParseManifest(*bytes);
		if (!parsed.empty()) {
			ModelsList = std::move(parsed);
		}
	}
}

[[nodiscard]] QString DownloadFolder(int id) {
	// The loader creates only the last level of its output folder.
	const auto parent = ModelsPath() + u"/.download"_q;
	QDir().mkpath(parent);
	return parent + '/' + QString::number(id);
}

[[nodiscard]] QString ShaOfFile(const QString &path) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return QString();
	}
	auto hash = QCryptographicHash(QCryptographicHash::Sha256);
	if (!hash.addData(&file)) {
		return QString();
	}
	return QString::fromLatin1(hash.result().toHex());
}

void ApplyDownloaded(const Model &model) {
	auto &settings = AyuSettings::getInstance();
	switch (model.kind) {
	case ModelKind::Whisper:
		settings.setWhisperModel(model.file);
		break;
	case ModelKind::TesseractAux: {
		const auto current = settings.ocrLanguages();
		if (!IsAutoOcr(current)) {
			auto parts = OcrCodes(current);
			parts.push_front(kAutoOcrLanguages.utf16());
			settings.setOcrLanguages(parts.join('+'));
		}
	} break;
	case ModelKind::Translate:
		break;
	case ModelKind::Tesseract: {
		const auto current = settings.ocrLanguages();
		if (current == kAutoOcrLanguages.utf16()) {
			break; // All the downloaded languages are used anyway.
		}
		auto parts = current.split('+', Qt::SkipEmptyParts);
		if (!parts.contains(model.code)) {
			parts.push_back(model.code);
			settings.setOcrLanguages(parts.join('+'));
		}
	} break;
	}
}

void FinishDownload(const Model &model, const std::shared_ptr<bool> &active) {
	if (!*active) {
		return;
	}
	const auto id = model.id;
	Downloads.erase(id);
	const auto wanted = WantedModels.find(id);
	if (wanted != end(WantedModels)) {
		if (ModelExists(model)) {
			ApplyDownloaded(model);
		}
		WantedModels.erase(wanted);
	}
	DownloadFinishedStream.fire_copy(id);
}

class ManifestLoader final : public base::has_weak_ptr {
public:
	explicit ManifestLoader(base::weak_ptr<Main::Session> session);

	void start();

private:
	void resolved(const MTPInputChannel &channel);
	void download(int postId);
	void finish();

	MTP::WeakInstance _mtp;
	std::unique_ptr<MTP::DedicatedLoader> _loader;

};

std::shared_ptr<ManifestLoader> ActiveManifestLoader;

ManifestLoader::ManifestLoader(base::weak_ptr<Main::Session> session)
: _mtp(session) {
}

void ManifestLoader::start() {
	if (!_mtp.valid()) {
		finish();
		return;
	}
	const auto weak = base::make_weak(this);
	MTP::ResolveChannel(&_mtp, kModelsChannel.utf16(), [=](
			const MTPInputChannel &channel) {
		if (const auto strong = weak.get()) {
			strong->resolved(channel);
		}
	}, [=] {
		if (const auto strong = weak.get()) {
			strong->finish();
		}
	});
}

void ManifestLoader::resolved(const MTPInputChannel &channel) {
	const auto weak = base::make_weak(this);
	_mtp.send(
		MTPchannels_GetFullChannel(channel),
		[=](const MTPmessages_ChatFull &result) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			auto postId = kFallbackManifestPostId;
			result.match([&](const MTPDmessages_chatFull &data) {
				data.vfull_chat().match([&](const MTPDchannelFull &full) {
					if (const auto pinned = full.vpinned_msg_id()) {
						if (pinned->v > 0) {
							postId = pinned->v;
						}
					}
				}, [](const auto &) {});
			});
			strong->download(postId);
		},
		[=](const MTP::Error &) {
			if (const auto strong = weak.get()) {
				strong->download(kFallbackManifestPostId);
			}
		});
}

void ManifestLoader::download(int postId) {
	const auto folder = ModelsPath() + u"/.manifest"_q;
	QDir(folder).removeRecursively();
	QDir().mkpath(folder);
	const auto weak = base::make_weak(this);
	MTP::StartDedicatedLoader(
		&_mtp,
		MTP::DedicatedLoader::Location{
			.username = kModelsChannel.utf16(),
			.postId = postId,
		},
		folder,
		[=](std::unique_ptr<MTP::DedicatedLoader> loader) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			} else if (!loader) {
				strong->finish();
				return;
			}
			strong->_loader = std::move(loader);
			strong->_loader->ready(
			) | rpl::on_next([=](const QString &path) {
				auto file = QFile(path);
				if (file.open(QIODevice::ReadOnly)) {
					const auto bytes = file.readAll();
					file.close();
					auto parsed = ParseManifest(bytes);
					if (!parsed.empty()) {
						FreshVault::WriteSealedFile(ManifestCachePath(), bytes);
						ApplyManifest(std::move(parsed));
					}
				}
				QDir(QFileInfo(path).absolutePath()).removeRecursively();
				if (const auto strong = weak.get()) {
					strong->finish();
				}
			}, strong->_loader->lifetime());
			strong->_loader->failed(
			) | rpl::on_next([=] {
				if (const auto strong = weak.get()) {
					strong->finish();
				}
			}, strong->_loader->lifetime());
			strong->_loader->start();
		});
}

void ManifestLoader::finish() {
	crl::on_main([] {
		ActiveManifestLoader.reset();
		LoadCachedManifest();
		ManifestFinishedStream.fire(!ModelsList.empty());
		for (const auto &callback : base::take(ManifestWaiters)) {
			callback();
		}
	});
}

} // namespace

QString ModelsPath() {
	return cWorkingDir() + u"tdata/recognition"_q;
}

QString TessdataPath() {
	return ModelsPath() + u"/tessdata"_q;
}

QString TranslatePath() {
	return ModelsPath() + u"/translate"_q;
}

QString ModelPath(const Model &model) {
	switch (model.kind) {
	case ModelKind::Whisper:
		return ModelsPath() + u"/whisper/"_q + model.file;
	case ModelKind::Translate:
		return TranslatePath() + '/' + model.code + u".fgtm"_q;
	case ModelKind::Tesseract:
	case ModelKind::TesseractAux:
		break;
	}
	return TessdataPath() + '/' + model.code + u".traineddata"_q;
}

bool ModelExists(const Model &model) {
	const auto info = QFileInfo(ModelPath(model));
	return info.exists() && info.size() == model.size;
}

bool RemoveModel(const Model &model) {
	return QFile::remove(ModelPath(model));
}

std::vector<Model> Models(ModelKind kind) {
	LoadCachedManifest();
	auto result = std::vector<Model>();
	for (const auto &model : ModelsList) {
		if (model.kind == kind) {
			result.push_back(model);
		}
	}
	return result;
}

rpl::producer<> ModelsChanged() {
	return ModelsListChanged.events();
}

void RefreshManifest(not_null<Main::Session*> session) {
	if (ActiveManifestLoader) {
		return;
	}
	ActiveManifestLoader = std::make_shared<ManifestLoader>(
		base::make_weak(session));
	ActiveManifestLoader->start();
}

bool ModelsKnown() {
	LoadCachedManifest();
	return !ModelsList.empty();
}

void LoadModelsList(not_null<Main::Session*> session, Fn<void()> done) {
	if (ModelsKnown()) {
		done();
		return;
	}
	ManifestWaiters.push_back(std::move(done));
	RefreshManifest(session);
}

QString RecognitionErrorText(RecognitionError error) {
	switch (error) {
	case RecognitionError::None:
	case RecognitionError::Failed:
		break;
	case RecognitionError::Unavailable:
		return tr::fresh_RecognitionErrorUnavailable(tr::now);
	case RecognitionError::NoLanguages:
		return tr::fresh_RecognitionErrorNoLanguages(tr::now);
	case RecognitionError::UnsupportedCpu:
		return tr::fresh_RecognitionErrorCpu(tr::now);
	case RecognitionError::Integrity:
		return tr::fresh_RecognitionErrorIntegrity(tr::now);
	case RecognitionError::LoadFailed:
		return tr::fresh_RecognitionErrorLoad(tr::now);
	case RecognitionError::Decode:
		return tr::fresh_RecognitionErrorDecode(tr::now);
	case RecognitionError::TooLarge:
		return tr::fresh_TranscribeTooBig(tr::now);
	case RecognitionError::NoData:
		return tr::fresh_RecognitionErrorNoData(tr::now);
	}
	return tr::fresh_RecognitionErrorFailed(tr::now);
}

MTP::DedicatedLoader::Location ModelLocation(const Model &model) {
	return MTP::DedicatedLoader::Location{
		.username = model.channel,
		.postId = model.postId,
	};
}

std::optional<Model> SelectedWhisperModel() {
	const auto &file = AyuSettings::getInstance().whisperModel();
	if (file.isEmpty()) {
		return std::nullopt;
	}
	for (const auto &model : Models(ModelKind::Whisper)) {
		if (model.file == file) {
			return ModelExists(model)
				? std::make_optional(model)
				: std::nullopt;
		}
	}
	return std::nullopt;
}

bool ModelFileIntact(const QString &path, const QString &sha256) {
	const auto info = QFileInfo(path);
	if (!info.exists() || !IsSha256(sha256)) {
		return false;
	}
	const auto stamp = Stamp{
		.size = info.size(),
		.modified = info.lastModified().toMSecsSinceEpoch(),
		.sha256 = sha256,
	};
	{
		const auto lock = std::lock_guard(IntactMutex);
		const auto i = IntactFiles.find(path);
		if (i != end(IntactFiles) && i->second == stamp) {
			return true;
		}
	}
	if (ShaOfFile(path) != sha256) {
		LOG(("Recognition Error: '%1' failed the integrity check."
			).arg(info.fileName()));
		return false;
	}
	const auto lock = std::lock_guard(IntactMutex);
	IntactFiles[path] = stamp;
	return true;
}

QHash<QString, QString> TesseractHashes() {
	auto result = QHash<QString, QString>();
	for (const auto kind : { ModelKind::Tesseract, ModelKind::TesseractAux }) {
		for (const auto &model : Models(kind)) {
			if (ModelExists(model)) {
				result.insert(
					QFileInfo(ModelPath(model)).fileName(),
					model.sha256);
			}
		}
	}
	return result;
}

QStringList InstalledOcrLanguages() {
	auto result = QStringList();
	for (const auto &model : Models(ModelKind::Tesseract)) {
		if (ModelExists(model)) {
			result.push_back(model.code);
		}
	}
	return result;
}

std::vector<Model> InstalledTranslateModels() {
	auto result = std::vector<Model>();
	for (const auto &model : Models(ModelKind::Translate)) {
		if (ModelExists(model)) {
			result.push_back(model);
		}
	}
	return result;
}

bool IsAutoOcr(const QString &value) {
	return value.split('+', Qt::SkipEmptyParts).contains(
		kAutoOcrLanguages.utf16());
}

QStringList OcrCodes(const QString &value) {
	auto result = value.split('+', Qt::SkipEmptyParts);
	result.removeAll(kAutoOcrLanguages.utf16());
	return result;
}

QString DefaultOcrLanguages(const QStringList &installed) {
	constexpr auto kMaxLanguages = 3;
	const auto english = u"eng"_q;
	const auto hasEnglish = installed.contains(english);
	auto result = QStringList();
	for (const auto &code : installed) {
		if (code != english
			&& result.size() < kMaxLanguages - (hasEnglish ? 1 : 0)) {
			result.push_back(code);
		}
	}
	if (hasEnglish) {
		result.push_back(english);
	}
	return result.join('+');
}

QString EnabledOcrLanguages() {
	const auto installed = InstalledOcrLanguages();
	if (installed.isEmpty()) {
		return QString();
	}
	const auto automatic = [&] {
		for (const auto &model : Models(ModelKind::TesseractAux)) {
			if (ModelExists(model)) {
				return true;
			}
		}
		return false;
	}();
	const auto &value = AyuSettings::getInstance().ocrLanguages();
	auto chosen = QStringList();
	for (const auto &code : OcrCodes(value)) {
		if (installed.contains(code) && !chosen.contains(code)) {
			chosen.push_back(code);
		}
	}
	if (automatic && IsAutoOcr(value)) {
		chosen.push_front(kAutoOcrLanguages.utf16());
		return chosen.join('+');
	} else if (!chosen.isEmpty()) {
		return chosen.join('+');
	}
	// WHY: the setting can miss the downloaded data, for example when
	// the models box was closed before the download finished.
	return automatic
		? kAutoOcrLanguages.utf16()
		: DefaultOcrLanguages(installed);
}

ModelLoader::ModelLoader(
	QObject *parent,
	not_null<Main::Session*> session,
	const Model &model,
	Fn<void()> destroyCallback)
: BlobLoader(
	parent,
	session,
	model.id,
	ModelLocation(model),
	DownloadFolder(model.id),
	model.size)
, _model(model)
, _destroyCallback(std::move(destroyCallback)) {
}

void ModelLoader::unpack(const QString &path) {
	const auto weak = base::make_weak(this);
	const auto model = _model;
	const auto folder = _folder;
	crl::async([=] {
		auto success = false;
		if (QFileInfo(path).size() == model.size
			&& ShaOfFile(path) == model.sha256) {
			const auto target = ModelPath(model);
			QDir().mkpath(QFileInfo(target).absolutePath());
			QFile::remove(target);
			success = QFile::rename(path, target)
				|| (QFile::copy(path, target) && QFile::remove(path));
		} else {
			LOG(("Recognition Error: '%1' failed the integrity check."
				).arg(model.file));
		}
		QDir(folder).removeRecursively();
		crl::on_main(weak, [=] {
			if (success) {
				destroy();
			} else {
				fail();
			}
		});
	});
}

void ModelLoader::destroy() {
	Expects(_destroyCallback);

	crl::on_main(_destroyCallback);
}

void ModelLoader::fail() {
	BlobLoader::fail();
	destroy();
}

ModelLoader *ModelDownload(int id) {
	const auto i = Downloads.find(id);
	return (i != end(Downloads)) ? i->second.loader.get() : nullptr;
}

void StartModelDownload(
		not_null<Main::Session*> session,
		const Model &model) {
	if (Downloads.contains(model.id)) {
		return;
	}
	const auto active = std::make_shared<bool>(true);
	Downloads.emplace(model.id, Download{
		.loader = base::make_unique_q<ModelLoader>(
			QCoreApplication::instance(),
			session,
			model,
			[=] { FinishDownload(model, active); }),
		.active = active,
	});
}

void CancelModelDownload(int id) {
	WantedModels.erase(id);
	const auto i = Downloads.find(id);
	if (i != end(Downloads)) {
		*i->second.active = false;
		Downloads.erase(i);
	}
}

rpl::producer<int> ModelDownloadFinished() {
	return DownloadFinishedStream.events();
}

void WantModel(const Model &model) {
	if (ModelExists(model)) {
		ApplyDownloaded(model);
	} else if (Downloads.contains(model.id)) {
		WantedModels.insert_or_assign(model.id, model);
	}
}

rpl::producer<bool> ManifestFinished() {
	return ManifestFinishedStream.events();
}

} // namespace FreshRecognition

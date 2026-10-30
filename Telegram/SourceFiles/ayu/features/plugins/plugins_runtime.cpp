// This is the source code of freshGram.
#include "ayu/features/plugins/plugins_runtime.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/plugins/plugins_service.h"
#include "base/timer.h"
#include "core/application.h"
#include "lang_auto.h"
#include "crl/crl_async.h"
#include "crl/crl_on_main.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "mtproto/dedicated_file_loader.h"
#include "settings.h"
#include "window/window_controller.h"

#ifdef FRESHGRAM_PLUGINS
#include "fgpl/base64.h"
#include "fgpl/runtime_bundle.h"
#include "fgpl/signature.h"
#endif

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QStorageInfo>

#ifndef FRESHGRAM_RUNTIME_KEY
#define FRESHGRAM_RUNTIME_KEY ""
#endif

#ifndef FRESHGRAM_PLUGINS_KEY
#define FRESHGRAM_PLUGINS_KEY ""
#endif

#ifndef FRESHGRAM_RUNTIME_CHANNEL
#define FRESHGRAM_RUNTIME_CHANNEL "freshgram_runtime"
#endif

#ifndef FRESHGRAM_RUNTIME_DIR
#define FRESHGRAM_RUNTIME_DIR ""
#endif

namespace FreshPlugins {

#ifdef FRESHGRAM_PLUGINS

namespace {

#if defined Q_OS_WIN
#define FRESH_RUNTIME_OS "windows"
#elif defined Q_OS_MAC
#define FRESH_RUNTIME_OS "macos"
#else
#define FRESH_RUNTIME_OS "linux"
#endif
#if defined Q_PROCESSOR_ARM_64
constexpr auto kPlatform = FRESH_RUNTIME_OS "-aarch64";
#else
constexpr auto kPlatform = FRESH_RUNTIME_OS "-x86_64";
#endif
constexpr auto kMaxIndexBytes = qint64(256) << 10;
constexpr auto kFirstUpdateCheck = crl::time(60) * 1000;
constexpr auto kUpdateCheckEach = crl::time(24) * 60 * 60 * 1000;
constexpr auto kUpdateRetry = crl::time(60) * 60 * 1000;
constexpr auto kSpaceFactor = 4;

struct State {
	RuntimeStatus status;
	std::optional<fgpl::RuntimeIndex> index;
	rpl::variable<RuntimeStatus> value;
	std::unique_ptr<MTP::WeakInstance> mtp;
	std::unique_ptr<MTP::DedicatedLoader> loader;
	base::Timer updates;
	QString localIndex;
	uint64 generation = 0;
};

[[nodiscard]] State &GetState() {
	static auto result = State();
	return result;
}

[[nodiscard]] std::filesystem::path Root() {
	return PathOf(RuntimeRootPath());
}

[[nodiscard]] QString ChannelName() {
	return QStringLiteral(FRESHGRAM_RUNTIME_CHANNEL);
}

[[nodiscard]] QString PackagedDirectory() {
	return QStringLiteral(FRESHGRAM_RUNTIME_DIR);
}

[[nodiscard]] QString LocalIndexPath(const State &state) {
	if (!state.localIndex.isEmpty()) {
		return state.localIndex;
	}
	const auto packaged = PackagedDirectory();
	return packaged.isEmpty() ? QString() : (packaged + u"/runtime.json"_q);
}

[[nodiscard]] QString BundleFileName() {
	return u"runtime-"_q + QString::fromLatin1(kPlatform) + u".zip"_q;
}

void ReadLocal(
		const QString &path,
		qint64 maxBytes,
		Fn<void(QByteArray, QString)> done) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		done({}, u"cannot open "_q + path);
	} else if (file.size() > maxBytes) {
		done({}, u"the file is too large"_q);
	} else {
		done(file.readAll(), QString());
	}
}

[[nodiscard]] QString DownloadPath() {
	return RuntimeRootPath() + u"/download"_q;
}

[[nodiscard]] std::optional<fgpl::PublicKey> ParseKey(const char *base64) {
	const auto decoded = fgpl::Base64Decode(base64);
	if (!decoded || decoded->size() != 32) {
		return std::nullopt;
	}
	auto key = fgpl::PublicKey();
	std::copy(decoded->begin(), decoded->end(), key.begin());
	return key;
}

[[nodiscard]] std::optional<fgpl::PublicKey> TrustedKey() {
	return ParseKey(FRESHGRAM_RUNTIME_KEY);
}

[[nodiscard]] QString SizeText(qint64 bytes) {
	return QString::number(std::max<qint64>(1, bytes / (1024 * 1024))) + u" MB"_q;
}

void Publish(State &state) {
	const auto installed = TrustedRuntime();
	state.status.installed = installed.has_value();
	state.status.wasmtime = installed
		? QString::fromStdString(installed->wasmtime)
		: QString();
	state.status.python = installed
		? QString::fromStdString(installed->python)
		: QString();
	state.status.revision = installed ? installed->revision : 0;
	state.status.remoteKnown = state.index.has_value();
	if (state.index) {
		state.status.remoteWasmtime = QString::fromStdString(state.index->wasmtime);
		state.status.remotePython = QString::fromStdString(state.index->python);
		state.status.remoteRevision = state.index->revision;
		const auto bundle = fgpl::BundleFor(*state.index, kPlatform);
		state.status.remoteSize = bundle ? qint64(bundle->size) : 0;
		state.status.updateAvailable = bundle
			&& fgpl::Acceptable(*state.index)
			&& (!installed || installed->revision < state.index->revision);
	} else {
		state.status.updateAvailable = false;
	}
	state.value = state.status;
}

void Fail(State &state, const QString &error) {
	state.status.stage = RuntimeStage::Idle;
	state.status.error = error;
	state.status.progress = 0.;
	Publish(state);
}

[[nodiscard]] MTP::WeakInstance *Mtp(State &state) {
	const auto session = Core::App().activeAccount().maybeSession();
	if (!session) {
		state.mtp = nullptr;
		return nullptr;
	} else if (!state.mtp || state.mtp->session().get() != session) {
		state.mtp = std::make_unique<MTP::WeakInstance>(
			base::make_weak(session));
	}
	return state.mtp.get();
}

void Load(
		State &state,
		int post,
		const QString &folder,
		qint64 maxBytes,
		Fn<void(float64)> progress,
		Fn<void(QByteArray, QString)> done) {
	const auto mtp = Mtp(state);
	if (!mtp) {
		done({}, u"sign in to Telegram to download"_q);
		return;
	} else if (!QDir().mkpath(folder)) {
		done({}, u"cannot create the download folder"_q);
		return;
	}
	auto location = MTP::DedicatedLoader::Location();
	location.username = ChannelName();
	location.postId = post;
	const auto generation = state.generation;
	const auto finished = std::make_shared<bool>(false);
	const auto finish = [=](QByteArray bytes, QString error, bool wipe) {
		if (*finished) {
			return;
		}
		*finished = true;
		// WHY: the loader calls us from its own rpl streams, so it is
		// destroyed and the result delivered only after they return.
		crl::on_main([=] {
			auto &state = GetState();
			if (generation != state.generation) {
				return;
			}
			state.loader = nullptr;
			if (wipe) {
				QDir(folder).removeRecursively();
			}
			done(bytes, error);
		});
	};
	MTP::StartDedicatedLoader(mtp, location, folder, [=](
			std::unique_ptr<MTP::DedicatedLoader> loader) {
		auto &state = GetState();
		if (generation != state.generation) {
			return;
		} else if (!loader) {
			done({}, u"the file is not in the channel"_q);
			return;
		}
		const auto raw = loader.get();
		raw->progress(
		) | rpl::on_next([=](MTP::AbstractDedicatedLoader::Progress value) {
			if (value.size > maxBytes) {
				finish({}, u"the file is too large"_q, true);
			} else if (progress && value.size > 0) {
				progress(float64(value.already) / float64(value.size));
			}
		}, raw->lifetime());
		raw->ready(
		) | rpl::on_next([=](const QString &path) {
			auto file = QFile(path);
			const auto bytes = file.open(QIODevice::ReadOnly)
				? file.read(maxBytes + 1)
				: QByteArray();
			file.close();
			finish(
				bytes,
				bytes.isEmpty()
					? u"cannot read the downloaded file"_q
					: (bytes.size() > maxBytes)
					? u"the file is too large"_q
					: QString(),
				true);
		}, raw->lifetime());
		raw->failed(
		) | rpl::on_next([=] {
			finish({}, u"the download failed"_q, false);
		}, raw->lifetime());
		state.loader = std::move(loader);
		raw->start();
	});
}

void ResolveIndexPost(State &state, Fn<void(int, QString)> done) {
	const auto mtp = Mtp(state);
	if (!mtp) {
		done(0, u"sign in to Telegram to download"_q);
		return;
	}
	const auto generation = state.generation;
	const auto failed = [=](const QString &error) {
		if (generation == GetState().generation) {
			done(0, error);
		}
	};
	const auto username = ChannelName();
	MTP::ResolveChannel(mtp, username, [=](const MTPInputChannel &channel) {
		if (generation != GetState().generation) {
			return;
		}
		mtp->send(MTPchannels_GetFullChannel(channel), [=](
				const MTPmessages_ChatFull &result) {
			const auto pinned = result.data().vfull_chat().match([](
					const MTPDchannelFull &data) {
				return data.vpinned_msg_id().value_or_empty();
			}, [](const auto &) {
				return 0;
			});
			if (pinned) {
				if (generation == GetState().generation) {
					done(pinned, QString());
				}
			} else {
				failed(u"the channel has no pinned runtime index"_q);
			}
		}, [=](const MTP::Error &error) {
			failed(error.type());
		});
	}, [=] {
		failed(u"cannot find the channel @"_q + username);
	});
}

} // namespace

bool RuntimeCanBeVerified() {
	return TrustedKey().has_value();
}

QString RuntimeChannel() {
	return ChannelName();
}

bool IsOfficialPublisher(const std::string &publisher) {
	static const auto official = [] {
		const auto key = ParseKey(FRESHGRAM_PLUGINS_KEY);
		return key ? fgpl::Fingerprint(*key) : std::string();
	}();
	return !official.empty() && (publisher == official);
}

std::optional<fgpl::InstalledRuntime> TrustedRuntime() {
	const auto key = TrustedKey();
	return key
		? fgpl::ReadTrustedRuntime(Root(), *key, kPlatform)
		: std::nullopt;
}

RuntimeStatus CurrentRuntimeStatus() {
	auto &state = GetState();
	Publish(state);
	return state.status;
}

rpl::producer<RuntimeStatus> RuntimeStatusValue() {
	auto &state = GetState();
	Publish(state);
	return state.value.value();
}

void CheckRuntime(Fn<void(bool ok)> done) {
	auto &state = GetState();
	const auto key = TrustedKey();
	if (!key) {
		Fail(state, u"this build has no key to verify downloads"_q);
		if (done) {
			done(false);
		}
		return;
	} else if (state.status.stage != RuntimeStage::Idle) {
		if (done) {
			done(false);
		}
		return;
	}
	state.status.stage = RuntimeStage::Checking;
	state.status.error = QString();
	Publish(state);
	const auto parse = [=](QByteArray bytes, QString error) {
		auto &state = GetState();
		if (!error.isEmpty()) {
			Fail(state, error);
		} else {
			const auto parsed = fgpl::ParseRuntimeIndex(
				std::string_view(bytes.constData(), size_t(bytes.size())),
				*key);
			if (!parsed.ok()) {
				Fail(state, QString::fromStdString(parsed.error()));
			} else {
				fgpl::SaveRuntimePolicy(Root(), parsed.value());
				state.index = parsed.value();
				state.status.stage = RuntimeStage::Idle;
				Publish(state);
				NotifyRuntimeChanged();
			}
		}
		if (done) {
			done(state.status.error.isEmpty());
		}
	};
	if (const auto local = LocalIndexPath(state); !local.isEmpty()) {
		ReadLocal(local, kMaxIndexBytes, parse);
		return;
	}
	ResolveIndexPost(state, [=](int post, QString error) {
		if (!error.isEmpty()) {
			parse({}, error);
			return;
		}
		const auto folder = DownloadPath() + u"/index"_q;
		QDir(folder).removeRecursively();
		Load(GetState(), post, folder, kMaxIndexBytes, nullptr, parse);
	});
}

void DownloadRuntime(Fn<void(bool ok)> done) {
	auto &state = GetState();
	const auto bundle = state.index
		? fgpl::BundleFor(*state.index, kPlatform)
		: nullptr;
	if (!bundle || state.status.stage != RuntimeStage::Idle
		|| !fgpl::Acceptable(*state.index)) {
		if (done) {
			done(false);
		}
		return;
	}
	const auto needed = qint64(bundle->size) * kSpaceFactor;
	const auto storage = QStorageInfo(cWorkingDir());
	if (storage.isValid() && storage.bytesAvailable() < needed) {
		Fail(state, u"not enough free disk space: %1 needed"_q.arg(SizeText(needed)));
		if (done) {
			done(false);
		}
		return;
	}
	state.status.stage = RuntimeStage::Downloading;
	state.status.error = QString();
	state.status.progress = 0.;
	Publish(state);
	const auto index = *state.index;
	const auto copy = *bundle;
	const auto install = [=](QByteArray bytes, QString error) {
		auto &state = GetState();
		if (!error.isEmpty()) {
			Fail(state, error);
			if (done) {
				done(false);
			}
			return;
		}
		state.status.stage = RuntimeStage::Installing;
		Publish(state);
		const auto generation = state.generation;
		crl::async([=] {
			auto result = fgpl::InstallRuntime(
				{ reinterpret_cast<const uint8_t*>(bytes.constData()), size_t(bytes.size()) },
				index,
				copy,
				Root());
			const auto message = result.ok()
				? QString()
				: QString::fromStdString(result.error());
			crl::on_main([=] {
				auto &state = GetState();
				if (generation != state.generation) {
					return;
				}
				if (message.isEmpty()) {
					state.status.stage = RuntimeStage::Idle;
					Publish(state);
					NotifyRuntimeChanged();
				} else {
					Fail(state, message);
				}
				if (done) {
					done(message.isEmpty());
				}
			});
		});
	};
	if (const auto local = LocalIndexPath(state); !local.isEmpty()) {
		const auto directory = QFileInfo(local).absoluteDir();
		ReadLocal(directory.filePath(BundleFileName()), qint64(copy.size), install);
		return;
	}
	const auto name = u"rev-"_q + QString::number(index.revision);
	auto downloads = QDir(DownloadPath());
	for (const auto &other : downloads.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
		if (other != name) {
			QDir(downloads.filePath(other)).removeRecursively();
		}
	}
	Load(state, copy.post, downloads.filePath(name), qint64(copy.size),
		[](float64 progress) {
			auto &state = GetState();
			state.status.progress = progress;
			state.value = state.status;
		},
		install);
}

void InstallRuntimeFromFile(const QString &index, Fn<void(bool ok)> done) {
	auto &state = GetState();
	if (state.status.stage != RuntimeStage::Idle) {
		if (done) {
			done(false);
		}
		return;
	}
	state.localIndex = index;
	const auto finish = [=](bool ok) {
		GetState().localIndex = QString();
		if (done) {
			done(ok);
		}
	};
	CheckRuntime([=](bool ok) {
		if (ok) {
			DownloadRuntime(finish);
		} else {
			finish(false);
		}
	});
}

bool RuntimePackaged() {
	return !PackagedDirectory().isEmpty();
}

void UpdateAutomatically() {
	auto &state = GetState();
	const auto schedule = [](crl::time delay) {
		GetState().updates.callOnce(delay);
	};
	if (!AyuSettings::getInstance().pluginsEnabled() || !TrustedRuntime()) {
		schedule(kUpdateCheckEach);
		return;
	} else if (state.status.stage != RuntimeStage::Idle
		|| (!RuntimePackaged() && !Core::App().activeAccount().maybeSession())) {
		schedule(kUpdateRetry);
		return;
	}
	CheckRuntime([=](bool ok) {
		schedule(ok ? kUpdateCheckEach : kUpdateRetry);
		const auto status = CurrentRuntimeStatus();
		if (!ok
			|| !status.updateAvailable
			|| !AyuSettings::getInstance().pluginsRuntimeAutoUpdate()) {
			return;
		}
		DownloadRuntime([](bool ok) {
			const auto window = Core::App().activePrimaryWindow();
			if (!ok || !window) {
				return;
			}
			const auto status = CurrentRuntimeStatus();
			window->showToast(tr::fresh_PluginsUpdated(
				tr::now,
				lt_wasmtime,
				status.wasmtime,
				lt_python,
				status.python));
		});
	});
}

void StartRuntimeUpdates() {
	auto &state = GetState();
	state.updates.setCallback(UpdateAutomatically);
	state.updates.callOnce(kFirstUpdateCheck);
}

void CancelRuntime() {
	auto &state = GetState();
	++state.generation;
	state.loader = nullptr;
	state.localIndex = QString();
	state.status.stage = RuntimeStage::Idle;
	state.status.progress = 0.;
	Publish(state);
}

void RemoveRuntimeFiles() {
	CancelRuntime();
	fgpl::RemoveRuntime(Root());
	QDir(DownloadPath()).removeRecursively();
	auto &state = GetState();
	state.index = std::nullopt;
	Publish(state);
	NotifyRuntimeChanged();
}

#else

bool RuntimeCanBeVerified() {
	return false;
}

QString RuntimeChannel() {
	return QString();
}

bool IsOfficialPublisher(const std::string &) {
	return false;
}

RuntimeStatus CurrentRuntimeStatus() {
	return {};
}

rpl::producer<RuntimeStatus> RuntimeStatusValue() {
	return rpl::single(RuntimeStatus());
}

void CheckRuntime(Fn<void(bool ok)> done) {
	if (done) {
		done(false);
	}
}

void DownloadRuntime(Fn<void(bool ok)> done) {
	if (done) {
		done(false);
	}
}

void InstallRuntimeFromFile(const QString &index, Fn<void(bool ok)> done) {
	if (done) {
		done(false);
	}
}

bool RuntimePackaged() {
	return false;
}

void CancelRuntime() {
}

void StartRuntimeUpdates() {
}

void RemoveRuntimeFiles() {
}

#endif

} // namespace FreshPlugins

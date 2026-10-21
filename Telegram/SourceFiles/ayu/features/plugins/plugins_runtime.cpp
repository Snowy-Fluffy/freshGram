// This is the source code of freshGram.
#include "ayu/features/plugins/plugins_runtime.h"

#include "ayu/features/plugins/plugins_service.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/core_settings_proxy.h"
#include "crl/crl_async.h"
#include "crl/crl_on_main.h"
#include "mtproto/mtproto_proxy_data.h"

#ifdef FRESHGRAM_PLUGINS
#include "fgpl/base64.h"
#include "fgpl/runtime_bundle.h"
#endif

#include <QtCore/QDir>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#ifndef FRESHGRAM_RUNTIME_KEY
#define FRESHGRAM_RUNTIME_KEY ""
#endif

#ifndef FRESHGRAM_RUNTIME_INDEX_URL
#define FRESHGRAM_RUNTIME_INDEX_URL "https://github.com/Snowy-Fluffy/freshGram/releases/download/plugins-runtime/runtime.json"
#endif

namespace AyuPlugins {

#ifdef FRESHGRAM_PLUGINS

namespace {

constexpr auto kPlatform = "linux-x86_64";
constexpr auto kMaxIndexBytes = qint64(256) << 10;
constexpr auto kTimeoutMs = 30000;

struct State {
	RuntimeStatus status;
	std::optional<fgpl::RuntimeIndex> index;
	rpl::variable<RuntimeStatus> value;
	std::unique_ptr<QNetworkAccessManager> network;
	QPointer<QNetworkReply> reply;
	uint64 generation = 0;
};

[[nodiscard]] State &GetState() {
	static auto result = State();
	return result;
}

[[nodiscard]] std::filesystem::path Root() {
	return RuntimeRootPath().toStdString();
}

[[nodiscard]] std::optional<fgpl::PublicKey> TrustedKey() {
	const auto decoded = fgpl::Base64Decode(FRESHGRAM_RUNTIME_KEY);
	if (!decoded || decoded->size() != 32) {
		return std::nullopt;
	}
	auto key = fgpl::PublicKey();
	std::copy(decoded->begin(), decoded->end(), key.begin());
	return key;
}

[[nodiscard]] QString SizeText(qint64 bytes) {
	return QString::number(std::max<qint64>(1, bytes / (1024 * 1024))) + u" MB"_q;
}

void Publish(State &state) {
	const auto installed = fgpl::ReadInstalledRuntime(Root());
	state.status.installed = installed.has_value();
	state.status.wasmtime = installed
		? QString::fromStdString(installed->wasmtime)
		: QString();
	state.status.python = installed
		? QString::fromStdString(installed->python)
		: QString();
	state.status.remoteKnown = state.index.has_value();
	if (state.index) {
		state.status.remoteWasmtime = QString::fromStdString(state.index->wasmtime);
		state.status.remotePython = QString::fromStdString(state.index->python);
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

[[nodiscard]] QNetworkAccessManager &Network(State &state) {
	if (!state.network) {
		state.network = std::make_unique<QNetworkAccessManager>();
	}
	auto proxy = QNetworkProxy(QNetworkProxy::DefaultProxy);
	const auto &settings = Core::App().settings().proxy();
	if (!settings.isSystem() && settings.isEnabled()) {
		const auto selected = settings.selected();
		if (selected.type == MTP::ProxyData::Type::Socks5
			|| selected.type == MTP::ProxyData::Type::Http) {
			proxy = MTP::ToNetworkProxy(selected);
		} else {
			proxy = QNetworkProxy(QNetworkProxy::NoProxy);
		}
	} else if (!settings.isSystem()) {
		proxy = QNetworkProxy(QNetworkProxy::NoProxy);
	}
	state.network->setProxy(proxy);
	return *state.network;
}

void Fail(State &state, const QString &error) {
	state.status.stage = RuntimeStage::Idle;
	state.status.error = error;
	state.status.progress = 0.;
	Publish(state);
}

void Fetch(
		State &state,
		const QUrl &url,
		qint64 maxBytes,
		Fn<void(float64)> progress,
		Fn<void(QByteArray, QString)> done) {
	auto request = QNetworkRequest(url);
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setTransferTimeout(kTimeoutMs);
	request.setRawHeader("User-Agent", "freshGram-runtime");
	const auto reply = Network(state).get(request);
	state.reply = reply;
	const auto generation = state.generation;
	const auto buffer = std::make_shared<QByteArray>();
	const auto tooLarge = std::make_shared<bool>(false);
	QObject::connect(reply, &QNetworkReply::readyRead, reply, [=] {
		buffer->append(reply->readAll());
		if (buffer->size() > maxBytes) {
			*tooLarge = true;
			reply->abort();
		}
	});
	QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [=](qint64 received, qint64 total) {
		if (progress && total > 0) {
			progress(float64(received) / float64(total));
		}
	});
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		reply->deleteLater();
		auto &state = GetState();
		if (generation != state.generation) {
			return;
		}
		buffer->append(reply->readAll());
		if (*tooLarge || buffer->size() > maxBytes) {
			done({}, u"the file is too large"_q);
		} else if (reply->error() != QNetworkReply::NoError) {
			done({}, reply->errorString());
		} else {
			done(*buffer, QString());
		}
	});
}

} // namespace

bool RuntimeCanBeVerified() {
	return TrustedKey().has_value();
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
	Fetch(state, QUrl(QStringLiteral(FRESHGRAM_RUNTIME_INDEX_URL)), kMaxIndexBytes, nullptr, [=](QByteArray bytes, QString error) {
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
	state.status.stage = RuntimeStage::Downloading;
	state.status.error = QString();
	state.status.progress = 0.;
	Publish(state);
	const auto index = *state.index;
	const auto copy = *bundle;
	Fetch(state, QUrl(QString::fromStdString(copy.url)), qint64(copy.size) + 1024,
		[](float64 progress) {
			auto &state = GetState();
			state.status.progress = progress;
			state.value = state.status;
		},
		[=](QByteArray bytes, QString error) {
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
		});
}

void CancelRuntime() {
	auto &state = GetState();
	++state.generation;
	if (const auto reply = state.reply.data()) {
		reply->abort();
	}
	state.status.stage = RuntimeStage::Idle;
	state.status.progress = 0.;
	Publish(state);
}

void RemoveRuntimeFiles() {
	CancelRuntime();
	fgpl::RemoveRuntime(Root());
	auto &state = GetState();
	state.index = std::nullopt;
	Publish(state);
	NotifyRuntimeChanged();
}

#else

bool RuntimeCanBeVerified() {
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

void CancelRuntime() {
}

void RemoveRuntimeFiles() {
}

#endif

} // namespace AyuPlugins

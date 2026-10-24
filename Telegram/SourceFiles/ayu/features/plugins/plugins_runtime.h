// This is the source code of freshGram.
#pragma once

#include "base/basic_types.h"

namespace fgpl {
struct InstalledRuntime;
} // namespace fgpl

namespace AyuPlugins {

enum class RuntimeStage {
	Idle,
	Checking,
	Downloading,
	Installing,
};

struct RuntimeStatus {
	RuntimeStage stage = RuntimeStage::Idle;
	float64 progress = 0.;
	QString error;
	bool installed = false;
	QString wasmtime;
	QString python;
	bool remoteKnown = false;
	bool updateAvailable = false;
	QString remoteWasmtime;
	QString remotePython;
	qint64 remoteSize = 0;
};

[[nodiscard]] bool RuntimeCanBeVerified();
[[nodiscard]] QString RuntimeChannel();
[[nodiscard]] std::optional<fgpl::InstalledRuntime> TrustedRuntime();
[[nodiscard]] RuntimeStatus CurrentRuntimeStatus();
[[nodiscard]] rpl::producer<RuntimeStatus> RuntimeStatusValue();

void CheckRuntime(Fn<void(bool ok)> done = nullptr);
void DownloadRuntime(Fn<void(bool ok)> done = nullptr);
void InstallRuntimeFromFile(const QString &index, Fn<void(bool ok)> done);
[[nodiscard]] bool RuntimePackaged();
void CancelRuntime();
void StartRuntimeUpdates();
void RemoveRuntimeFiles();

} // namespace AyuPlugins

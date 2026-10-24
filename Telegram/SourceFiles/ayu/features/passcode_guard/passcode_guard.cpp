// This is the source code of freshGram.
#include "ayu/features/passcode_guard/passcode_guard.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/fake_passcode/fake_passcode.h"
#include "core/application.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "storage/details/storage_file_utilities.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>

namespace FreshPasscodeGuard {
namespace {

using namespace Storage::details;

constexpr auto kFreeTries = 3;

struct State {
	bool loaded = false;
	int tries = 0;
	qint64 lastTryWall = 0;
};

[[nodiscard]] State &GetState() {
	static auto result = State();
	return result;
}

[[nodiscard]] QString BasePath() {
	return cWorkingDir() + u"tdata/"_q;
}

[[nodiscard]] QString FileName() {
	return u"passcode_state"_q;
}

[[nodiscard]] crl::time RequiredDelay(int tries) {
	if (tries < kFreeTries) {
		return 0;
	}
	return std::min(tries - kFreeTries + 1, 6) * crl::time(5000);
}

void Write() {
	const auto &state = GetState();
	QDir().mkpath(BasePath());
	auto file = FileWriteDescriptor(FileName(), BasePath(), true);
	auto data = QByteArray();
	{
		auto stream = QDataStream(&data, QIODevice::WriteOnly);
		stream.setVersion(QDataStream::Qt_5_1);
		stream << qint32(state.tries) << qint64(state.lastTryWall);
	}
	file.writeData(data);
}

void Remove() {
	const auto base = BasePath() + FileName();
	QFile::remove(base + 's');
	QFile::remove(base + '0');
	QFile::remove(base + '1');
}

void SyncGlobals() {
	// WHY: the wait starts again after a restart, so changing the clock
	// of the system does not skip it.
	cSetPasscodeBadTries(GetState().tries);
	cSetPasscodeLastTry(crl::now());
}

void Wipe() {
	const auto action = WipeAction(
		AyuSettings::getInstance().passcodeWipeAction());
	LOG(("Passcode Guard: too many failed attempts, wiping."));
	auto actions = FreshFakePasscode::Actions();
	if (!Core::App().domain().started()
		|| action == WipeAction::DeleteAll) {
		actions.deleteAll = true;
	} else {
		actions.logoutDb = true;
		actions.logoutMedia = true;
		for (const auto &entry : Core::App().domain().accounts()) {
			actions.logoutAccounts.push_back(entry.index);
		}
	}
	const auto started = Core::App().domain().started();
	FreshFakePasscode::RunSilent(actions);
	if (!started) {
		// WHY: the session files are gone, a restart starts from scratch.
		crl::on_main(&Core::App(), [] { Core::Restart(); });
	}
}

} // namespace

void EnsureLoaded() {
	auto &state = GetState();
	if (state.loaded) {
		return;
	}
	state.loaded = true;
	auto file = FileReadDescriptor();
	if (!ReadFile(file, FileName(), BasePath())) {
		return;
	}
	auto data = QByteArray();
	file.stream >> data;
	if (!CheckStreamStatus(file.stream)) {
		return;
	}
	auto stream = QDataStream(data);
	stream.setVersion(QDataStream::Qt_5_1);
	auto tries = qint32();
	auto lastTryWall = qint64();
	stream >> tries >> lastTryWall;
	if (stream.status() != QDataStream::Ok || tries < 0) {
		return;
	}
	state.tries = tries;
	state.lastTryWall = lastTryWall;
	SyncGlobals();
}

bool RegisterFailure() {
	EnsureLoaded();
	auto &state = GetState();
	++state.tries;
	state.lastTryWall = QDateTime::currentMSecsSinceEpoch();
	SyncGlobals();
	const auto limit = AyuSettings::getInstance().passcodeWipeAttempts();
	if (limit > 0 && state.tries >= limit) {
		state.tries = 0;
		state.lastTryWall = 0;
		SyncGlobals();
		Remove();
		Wipe();
		return true;
	}
	Write();
	return false;
}

void RegisterSuccess() {
	EnsureLoaded();
	auto &state = GetState();
	const auto had = (state.tries > 0);
	state.tries = 0;
	state.lastTryWall = 0;
	cSetPasscodeBadTries(0);
	if (had) {
		Remove();
	}
}

crl::time WaitLeft() {
	EnsureLoaded();
	const auto delay = RequiredDelay(cPasscodeBadTries());
	const auto passed = crl::now() - cPasscodeLastTry();
	return std::max(delay - passed, crl::time(0));
}

int AttemptsBeforeWipe() {
	const auto limit = AyuSettings::getInstance().passcodeWipeAttempts();
	if (limit <= 0) {
		return -1;
	}
	EnsureLoaded();
	return std::max(limit - GetState().tries, 0);
}

} // namespace FreshPasscodeGuard

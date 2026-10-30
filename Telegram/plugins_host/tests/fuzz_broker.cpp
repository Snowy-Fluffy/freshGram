#include "fgpl/broker.h"
#include "fgpl/manifest.h"
#include "fakes.h"

#include <cstdlib>
#include <set>
#include <string>
#include <vector>

// Input: a grants byte, then up to kMaxCalls CBOR requests that run in order.

namespace {

using namespace fgpl;

constexpr auto kMaxCalls = 16;
constexpr auto kSecretChat = int64_t(3);
constexpr auto kSavedChat = fakes::FakeData::kSavedChat;

enum class Config : uint8_t {
	Everything,
	SelectedChat,
	Nothing,
};

// WHY: strings only the fakes produce, unless the plugin sent them in itself.
const auto kPrivate = std::vector<std::string>{
	"Alice", "Group", "Channel", "Bob", "Carol", "hello", "removed", "Work", "News",
	"About", "Text one", "Text two",
};
const auto kHidden = std::vector<std::string>{ "Secret", "Saved Messages" };

[[nodiscard]] Grants MakeGrants(Config config) {
	auto grants = Grants();
	grants.pluginId = "com.example.fuzz";
	grants.account = "acc";
	if (config != Config::Nothing) {
		grants.permissions = KnownPermissions();
	}
	if (config == Config::SelectedChat) {
		grants.chats = std::set<int64_t>{ 1 };
	}
	return grants;
}

[[nodiscard]] bool Contains(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

struct Counters {
	int ui = 0;
	int plugins = 0;
	int timers = 0;
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size < 2 || size > (1 << 16)) {
		return 0;
	}
	const auto config = Config(data[0] % 3);
	const auto input = std::string(reinterpret_cast<const char *>(data + 1), size - 1);

	auto storage = MemoryStorage();
	auto fake = fakes::FakeData();
	fake.withSaved = true;
	auto transport = fakes::FakeTransport();
	auto counters = Counters();
	auto time = int64_t(0);

	auto services = BrokerServices();
	services.storage = &storage;
	services.data = &fake;
	services.transport = &transport;
	services.clockMs = [&] { return time; };
	services.log = [](std::string_view) {};
	services.audit = [](std::string_view, std::string_view) {};
	services.ui = [&](std::string_view, const cbor::Value &) {
		++counters.ui;
		return true;
	};
	services.unload = [] {};
	services.sendToPlugin = [&](const std::string &, cbor::Value) {
		++counters.plugins;
		return std::string();
	};
	services.pluginPeers = [] { return std::vector<std::string>{ "com.example.peer" }; };
	services.setTimer = [&](const std::string &id, int64_t, bool) {
		++counters.timers;
		return id;
	};
	services.clearTimer = [](const std::string &) { return true; };
	services.timers = [] { return std::vector<std::string>(); };
	services.toastOffered = [](const std::string &) {};

	auto broker = Broker(MakeGrants(config), services);

	auto limits = cbor::Limits();
	limits.maxItems = 4096;
	limits.maxString = 1 << 14;
	limits.maxBytes = 1 << 16;
	auto rest = std::span<const uint8_t>(data + 1, size - 1);
	for (auto call = 0; call != kMaxCalls && !rest.empty(); ++call) {
		const auto decoded = cbor::DecodePrefix(rest, limits);
		if (!decoded.ok() || decoded.value().used == 0) {
			break;
		}
		rest = rest.subspan(decoded.value().used);
		time += 10;

		const auto response = broker.call(decoded.value().value);

		// The guest gets the response through the same encoder in HostCall.
		auto bytes = cbor::Bytes();
		if (!cbor::Encode(response, bytes, 32)) {
			std::abort();
		}
		const auto ok = response.find("ok");
		if (!ok || !ok->asBool()) {
			std::abort();
		}
		const auto text = std::string(bytes.begin(), bytes.end());
		for (const auto &marker : kHidden) {
			if (Contains(text, marker) && !Contains(input, marker)) {
				std::abort();
			}
		}
		if (config == Config::Nothing) {
			for (const auto &marker : kPrivate) {
				if (Contains(text, marker) && !Contains(input, marker)) {
					std::abort();
				}
			}
		}
	}
	broker.close();
	broker.releaseSockets();

	for (const auto &item : fake.sent) {
		if (item.first == kSecretChat
			|| item.first == kSavedChat
			|| item.first == api::kServiceChat
			|| (config == Config::SelectedChat && item.first != 1)) {
			std::abort();
		}
	}
	if (config == Config::Nothing) {
		// No permission means no effect on the app and no traffic.
		if (!fake.sent.empty()
			|| !fake.drafts.empty()
			|| !fake.toasts.empty()
			|| !fake.toastActions.empty()
			|| !fake.actions.empty()
			|| !fake.outgoing.empty()
			|| !fake.sentMedia.empty()
			|| !fake.editedMedia.empty()
			|| !fake.inlineQueries.empty()
			|| !fake.clipboard.empty()
			|| !transport.requests.empty()
			|| !transport.opened.empty()
			|| storage.count() != 0
			|| counters.ui != 0
			|| counters.plugins != 0) {
			std::abort();
		}
	}
	return 0;
}

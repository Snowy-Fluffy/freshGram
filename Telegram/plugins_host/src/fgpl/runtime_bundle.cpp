#include "fgpl/runtime_bundle.h"

#include "fgpl/base64.h"
#include "fgpl/json.h"
#include "fgpl/manifest.h"
#include "fgpl/package.h"
#include "fgpl/zip_reader.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>

#include <sys/stat.h>
#include <unistd.h>

namespace fgpl {
namespace {

namespace fs = std::filesystem;

constexpr auto kSpaceReserve = uint64_t(32) << 20;

constexpr auto kIndexKeys = std::to_array<std::string_view>({
	"schema", "revision", "minRevision", "host", "wasmtime", "python",
	"revoked", "bundles", "signature",
});

[[nodiscard]] std::optional<std::string> ReadText(const fs::path &path) {
	auto stream = std::ifstream(path, std::ios::binary);
	if (!stream) {
		return std::nullopt;
	}
	return std::string(std::istreambuf_iterator<char>(stream), {});
}

[[nodiscard]] bool WriteBytes(const fs::path &path, std::span<const uint8_t> data) {
	auto stream = std::ofstream(path, std::ios::binary | std::ios::trunc);
	return stream.write(
		reinterpret_cast<const char*>(data.data()),
		std::streamsize(data.size())).good();
}

[[nodiscard]] bool SafeVersionText(const std::string &text) {
	if (text.empty() || text.size() > 32) {
		return false;
	}
	return std::all_of(text.begin(), text.end(), [](char ch) {
		return (ch >= '0' && ch <= '9')
			|| (ch >= 'a' && ch <= 'z')
			|| ch == '.' || ch == '-' || ch == '+';
	});
}

[[nodiscard]] bool AllowedPath(const std::string &name) {
	const auto slash = name.find('/');
	const auto first = name.substr(0, slash);
	if (slash == std::string::npos) {
		return first == "fgpl_plugin_host";
	}
	return first == "python" || first == "sdk";
}

[[nodiscard]] std::string PolicyName() {
	return "policy.json";
}

[[nodiscard]] std::optional<std::pair<int, std::set<int>>> LoadPolicy(
		const fs::path &root) {
	const auto text = ReadText(root / PolicyName());
	if (!text) {
		return std::nullopt;
	}
	const auto parsed = json::Parse(*text);
	if (!parsed.ok() || !parsed.value().asMap()) {
		return std::nullopt;
	}
	const auto minimum = parsed.value().find("minRevision");
	const auto list = parsed.value().find("revoked");
	if (!minimum || !minimum->asInt() || !list || !list->asArray()) {
		return std::nullopt;
	}
	auto revoked = std::set<int>();
	for (const auto &item : *list->asArray()) {
		if (item.asInt()) {
			revoked.insert(int(*item.asInt()));
		}
	}
	return std::make_pair(int(*minimum->asInt()), std::move(revoked));
}

[[nodiscard]] std::string Hash(const fs::path &path) {
	const auto text = ReadText(path);
	if (!text) {
		return {};
	}
	return HashText({
		reinterpret_cast<const uint8_t*>(text->data()),
		text->size() });
}

} // namespace

Result<RuntimeIndex, std::string> ParseRuntimeIndex(
		std::string_view text,
		const PublicKey &key) {
	const auto parsed = json::Parse(text);
	if (!parsed.ok()) {
		return std::string("the runtime index is not valid JSON");
	}
	const auto &document = parsed.value();
	const auto map = document.asMap();
	if (!map) {
		return std::string("the runtime index must be an object");
	}
	for (const auto &entry : *map) {
		if (std::find(kIndexKeys.begin(), kIndexKeys.end(), entry.key) == kIndexKeys.end()) {
			return std::string("unknown field ") + entry.key;
		}
	}
	const auto signature = document.find("signature");
	const auto value = signature ? signature->find("value") : nullptr;
	const auto alg = signature ? signature->find("alg") : nullptr;
	if (!value || !value->asText() || !alg || !alg->asText() || *alg->asText() != "ed25519") {
		return std::string("the runtime index is not signed");
	}
	const auto decoded = Base64Decode(*value->asText());
	if (!decoded || decoded->size() != 64) {
		return std::string("the signature is malformed");
	}
	auto bytes = SignatureBytes();
	std::copy(decoded->begin(), decoded->end(), bytes.begin());
	const auto signedText = SignedText(document);
	if (!Ed25519Verify(key, {
			reinterpret_cast<const uint8_t*>(signedText.data()),
			signedText.size() }, bytes)) {
		return std::string("the signature does not match");
	}

	auto result = RuntimeIndex();
	const auto number = [&](const char *name) -> std::optional<int> {
		const auto field = document.find(name);
		const auto number = field ? field->asInt() : std::nullopt;
		if (!number || *number < 0 || *number > 1000000) {
			return std::nullopt;
		}
		return int(*number);
	};
	const auto schema = number("schema");
	const auto revision = number("revision");
	const auto minimum = number("minRevision");
	const auto host = number("host");
	if (!schema || *schema != 1 || !revision || !minimum || !host || *revision < 1) {
		return std::string("the runtime index has bad numbers");
	}
	result.revision = *revision;
	result.minRevision = *minimum;
	result.host = *host;
	for (const auto &[name, target] : { std::pair{ "wasmtime", &result.wasmtime }, std::pair{ "python", &result.python } }) {
		const auto field = document.find(name);
		if (!field || !field->asText() || !SafeVersionText(*field->asText())) {
			return std::string("bad version of ") + name;
		}
		*target = *field->asText();
	}
	if (const auto revoked = document.find("revoked")) {
		if (!revoked->asArray() || revoked->asArray()->size() > 1000) {
			return std::string("bad revoked list");
		}
		for (const auto &item : *revoked->asArray()) {
			if (!item.asInt()) {
				return std::string("bad revoked list");
			}
			result.revoked.push_back(int(*item.asInt()));
		}
	}
	const auto bundles = document.find("bundles");
	if (!bundles || !bundles->asMap() || bundles->asMap()->empty() || bundles->asMap()->size() > 16) {
		return std::string("no bundles");
	}
	for (const auto &entry : *bundles->asMap()) {
		const auto post = entry.value.find("post");
		const auto hash = entry.value.find("sha256");
		const auto size = entry.value.find("size");
		const auto hostHash = entry.value.find("hostSha256");
		const auto pythonHash = entry.value.find("pythonSha256");
		const auto hashText = [](const cbor::Value *value) {
			return value && value->asText()
				&& value->asText()->rfind("sha256-", 0) == 0;
		};
		if (!post || !post->asInt() || *post->asInt() < 1
			|| *post->asInt() > 0x7FFFFFFF
			|| !hash || !hash->asText() || hash->asText()->rfind("sha256-", 0) != 0
			|| !size || !size->asInt() || *size->asInt() < 1
			|| *size->asInt() > (int64_t(300) << 20)
			|| !hashText(hostHash) || !hashText(pythonHash)) {
			return std::string("bad bundle ") + entry.key;
		}
		result.bundles[entry.key] = {
			int(*post->asInt()),
			*hash->asText(),
			uint64_t(*size->asInt()),
			*hostHash->asText(),
			*pythonHash->asText(),
		};
	}
	result.text = std::string(text);
	return result;
}

const RuntimeBundle *BundleFor(
		const RuntimeIndex &index,
		const std::string &platform) {
	const auto i = index.bundles.find(platform);
	return i == index.bundles.end() ? nullptr : &i->second;
}

bool Acceptable(const RuntimeIndex &index) {
	return index.revision >= index.minRevision
		&& std::find(index.revoked.begin(), index.revoked.end(), index.revision) == index.revoked.end();
}

void SaveRuntimePolicy(const fs::path &root, const RuntimeIndex &index) {
	std::error_code error;
	fs::create_directories(root, error);
	const auto current = LoadPolicy(root);
	auto minimum = index.minRevision;
	auto revoked = std::set<int>(index.revoked.begin(), index.revoked.end());
	if (current) {
		minimum = std::max(minimum, current->first);
		revoked.insert(current->second.begin(), current->second.end());
	}
	auto list = cbor::Array();
	for (const auto number : revoked) {
		list.push_back(cbor::Value::FromInt(number));
	}
	const auto text = json::Canonical(cbor::Value::FromMap({
		{ "minRevision", cbor::Value::FromInt(minimum) },
		{ "revoked", cbor::Value::FromArray(std::move(list)) },
	}));
	const auto temporary = root / (PolicyName() + ".tmp");
	if (WriteBytes(temporary, { reinterpret_cast<const uint8_t*>(text.data()), text.size() })) {
		fs::rename(temporary, root / PolicyName(), error);
	}
}

Result<InstalledRuntime, std::string> InstallRuntime(
		std::span<const uint8_t> archive,
		const RuntimeIndex &index,
		const RuntimeBundle &bundle,
		const fs::path &root) {
	if (!Acceptable(index)) {
		return std::string("this runtime revision is not allowed");
	}
	if (const auto policy = LoadPolicy(root)) {
		if (index.revision < policy->first || policy->second.contains(index.revision)) {
			return std::string("this runtime revision is not allowed");
		}
	}
	if (archive.size() != bundle.size || HashText(archive) != bundle.hash) {
		return std::string("the downloaded file does not match its hash");
	}
	auto limits = ZipLimits();
	limits.maxEntries = 20000;
	limits.maxEntryBytes = uint64_t(64) << 20;
	limits.maxTotalBytes = uint64_t(400) << 20;
	limits.maxArchiveBytes = uint64_t(300) << 20;
	auto files = ReadZip(archive, limits);
	if (!files.ok()) {
		return std::string("the runtime archive is not valid: ") + files.error().detail;
	}
	auto host = std::string();
	auto python = std::string();
	auto hasLibrary = false;
	auto hasSdk = false;
	for (const auto &file : files.value()) {
		if (!AllowedPath(file.name)) {
			return std::string("unexpected file ") + file.name;
		}
		if (file.name == "fgpl_plugin_host") {
			host = HashText(file.data);
		} else if (file.name == "python/python.wasm") {
			python = HashText(file.data);
		} else if (file.name.rfind("python/lib/", 0) == 0) {
			hasLibrary = true;
		} else if (file.name.rfind("sdk/freshgram/", 0) == 0) {
			hasSdk = true;
		}
	}
	if (host.empty() || python.empty() || !hasLibrary || !hasSdk) {
		return std::string("the runtime archive is incomplete");
	} else if (host != bundle.hostHash || python != bundle.pythonHash) {
		return std::string("the runtime files do not match the signed index");
	} else if (index.text.empty()) {
		return std::string("the signed index is missing");
	}

	auto error = std::error_code();
	fs::create_directories(root, error);
	auto unpacked = uint64_t(0);
	for (const auto &file : files.value()) {
		unpacked += file.data.size();
	}
	const auto space = fs::space(root, error);
	if (error || space.available < unpacked + kSpaceReserve) {
		return std::string("not enough free disk space: ")
			+ std::to_string((unpacked + kSpaceReserve) >> 20) + " MB needed";
	}
	const auto name = "rev-" + std::to_string(index.revision);
	const auto staging = root / ("staging-" + std::to_string(::getpid()));
	fs::remove_all(staging, error);
	for (const auto &file : files.value()) {
		const auto path = staging / file.name;
		fs::create_directories(path.parent_path(), error);
		if (error || !WriteBytes(path, file.data)) {
			fs::remove_all(staging, error);
			return std::string("cannot write ") + file.name;
		}
	}
	::chmod((staging / "fgpl_plugin_host").c_str(), 0700);

	auto installed = InstalledRuntime();
	installed.revision = index.revision;
	installed.host = index.host;
	installed.wasmtime = index.wasmtime;
	installed.python = index.python;
	installed.hostHash = host;
	installed.pythonHash = python;
	const auto info = json::Canonical(cbor::Value::FromMap({
		{ "revision", cbor::Value::FromInt(index.revision) },
		{ "host", cbor::Value::FromInt(index.host) },
		{ "wasmtime", cbor::Value::FromText(index.wasmtime) },
		{ "python", cbor::Value::FromText(index.python) },
		{ "hostHash", cbor::Value::FromText(host) },
		{ "pythonHash", cbor::Value::FromText(python) },
	}));
	if (!WriteBytes(staging / "installed.json", { reinterpret_cast<const uint8_t*>(info.data()), info.size() })
		|| !WriteBytes(staging / "index.json", { reinterpret_cast<const uint8_t*>(index.text.data()), index.text.size() })) {
		fs::remove_all(staging, error);
		return std::string("cannot write the version file");
	}

	const auto target = root / name;
	fs::remove_all(target, error);
	fs::rename(staging, target, error);
	if (error) {
		fs::remove_all(staging, error);
		return std::string("cannot move the runtime into place");
	}
	const auto link = root / "current";
	const auto temporary = root / ("current.tmp-" + std::to_string(::getpid()));
	fs::remove(temporary, error);
	fs::create_directory_symlink(name, temporary, error);
	if (!error) {
		fs::rename(temporary, link, error);
	}
	if (error) {
		fs::remove(temporary, error);
		return std::string("cannot switch to the new runtime");
	}
	for (const auto &entry : fs::directory_iterator(root, error)) {
		const auto other = entry.path().filename().string();
		if (other.rfind("rev-", 0) == 0 && other != name) {
			auto number = 0;
			try {
				number = std::stoi(other.substr(4));
			} catch (...) {
				continue;
			}
			if (number < index.revision - 1) {
				fs::remove_all(entry.path(), error);
			}
		}
	}
	installed.directory = link;
	return installed;
}

std::optional<InstalledRuntime> ReadInstalledRuntime(const fs::path &root) {
	const auto directory = root / "current";
	const auto text = ReadText(directory / "installed.json");
	if (!text) {
		return std::nullopt;
	}
	const auto parsed = json::Parse(*text);
	if (!parsed.ok() || !parsed.value().asMap()) {
		return std::nullopt;
	}
	const auto &document = parsed.value();
	const auto text_of = [&](const char *name) -> std::string {
		const auto field = document.find(name);
		return field && field->asText() ? *field->asText() : std::string();
	};
	const auto number_of = [&](const char *name) -> int {
		const auto field = document.find(name);
		return field && field->asInt() ? int(*field->asInt()) : 0;
	};
	auto result = InstalledRuntime();
	result.revision = number_of("revision");
	result.host = number_of("host");
	result.wasmtime = text_of("wasmtime");
	result.python = text_of("python");
	result.hostHash = text_of("hostHash");
	result.pythonHash = text_of("pythonHash");
	result.directory = directory;
	if (result.revision < 1 || result.hostHash.empty() || result.pythonHash.empty()) {
		return std::nullopt;
	}
	if (const auto policy = LoadPolicy(root)) {
		if (result.revision < policy->first || policy->second.contains(result.revision)) {
			return std::nullopt;
		}
	}
	return result;
}

std::optional<InstalledRuntime> ReadTrustedRuntime(
		const fs::path &root,
		const PublicKey &key,
		const std::string &platform) {
	auto result = ReadInstalledRuntime(root);
	const auto text = result ? ReadText(result->directory / "index.json") : std::nullopt;
	if (!text) {
		return std::nullopt;
	}
	const auto index = ParseRuntimeIndex(*text, key);
	const auto bundle = index.ok() ? BundleFor(index.value(), platform) : nullptr;
	if (!bundle || !Acceptable(index.value())
		|| index.value().revision != result->revision
		|| index.value().host != result->host
		|| index.value().wasmtime != result->wasmtime
		|| index.value().python != result->python) {
		return std::nullopt;
	}
	result->hostHash = bundle->hostHash;
	result->pythonHash = bundle->pythonHash;
	return result;
}

bool VerifyInstalledRuntime(const InstalledRuntime &runtime) {
	return Hash(runtime.directory / "fgpl_plugin_host") == runtime.hostHash
		&& Hash(runtime.directory / "python" / "python.wasm") == runtime.pythonHash;
}

void RemoveRuntime(const fs::path &root) {
	auto error = std::error_code();
	for (const auto &entry : fs::directory_iterator(root, error)) {
		const auto name = entry.path().filename().string();
		if (name == "current" || name.rfind("rev-", 0) == 0
			|| name.rfind("staging-", 0) == 0) {
			fs::remove_all(entry.path(), error);
		}
	}
}

} // namespace fgpl

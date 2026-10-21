#pragma once

#include "fgpl/base64.h"
#include "fgpl/json.h"
#include "fgpl/package.h"
#include "fgpl/signature.h"
#include "wat_module.h"
#include "zip_builder.h"

#include <map>
#include <string>
#include <vector>

namespace pkg {

using namespace fgpl;

[[nodiscard]] inline cbor::Value Text(std::string_view text) {
	return cbor::Value::FromText(std::string(text));
}

[[nodiscard]] inline cbor::Value Int(int64_t number) {
	return cbor::Value::FromInt(number);
}

[[nodiscard]] inline cbor::Value Array(std::vector<std::string> items) {
	auto result = cbor::Array();
	for (auto &item : items) {
		result.push_back(Text(item));
	}
	return cbor::Value::FromArray(std::move(result));
}

inline void Set(cbor::Value &document, const std::string &path, cbor::Value value) {
	auto *current = &document;
	auto start = size_t(0);
	while (true) {
		const auto dot = path.find('.', start);
		const auto key = path.substr(
			start,
			dot == std::string::npos ? std::string::npos : dot - start);
		auto *map = current->asMap();
		if (!map) {
			return;
		}
		auto found = static_cast<cbor::MapEntry *>(nullptr);
		for (auto &entry : *map) {
			if (entry.key == key) {
				found = &entry;
			}
		}
		if (!found) {
			map->push_back({ key, cbor::Value::FromMap({}) });
			found = &map->back();
		}
		if (dot == std::string::npos) {
			found->value = std::move(value);
			return;
		}
		current = &found->value;
		start = dot + 1;
	}
}

inline void Remove(cbor::Value &document, const std::string &path) {
	auto *current = &document;
	auto start = size_t(0);
	while (true) {
		const auto dot = path.find('.', start);
		const auto key = path.substr(
			start,
			dot == std::string::npos ? std::string::npos : dot - start);
		auto *map = current->asMap();
		if (!map) {
			return;
		}
		if (dot == std::string::npos) {
			std::erase_if(*map, [&](const cbor::MapEntry &entry) {
				return entry.key == key;
			});
			return;
		}
		auto next = static_cast<cbor::Value *>(nullptr);
		for (auto &entry : *map) {
			if (entry.key == key) {
				next = &entry.value;
			}
		}
		if (!next) {
			return;
		}
		current = next;
		start = dot + 1;
	}
}

[[nodiscard]] inline cbor::Value ValidDocument() {
	auto document = cbor::Value::FromMap({});
	Set(document, "schema", Int(1));
	Set(document, "id", Text("com.example.demo"));
	Set(document, "name", Text("Demo"));
	Set(document, "version", Text("1.2.0"));
	Set(document, "description", Text("A demo plugin."));
	Set(document, "author.name", Text("Example Dev"));
	Set(document, "author.url", Text("https://example.com"));
	Set(document, "license", Text("MIT"));
	Set(document, "compat.freshgram", Text(">=7.6.0 <8.0.0"));
	Set(document, "compat.testedWith", Text("7.6.1"));
	Set(document, "compat.api", Int(1));
	Set(document, "compat.platforms", Array({ "linux", "windows", "macos" }));
	Set(document, "runtime.kind", Text("wasm"));
	Set(document, "runtime.module", Text("plugin.wasm"));
	Set(document, "runtime.language", Text("c"));
	Set(document, "runtime.abi", Int(1));
	Set(document, "permissions.required", cbor::Value::FromArray({
		cbor::Value::FromMap({
			{ "id", Text("messages.read") },
			{ "reason", Text("Reads messages.") },
		}),
	}));
	Set(document, "permissions.optional", cbor::Value::FromArray({
		cbor::Value::FromMap({
			{ "id", Text("net.hosts") },
			{ "hosts", Array({ "api.example.com", "*.cdn.example.com:8443" }) },
			{ "reason", Text("Talks to the service.") },
		}),
		cbor::Value::FromMap({
			{ "id", Text("storage.private") },
			{ "reason", Text("Remembers settings.") },
		}),
	}));
	Set(document, "contributes.events", Array({ "message.new" }));
	Set(document, "files", cbor::Value::FromMap({}));
	return document;
}

[[nodiscard]] inline std::vector<uint8_t> ReactorWasm() {
	return wat::Wasm(wat::Module("", "(i32.const 0)"));
}

[[nodiscard]] inline std::vector<uint8_t> CommandWasm() {
	return wat::Wasm(R"((module
		(memory (export "memory") 1)
		(func (export "_start"))))");
}

struct Built {
	std::vector<uint8_t> archive;
	cbor::Value document;
	std::map<std::string, std::vector<uint8_t>> files;
};

[[nodiscard]] inline std::vector<uint8_t> Bytes(std::string_view text) {
	return { text.begin(), text.end() };
}

[[nodiscard]] inline Built Build(
		cbor::Value document,
		std::map<std::string, std::vector<uint8_t>> files,
		const PrivateSeed *seed = nullptr,
		bool fillHashes = true) {
	if (fillHashes) {
		auto hashes = cbor::Map();
		for (const auto &file : files) {
			hashes.push_back({ file.first, Text(HashText(file.second)) });
		}
		Set(document, "files", cbor::Value::FromMap(std::move(hashes)));
	}
	if (seed) {
		Remove(document, "signature");
		const auto text = SignedText(document);
		const auto signature = Ed25519Sign(*seed, {
			reinterpret_cast<const uint8_t *>(text.data()), text.size() });
		const auto key = *Ed25519PublicKey(*seed);
		Set(document, "signature.alg", Text("ed25519"));
		Set(document, "signature.publicKey", Text(Base64Encode(key)));
		Set(document, "signature.value", Text(Base64Encode(*signature)));
	}
	auto items = std::vector<zipbuild::Item>();
	auto manifest = zipbuild::Item();
	manifest.name = "manifest.json";
	const auto text = json::Canonical(document);
	manifest.data = Bytes(text);
	items.push_back(std::move(manifest));
	for (const auto &file : files) {
		auto item = zipbuild::Item();
		item.name = file.first;
		item.data = file.second;
		items.push_back(std::move(item));
	}
	return { zipbuild::Build(items), std::move(document), std::move(files) };
}

[[nodiscard]] inline PrivateSeed Seed(uint8_t value) {
	auto seed = PrivateSeed();
	seed.fill(value);
	return seed;
}

} // namespace pkg

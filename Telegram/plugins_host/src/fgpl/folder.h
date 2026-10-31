#pragma once

#include "fgpl/result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fgpl::folder {

constexpr auto kMaxPathBytes = size_t(1024);
constexpr auto kMaxNameBytes = size_t(255);
constexpr auto kMaxDepth = size_t(32);
constexpr auto kMaxListPage = size_t(1000);
constexpr auto kMaxScanned = size_t(100000);

enum class Fail : uint8_t {
	Invalid,
	NotFound,
	Denied,
	Exists,
	NotEmpty,
	Limit,
	Failed,
};

enum class WriteMode : uint8_t {
	Create,
	Replace,
	Append,
};

struct Entry {
	std::string name;
	bool directory = false;
	uint64_t size = 0;
	int64_t modified = 0;
};

struct Listing {
	std::vector<Entry> entries;
	bool more = false;
};

// Splits a path relative to the folder; refuses absolute paths, '..', links to the outside and odd names.
[[nodiscard]] Result<std::vector<std::string>, Fail> SplitPath(
	std::string_view path,
	bool allowEmpty);

// Accepts an existing directory that is no root, home or system location; returns its canonical path.
[[nodiscard]] Result<std::string, Fail> CheckRoot(const std::string &path);

// Every call below works only beneath `root` and never follows a link.
[[nodiscard]] Result<Listing, Fail> List(
	const std::string &root,
	std::string_view path,
	size_t offset,
	size_t limit);
[[nodiscard]] Result<Entry, Fail> Stat(
	const std::string &root,
	std::string_view path);
[[nodiscard]] Result<std::vector<uint8_t>, Fail> Read(
	const std::string &root,
	std::string_view path,
	uint64_t offset,
	size_t length);
[[nodiscard]] Result<bool, Fail> Write(
	const std::string &root,
	std::string_view path,
	std::span<const uint8_t> bytes,
	WriteMode mode);
[[nodiscard]] Result<bool, Fail> MakeDirectory(
	const std::string &root,
	std::string_view path);
[[nodiscard]] Result<bool, Fail> Remove(
	const std::string &root,
	std::string_view path);
[[nodiscard]] Result<bool, Fail> Rename(
	const std::string &root,
	std::string_view from,
	std::string_view to);

} // namespace fgpl::folder

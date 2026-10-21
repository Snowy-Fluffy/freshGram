#pragma once

#include "fgpl/broker.h"

#include <filesystem>
#include <map>
#include <string>

namespace fgpl {

class FileStorage final : public Storage {
public:
	explicit FileStorage(std::filesystem::path file);
	~FileStorage() override;

	std::optional<cbor::Bytes> get(const std::string &key) override;
	void set(const std::string &key, cbor::Bytes value) override;
	bool remove(const std::string &key) override;
	std::vector<std::string> keys(
		const std::string &prefix,
		size_t limit) override;
	uint64_t usedBytes() override;
	size_t count() override;

	bool flush();

	[[nodiscard]] bool healthy() const {
		return _healthy;
	}

private:
	std::filesystem::path _file;
	std::map<std::string, cbor::Bytes> _items;
	bool _dirty = false;
	bool _healthy = true;

};

} // namespace fgpl

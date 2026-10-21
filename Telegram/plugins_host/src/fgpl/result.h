#pragma once

#include <utility>
#include <variant>

namespace fgpl {

template <typename T, typename E>
class Result {
public:
	Result(T value) : _data(std::in_place_index<0>, std::move(value)) {
	}
	Result(E error) : _data(std::in_place_index<1>, error) {
	}

	[[nodiscard]] bool ok() const {
		return _data.index() == 0;
	}
	[[nodiscard]] T &value() {
		return std::get<0>(_data);
	}
	[[nodiscard]] const T &value() const {
		return std::get<0>(_data);
	}
	[[nodiscard]] E error() const {
		return std::get<1>(_data);
	}

private:
	std::variant<T, E> _data;

};

} // namespace fgpl

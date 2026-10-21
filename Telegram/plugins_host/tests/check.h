#pragma once

#include <cstdio>

inline int &Failures() {
	static auto count = 0;
	return count;
}

#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			std::fprintf( \
				stderr, \
				"%s:%d: CHECK failed: %s\n", \
				__FILE__, \
				__LINE__, \
				#condition); \
			++Failures(); \
		} \
	} while (false)

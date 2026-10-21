#include "fgpl/ipc.h"

#include <fcntl.h>
#include <unistd.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size > 60000) {
		return 0;
	}
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) != 0) {
		return 0;
	}
	if (write(fds[1], data, size) != ssize_t(size)) {
		close(fds[0]);
		close(fds[1]);
		return 0;
	}
	close(fds[1]);
	const auto frame = fgpl::ipc::ReadFrame(
		fds[0],
		fgpl::ipc::StrictLimits(),
		1 << 20,
		50);
	close(fds[0]);
	return frame.ok() ? 0 : 0;
}

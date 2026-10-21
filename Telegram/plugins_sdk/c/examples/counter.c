#include "../freshgram.h"

int fg_plugin_init(fg_value info) {
	(void)info;
	fg_log("counter ready");
	return 0;
}

int fg_plugin_event(fg_value event) {
	fg_i64 chat = 0;
	fg_as_int(fg_find(event, "chat"), &chat);
	fg_i64 count = 0;
	int read = fg_storage_get_int("count", &count);
	if (read != FG_OK) {
		fg_log("cannot read the counter");
		return 100 + read;
	}
	count += 1;
	int written = fg_storage_set_int("count", count);
	if (written != FG_OK) {
		fg_log("cannot save the counter");
		return 200 + written;
	}
	fg_storage_set_int("last_chat", chat);
	return (int)count;
}

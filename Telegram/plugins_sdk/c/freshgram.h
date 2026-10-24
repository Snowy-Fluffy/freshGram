#ifndef FRESHGRAM_H
#define FRESHGRAM_H

#pragma GCC diagnostic ignored "-Wunused-function"

typedef unsigned char fg_u8;
typedef unsigned int fg_u32;
typedef long long fg_i64;
typedef unsigned long long fg_u64;

#define FG_EXPORT(name) __attribute__((export_name(name)))
#define FG_HOST_CALL \
	__attribute__((import_module("freshgram"), import_name("call")))

FG_HOST_CALL int fg_host_call(
	const void *request,
	fg_u32 length,
	void **response,
	fg_u32 *response_length);

enum {
	FG_OK = 0,
	FG_DENIED = 1,
	FG_INVALID = 2,
	FG_NOT_FOUND = 3,
	FG_QUOTA = 4,
	FG_LIMIT = 5,
	FG_UNAVAILABLE = 6,
	FG_FAILED = 7,
	FG_TRANSPORT = 8,
};

#ifndef FG_HEAP_BYTES
#define FG_HEAP_BYTES (1u << 20)
#endif

static fg_u8 fg_heap_[FG_HEAP_BYTES] __attribute__((aligned(16)));
static fg_u32 fg_heap_used_;

__attribute__((weak)) void *memcpy(void *to, const void *from, unsigned long size) {
	fg_u8 *a = to;
	const fg_u8 *b = from;
	while (size--) {
		*a++ = *b++;
	}
	return to;
}

__attribute__((weak)) void *memset(void *to, int value, unsigned long size) {
	fg_u8 *a = to;
	while (size--) {
		*a++ = (fg_u8)value;
	}
	return to;
}

static void *fg_arena(fg_u32 size) {
	fg_u32 at = (fg_heap_used_ + 7u) & ~7u;
	if (at > FG_HEAP_BYTES || size > FG_HEAP_BYTES - at) {
		return 0;
	}
	fg_heap_used_ = at + size;
	return fg_heap_ + at;
}

static fg_u32 fg_strlen(const char *text) {
	fg_u32 n = 0;
	while (text[n]) {
		++n;
	}
	return n;
}

typedef struct {
	const fg_u8 *p;
	const fg_u8 *end;
} fg_value;

static int fg_head(const fg_u8 **p, const fg_u8 *end, int *major, fg_u64 *argument) {
	if (*p >= end) {
		return -1;
	}
	fg_u8 initial = *(*p)++;
	int info = initial & 31;
	*major = initial >> 5;
	int width = 0;
	if (info < 24) {
		*argument = (fg_u64)info;
		return 0;
	} else if (info == 24) {
		width = 1;
	} else if (info == 25) {
		width = 2;
	} else if (info == 26) {
		width = 4;
	} else if (info == 27) {
		width = 8;
	} else {
		return -1;
	}
	if ((unsigned long)(end - *p) < (unsigned long)width) {
		return -1;
	}
	fg_u64 value = 0;
	for (int i = 0; i < width; ++i) {
		value = (value << 8) | *(*p)++;
	}
	*argument = value;
	return 0;
}

static int fg_skip(const fg_u8 **p, const fg_u8 *end, int depth) {
	int major = 0;
	fg_u64 argument = 0;
	if (depth > 24 || fg_head(p, end, &major, &argument) != 0) {
		return -1;
	}
	if (major == 2 || major == 3) {
		if (argument > (fg_u64)(end - *p)) {
			return -1;
		}
		*p += argument;
	} else if (major == 4 || major == 5) {
		fg_u64 count = (major == 5) ? argument * 2 : argument;
		for (fg_u64 i = 0; i < count; ++i) {
			if (fg_skip(p, end, depth + 1) != 0) {
				return -1;
			}
		}
	}
	return 0;
}

static fg_value fg_none(void) {
	fg_value none = { 0, 0 };
	return none;
}

static int fg_valid(fg_value v) {
	return v.p != 0 && v.p < v.end;
}

static fg_value fg_find(fg_value map, const char *key) {
	const fg_u8 *p = map.p;
	int major = 0;
	fg_u64 count = 0;
	if (!fg_valid(map) || fg_head(&p, map.end, &major, &count) != 0 || major != 5) {
		return fg_none();
	}
	fg_u32 wanted = fg_strlen(key);
	for (fg_u64 i = 0; i < count; ++i) {
		int keyMajor = 0;
		fg_u64 length = 0;
		if (fg_head(&p, map.end, &keyMajor, &length) != 0
			|| keyMajor != 3
			|| length > (fg_u64)(map.end - p)) {
			return fg_none();
		}
		int same = (length == wanted);
		for (fg_u32 k = 0; same && k < wanted; ++k) {
			same = (p[k] == (fg_u8)key[k]);
		}
		p += length;
		if (same) {
			fg_value found = { p, map.end };
			return found;
		}
		if (fg_skip(&p, map.end, 0) != 0) {
			return fg_none();
		}
	}
	return fg_none();
}

static int fg_as_int(fg_value v, fg_i64 *out) {
	const fg_u8 *p = v.p;
	int major = 0;
	fg_u64 argument = 0;
	if (!fg_valid(v) || fg_head(&p, v.end, &major, &argument) != 0) {
		return -1;
	}
	if (major == 0) {
		*out = (fg_i64)argument;
		return 0;
	} else if (major == 1) {
		*out = -1 - (fg_i64)argument;
		return 0;
	}
	return -1;
}

static int fg_as_bool(fg_value v, int *out) {
	if (!fg_valid(v) || (*v.p != 0xF4 && *v.p != 0xF5)) {
		return -1;
	}
	*out = (*v.p == 0xF5);
	return 0;
}

static int fg_as_text(fg_value v, const char **text, fg_u32 *length) {
	const fg_u8 *p = v.p;
	int major = 0;
	fg_u64 argument = 0;
	if (!fg_valid(v)
		|| fg_head(&p, v.end, &major, &argument) != 0
		|| major != 3
		|| argument > (fg_u64)(v.end - p)) {
		return -1;
	}
	*text = (const char *)p;
	*length = (fg_u32)argument;
	return 0;
}

typedef struct {
	fg_u8 *data;
	fg_u32 size;
	fg_u32 capacity;
	int failed;
} fg_writer;

static void fg_writer_init(fg_writer *w, fg_u32 capacity) {
	w->data = (fg_u8 *)fg_arena(capacity);
	w->size = 0;
	w->capacity = w->data ? capacity : 0;
	w->failed = (w->data == 0);
}

static void fg_put(fg_writer *w, fg_u8 byte) {
	if (w->size >= w->capacity) {
		w->failed = 1;
		return;
	}
	w->data[w->size++] = byte;
}

static void fg_put_head(fg_writer *w, int major, fg_u64 argument) {
	fg_u8 lead = (fg_u8)(major << 5);
	int width = 0;
	if (argument < 24) {
		fg_put(w, (fg_u8)(lead | argument));
		return;
	} else if (argument <= 0xFF) {
		fg_put(w, lead | 24);
		width = 1;
	} else if (argument <= 0xFFFF) {
		fg_put(w, lead | 25);
		width = 2;
	} else if (argument <= 0xFFFFFFFFull) {
		fg_put(w, lead | 26);
		width = 4;
	} else {
		fg_put(w, lead | 27);
		width = 8;
	}
	for (int i = width - 1; i >= 0; --i) {
		fg_put(w, (fg_u8)(argument >> (8 * i)));
	}
}

static void fg_w_map(fg_writer *w, fg_u32 count) {
	fg_put_head(w, 5, count);
}

static void fg_w_array(fg_writer *w, fg_u32 count) {
	fg_put_head(w, 4, count);
}

static void fg_w_text_n(fg_writer *w, const char *text, fg_u32 length) {
	fg_put_head(w, 3, length);
	for (fg_u32 i = 0; i < length; ++i) {
		fg_put(w, (fg_u8)text[i]);
	}
}

static void fg_w_bytes(fg_writer *w, const void *data, fg_u32 length) {
	const fg_u8 *bytes = (const fg_u8 *)data;
	fg_put_head(w, 2, length);
	for (fg_u32 i = 0; i < length; ++i) {
		fg_put(w, bytes[i]);
	}
}

static void fg_w_text(fg_writer *w, const char *text) {
	fg_w_text_n(w, text, fg_strlen(text));
}

static void fg_w_int(fg_writer *w, fg_i64 value) {
	if (value >= 0) {
		fg_put_head(w, 0, (fg_u64)value);
	} else {
		fg_put_head(w, 1, (fg_u64)(-1 - value));
	}
}

static void fg_w_bool(fg_writer *w, int value) {
	fg_put(w, value ? 0xF5 : 0xF4);
}

static void fg_w_null(fg_writer *w) {
	fg_put(w, 0xF6);
}

static void fg_begin(fg_writer *w, const char *op, fg_u32 arguments) {
	fg_writer_init(w, 4096);
	fg_w_map(w, arguments ? 2 : 1);
	fg_w_text(w, "op");
	fg_w_text(w, op);
	if (arguments) {
		fg_w_text(w, "a");
		fg_w_map(w, arguments);
	}
}

static int fg_error_code(fg_value response) {
	const char *text = 0;
	fg_u32 length = 0;
	if (fg_as_text(fg_find(response, "e"), &text, &length) != 0) {
		return FG_FAILED;
	}
	static const char *const names[] = {
		"denied", "invalid", "not_found", "quota", "limit", "unavailable",
	};
	for (int i = 0; i < 6; ++i) {
		fg_u32 n = fg_strlen(names[i]);
		int same = (n == length);
		for (fg_u32 k = 0; same && k < n; ++k) {
			same = (text[k] == names[i][k]);
		}
		if (same) {
			return i + 1;
		}
	}
	return FG_FAILED;
}

static int fg_send(fg_writer *w, fg_value *result) {
	if (w->failed) {
		return FG_TRANSPORT;
	}
	void *response = 0;
	fg_u32 length = 0;
	if (fg_host_call(w->data, w->size, &response, &length) != 0 || !response) {
		return FG_TRANSPORT;
	}
	fg_value value = { (const fg_u8 *)response, (const fg_u8 *)response + length };
	int ok = 0;
	if (fg_as_bool(fg_find(value, "ok"), &ok) != 0) {
		return FG_FAILED;
	} else if (!ok) {
		return fg_error_code(value);
	}
	if (result) {
		*result = fg_find(value, "v");
	}
	return FG_OK;
}

static int fg_log(const char *text) {
	fg_writer w;
	fg_begin(&w, "log", 1);
	fg_w_text(&w, "text");
	fg_w_text(&w, text);
	return fg_send(&w, 0);
}

static int fg_storage_get_int(const char *key, fg_i64 *out) {
	fg_writer w;
	fg_value result = fg_none();
	fg_begin(&w, "storage.get", 1);
	fg_w_text(&w, "key");
	fg_w_text(&w, key);
	int code = fg_send(&w, &result);
	if (code == FG_OK) {
		fg_as_int(result, out);
	}
	return code;
}

static int fg_storage_set_int(const char *key, fg_i64 value) {
	fg_writer w;
	fg_begin(&w, "storage.set", 2);
	fg_w_text(&w, "key");
	fg_w_text(&w, key);
	fg_w_text(&w, "value");
	fg_w_int(&w, value);
	return fg_send(&w, 0);
}

static int fg_unload(void) {
	fg_writer w;
	fg_begin(&w, "plugin.unload", 0);
	return fg_send(&w, 0);
}

static int fg_set_outgoing(const char *text) {
	fg_writer w;
	fg_begin(&w, "messages.set_outgoing", 1);
	fg_w_text(&w, "text");
	fg_w_text(&w, text);
	return fg_send(&w, 0);
}

static int fg_socket_connect(const char *op, const char *host, fg_i64 port, int tls, fg_i64 *id) {
	fg_writer w;
	fg_value result = fg_none();
	fg_begin(&w, op, tls ? 3 : 2);
	fg_w_text(&w, "host");
	fg_w_text(&w, host);
	fg_w_text(&w, "port");
	fg_w_int(&w, port);
	if (tls) {
		fg_w_text(&w, "tls");
		fg_w_bool(&w, 1);
	}
	int code = fg_send(&w, &result);
	if (code == FG_OK) {
		fg_as_int(fg_find(result, "id"), id);
	}
	return code;
}

static int fg_tcp_connect(const char *host, fg_i64 port, int tls, fg_i64 *id) {
	return fg_socket_connect("net.tcp.connect", host, port, tls, id);
}

static int fg_udp_connect(const char *host, fg_i64 port, fg_i64 *id) {
	return fg_socket_connect("net.udp.connect", host, port, 0, id);
}

static int fg_socket_send(fg_i64 id, const void *data, fg_u32 length) {
	fg_writer w;
	fg_begin(&w, "net.socket.send", 2);
	fg_w_text(&w, "id");
	fg_w_int(&w, id);
	fg_w_text(&w, "data");
	fg_w_bytes(&w, data, length);
	return fg_send(&w, 0);
}

static int fg_socket_close(fg_i64 id) {
	fg_writer w;
	fg_begin(&w, "net.socket.close", 1);
	fg_w_text(&w, "id");
	fg_w_int(&w, id);
	return fg_send(&w, 0);
}

int fg_plugin_init(fg_value info);
int fg_plugin_event(fg_value event);

FG_EXPORT("fg_alloc") fg_u32 fg_alloc(fg_u32 size) {
	return (fg_u32)(unsigned long)fg_arena(size);
}

FG_EXPORT("fg_free") void fg_free(fg_u32 pointer, fg_u32 size) {
	(void)pointer;
	(void)size;
	fg_heap_used_ = 0;
}

FG_EXPORT("fg_init") int fg_init(fg_u32 pointer, fg_u32 size) {
	fg_value info = { (const fg_u8 *)(unsigned long)pointer, (const fg_u8 *)(unsigned long)pointer + size };
	return fg_plugin_init(info);
}

FG_EXPORT("fg_on_event") int fg_on_event(fg_u32 pointer, fg_u32 size) {
	fg_value event = { (const fg_u8 *)(unsigned long)pointer, (const fg_u8 *)(unsigned long)pointer + size };
	return fg_plugin_event(event);
}

#endif

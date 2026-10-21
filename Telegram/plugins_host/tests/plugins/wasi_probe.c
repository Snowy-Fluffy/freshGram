typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long i64;

#define IMPORT(name) \
	__attribute__((import_module("wasi_snapshot_preview1"), import_name(name)))
#define EXPORT(name) __attribute__((export_name(name)))

typedef struct { const void *base; u32 length; } Iovec;

IMPORT("fd_write") int fd_write(int, const Iovec *, int, u32 *);
IMPORT("fd_read") int fd_read(int, const Iovec *, int, u32 *);
IMPORT("fd_pread") int fd_pread(int, const Iovec *, int, i64, u32 *);
IMPORT("fd_seek") int fd_seek(int, i64, int, u64 *);
IMPORT("fd_tell") int fd_tell(int, u64 *);
IMPORT("fd_close") int fd_close(int);
IMPORT("fd_prestat_get") int fd_prestat_get(int, u8 *);
IMPORT("fd_prestat_dir_name") int fd_prestat_dir_name(int, char *, u32);
IMPORT("fd_filestat_get") int fd_filestat_get(int, u8 *);
IMPORT("fd_fdstat_get") int fd_fdstat_get(int, u8 *);
IMPORT("fd_readdir") int fd_readdir(int, u8 *, u32, u64, u32 *);
IMPORT("path_open") int path_open(
	int, int, const char *, u32, int, u64, u64, int, int *);
IMPORT("path_filestat_get") int path_filestat_get(
	int, int, const char *, u32, u8 *);
IMPORT("path_unlink_file") int path_unlink_file(int, const char *, u32);

void *memcpy(void *to, const void *from, unsigned long size) {
	u8 *a = to;
	const u8 *b = from;
	while (size--) {
		*a++ = *b++;
	}
	return to;
}

void *memset(void *to, int value, unsigned long size) {
	u8 *a = to;
	while (size--) {
		*a++ = (u8)value;
	}
	return to;
}

static u8 heap[1 << 16];
static u32 heapUsed;

EXPORT("fg_alloc") u32 fg_alloc(u32 size) {
	u32 at = (heapUsed + 7) & ~7u;
	if (at + size > sizeof(heap)) {
		return 0;
	}
	heapUsed = at + size;
	return (u32)(unsigned long)(heap + at);
}

EXPORT("fg_free") void fg_free(u32 pointer, u32 size) {
	(void)pointer;
	(void)size;
	heapUsed = 0;
}

EXPORT("fg_init") int fg_init(u32 pointer, u32 size) {
	(void)pointer;
	(void)size;
	return 0;
}

static u32 length(const char *text) {
	u32 n = 0;
	while (text[n]) {
		++n;
	}
	return n;
}

static void write_text(const char *text, u32 size) {
	Iovec vector = { text, size };
	u32 written = 0;
	fd_write(1, &vector, 1, &written);
}

static void line_begin(const char *label) {
	write_text(label, length(label));
	write_text("=", 1);
}

static void line_end(void) {
	write_text("\n", 1);
}

static void emit_number(const char *label, i64 value) {
	char digits[24];
	int at = 24;
	int negative = value < 0;
	u64 magnitude = negative ? (u64)(-value) : (u64)value;
	do {
		digits[--at] = (char)('0' + magnitude % 10);
		magnitude /= 10;
	} while (magnitude);
	if (negative) {
		digits[--at] = '-';
	}
	line_begin(label);
	write_text(digits + at, (u32)(24 - at));
	line_end();
}

static void emit_text(const char *label, const char *text, u32 size) {
	while (size && (text[size - 1] == '\n' || text[size - 1] == 0)) {
		--size;
	}
	line_begin(label);
	write_text(text, size);
	line_end();
}

static int open_read(int base, const char *path, int *out) {
	return path_open(base, 0, path, length(path), 0, 2 | 4 | 32, 0, 0, out);
}

static int read_all(int fd, char *buffer, u32 capacity, u32 *got) {
	Iovec vector = { buffer, capacity };
	return fd_read(fd, &vector, 1, got);
}

EXPORT("fg_on_event") int fg_on_event(u32 pointer, u32 size) {
	(void)pointer;
	(void)size;
	int plugin = -1;
	int runtime = -1;
	for (int fd = 3;; ++fd) {
		u8 stat[8];
		if (fd_prestat_get(fd, stat) != 0) {
			break;
		}
		u32 name_length = stat[4] | (stat[5] << 8) | (stat[6] << 16);
		char name[64];
		if (name_length > sizeof(name)
			|| fd_prestat_dir_name(fd, name, name_length) != 0) {
			emit_number("prestat_name_error", fd);
			continue;
		}
		emit_text("preopen", name, name_length);
		if (name_length == 7 && name[1] == 'p') {
			plugin = fd;
		} else if (name_length == 8 && name[1] == 'r') {
			runtime = fd;
		}
	}
	emit_number("plugin_fd", plugin);
	emit_number("runtime_fd", runtime);

	int file = -1;
	emit_number("open_hello", open_read(plugin, "hello.txt", &file));
	char buffer[64];
	u32 got = 0;
	emit_number("read_error", read_all(file, buffer, sizeof(buffer), &got));
	emit_text("content", buffer, got);

	u8 stat[64];
	emit_number("filestat_error", fd_filestat_get(file, stat));
	emit_number("filestat_type", stat[16]);
	emit_number("filestat_size", stat[32] | (stat[33] << 8));

	u64 position = 0;
	emit_number("seek_error", fd_seek(file, 5, 0, &position));
	emit_number("seek_position", (i64)position);
	read_all(file, buffer, 4, &got);
	emit_text("after_seek", buffer, got);
	fd_tell(file, &position);
	emit_number("tell", (i64)position);

	Iovec vector = { buffer, 4 };
	emit_number("pread_error", fd_pread(file, &vector, 1, 1, &got));
	emit_text("pread", buffer, got);
	emit_number("seek_negative", fd_seek(file, -1000, 1, &position));
	emit_number("seek_stdout", fd_seek(1, 0, 0, &position));

	u8 fdstat[24];
	emit_number("fdstat_error", fd_fdstat_get(file, fdstat));
	emit_number("fdstat_type", fdstat[0]);
	emit_number("fdstat_dir_type", (fd_fdstat_get(plugin, fdstat), fdstat[0]));
	emit_number("fdstat_stdout_type", (fd_fdstat_get(1, fdstat), fdstat[0]));

	int missing = -1;
	emit_number("open_missing", open_read(plugin, "nope.txt", &missing));
	emit_number("open_dotdot", open_read(plugin, "../runtime/lib/os.py", &missing));
	emit_number("open_absolute", open_read(plugin, "/runtime/lib/os.py", &missing));
	emit_number("open_deep_dotdot", open_read(plugin, "sub/../../hello.txt", &missing));
	emit_number("open_inner_dotdot", open_read(plugin, "sub/../hello.txt", &missing));
	if (missing >= 0) {
		fd_close(missing);
	}
	emit_number("open_dir_as_file", open_read(plugin, "sub/inner.txt/", &missing));
	emit_number("open_other_mount", open_read(runtime, "lib/os.py", &missing));
	if (missing >= 0) {
		fd_close(missing);
	}

	emit_number(
		"open_write_rights",
		path_open(plugin, 0, "hello.txt", 9, 0, 2 | 64, 0, 0, &missing));
	emit_number(
		"open_create",
		path_open(plugin, 0, "new.txt", 7, 1, 2, 0, 0, &missing));
	emit_number(
		"open_truncate",
		path_open(plugin, 0, "hello.txt", 9, 8, 2, 0, 0, &missing));
	emit_number("unlink", path_unlink_file(plugin, "hello.txt", 9));
	emit_number(
		"stat_path_error",
		path_filestat_get(plugin, 0, "sub/inner.txt", 13, stat));
	emit_number("stat_path_size", stat[32] | (stat[33] << 8));
	emit_number(
		"stat_path_missing",
		path_filestat_get(plugin, 0, "sub/none", 8, stat));

	int dir = -1;
	emit_number("open_dir", path_open(plugin, 0, "data", 4, 2, 2 | 16384, 0, 0, &dir));
	u8 entries[256];
	u32 used = 0;
	emit_number("readdir_error", fd_readdir(dir, entries, sizeof(entries), 0, &used));
	u32 at = 0;
	while (at + 24 <= used) {
		u32 name_length = entries[at + 16] | (entries[at + 17] << 8);
		if (at + 24 + name_length > used) {
			break;
		}
		emit_text("entry", (const char *)entries + at + 24, name_length);
		at += 24 + name_length;
	}
	emit_number("readdir_used", used);
	emit_number("readdir_tiny", (fd_readdir(dir, entries, 30, 0, &used), used));
	emit_number("readdir_past_end", (fd_readdir(dir, entries, 256, 99, &used), used));
	emit_number("open_file_with_dir_flag", path_open(plugin, 0, "hello.txt", 9, 2, 2, 0, 0, &missing));
	emit_number("readdir_on_file", fd_readdir(file, entries, 64, 0, &used));
	emit_number("read_dir", read_all(dir, buffer, 4, &got));

	emit_number("close", fd_close(file));
	emit_number("read_closed", read_all(file, buffer, 4, &got));
	emit_number("close_closed", fd_close(file));
	fd_close(dir);

	int count = 0;
	int last = 0;
	int handles[80];
	for (; count < 80; ++count) {
		last = open_read(plugin, "hello.txt", &handles[count]);
		if (last != 0) {
			break;
		}
	}
	emit_number("opened_until_error", count);
	emit_number("limit_error", last);
	for (int i = 0; i < count; ++i) {
		fd_close(handles[i]);
	}
	emit_number("reopen", open_read(plugin, "hello.txt", &file));
	return 0;
}

#define _GNU_SOURCE

#define UNW_LOCAL_ONLY

#include <dlfcn.h>
#include <elfutils/libdwfl.h>
#include <stdint.h>
#include <libunwind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-util.h>

typedef void (*emit_func_t)(struct wl_signal *signal, void *data);

static emit_func_t real_emit = NULL;
static int stack_depth = -1;
static Dwfl *dwfl_handle = NULL;
static int dwfl_initialized = 0;
static char *dwfl_debuginfo_path = NULL;

static const Dwfl_Callbacks dwfl_callbacks = {
	.find_elf = dwfl_linux_proc_find_elf,
	.find_debuginfo = dwfl_standard_find_debuginfo,
	.debuginfo_path = &dwfl_debuginfo_path,
};

static void init_dwfl(void) {
	if (dwfl_initialized) {
		return;
	}

	dwfl_handle = dwfl_begin(&dwfl_callbacks);
	if (!dwfl_handle) {
		fprintf(stderr, "[hook] dwfl_begin failed\n");
		dwfl_initialized = 1;
		return;
	}

	if (dwfl_linux_proc_report(dwfl_handle, getpid()) != 0) {
		fprintf(stderr, "[hook] dwfl_linux_proc_report failed\n");
		dwfl_end(dwfl_handle);
		dwfl_handle = NULL;
		dwfl_initialized = 1;
		return;
	}

	if (dwfl_report_end(dwfl_handle, NULL, NULL) != 0) {
		fprintf(stderr, "[hook] dwfl_report_end failed\n");
		dwfl_end(dwfl_handle);
		dwfl_handle = NULL;
		dwfl_initialized = 1;
		return;
	}

	dwfl_initialized = 1;
}

static const char *resolve_function_name(void *addr, char *buf, size_t buf_size) {
	if (!addr) {
		return "<null>";
	}

	init_dwfl();

	if (dwfl_handle) {
		Dwfl_Module *module = dwfl_addrmodule(dwfl_handle, (Dwarf_Addr) (uintptr_t) addr);
		if (module) {
			const char *name =
				dwfl_module_addrname(module, (Dwarf_Addr) (uintptr_t) addr);
			if (name && *name) {
				snprintf(buf, buf_size, "%s", name);
				return buf;
			}
		}
	}

	Dl_info info;
	if (dladdr(addr, &info) && info.dli_sname) {
		snprintf(buf, buf_size, "%s", info.dli_sname);
		return buf;
	}

	return "<unknown>";
}

static int resolve_source_location(void *addr, char *file_buf, size_t file_buf_size,
				   int *line_out, int *col_out) {
	if (!addr || !file_buf || file_buf_size == 0) {
		return 0;
	}

	init_dwfl();
	if (!dwfl_handle) {
		return 0;
	}

	Dwfl_Line *line = dwfl_getsrc(dwfl_handle, (Dwarf_Addr) (uintptr_t) addr);
	if (!line) {
		return 0;
	}

	int line_no = 0;
	int col_no = 0;
	const char *file = dwfl_lineinfo(line, NULL, &line_no, &col_no, NULL, NULL);
	if (!file || !*file) {
		return 0;
	}

	snprintf(file_buf, file_buf_size, "%s", file);
	if (line_out) {
		*line_out = line_no;
	}
	if (col_out) {
		*col_out = col_no;
	}

	return 1;
}

static void print_emit_callsite(void) {
	unw_context_t context;
	unw_cursor_t cursor;
	unw_word_t ip = 0;
	char func_name_buf[512] = {0};
	char file_buf[1024] = {0};
	const char *func_name = "<unknown>";
	int line_no = 0;
	int col_no = 0;

	if (unw_getcontext(&context) != 0 ||
	    unw_init_local(&cursor, &context) != 0) {
		fprintf(stderr, "[hook] emit callsite: <unavailable>\n");
		return;
	}

	if (unw_step(&cursor) <= 0) {
		fprintf(stderr, "[hook] emit callsite: <unavailable>\n");
		return;
	}

	if (unw_step(&cursor) <= 0) {
		fprintf(stderr, "[hook] emit callsite: <unavailable>\n");
		return;
	}

	if (unw_get_reg(&cursor, UNW_REG_IP, &ip) != 0) {
		fprintf(stderr, "[hook] emit callsite: <unavailable>\n");
		return;
	}

	func_name = resolve_function_name((void *) (uintptr_t) ip, func_name_buf,
					 sizeof(func_name_buf));

	if (resolve_source_location((void *) (uintptr_t) ip, file_buf,
				    sizeof(file_buf), &line_no, &col_no)) {
		if (col_no > 0) {
			fprintf(stderr,
				"[hook] emit callsite: %s (%s:%d:%d)\n",
				func_name, file_buf, line_no, col_no);
		} else {
			fprintf(stderr,
				"[hook] emit callsite: %s (%s:%d)\n",
				func_name, file_buf, line_no);
		}
	} else {
		fprintf(stderr, "[hook] emit callsite: %s (0x%lx)\n", func_name,
			(unsigned long) ip);
	}
}

static void init_stack_depth(void) {
	if (stack_depth >= 0) {
		return;
	}

	const char *env = getenv("HOOK_STACK_DEPTH");
	if (!env || !*env) {
		stack_depth = 0;
		return;
	}

	char *end = NULL;
	long value = strtol(env, &end, 10);
	if (end == env || *end != '\0' || value < 0) {
		fprintf(stderr, "[hook] invalid HOOK_STACK_DEPTH='%s'\n", env);
		stack_depth = 0;
		return;
	}

	stack_depth = (int) value;
}

static void print_stack_trace(int max_frames) {
	unw_context_t context;
	unw_cursor_t cursor;
	int frame = 0;
	int skipped_first = 0;

	if (max_frames <= 0) {
		return;
	}

	if (unw_getcontext(&context) != 0 ||
	    unw_init_local(&cursor, &context) != 0) {
		fprintf(stderr, "[hook] failed to collect unwind context\n");
		return;
	}

	fprintf(stderr, "[hook] stack trace:\n");
	while (frame < max_frames && unw_step(&cursor) > 0) {
		unw_word_t ip = 0;
		unw_word_t offset = 0;
		char symbol[256] = {0};
		char resolved_name_buf[512] = {0};
		const char *resolved_name = NULL;
		int has_symbol = 0;

		if (unw_get_reg(&cursor, UNW_REG_IP, &ip) != 0) {
			break;
		}

		if (!skipped_first) {
			skipped_first = 1;
			continue;
		}

		resolved_name = resolve_function_name((void *) (uintptr_t) ip,
						     resolved_name_buf,
						     sizeof(resolved_name_buf));

		has_symbol = (unw_get_proc_name(&cursor, symbol, sizeof(symbol),
						&offset) == 0);
		if (resolved_name && strcmp(resolved_name, "<unknown>") != 0) {
			if (has_symbol && strcmp(resolved_name, symbol) != 0) {
				fprintf(stderr,
					"[hook]   #%d 0x%lx <%s> [unwind:%s+0x%lx]\n",
					frame, (unsigned long) ip, resolved_name, symbol,
					(unsigned long) offset);
			} else {
				fprintf(stderr, "[hook]   #%d 0x%lx <%s>\n", frame,
					(unsigned long) ip, resolved_name);
			}
		} else if (has_symbol) {
			fprintf(stderr, "[hook]   #%d 0x%lx <%s+0x%lx>\n",
				frame, (unsigned long) ip, symbol,
				(unsigned long) offset);
		} else {
			fprintf(stderr, "[hook]   #%d 0x%lx <unknown>\n", frame,
				(unsigned long) ip);
		}

		frame++;
	}
}

static void print_listener_callbacks(struct wl_signal *signal) {
	fprintf(stderr, "[hook] notify handlers:\n");

	if (wl_list_empty(&signal->listener_list)) {
		fprintf(stderr, "[hook]   <none>\n");
		return;
	}

	struct wl_listener *listener;
	int count = 0;

	wl_list_for_each(listener, &signal->listener_list, link) {
		count++;

		wl_notify_func_t notify_func = listener->notify;
		char func_name_buf[512] = {0};
		const char *func_name =
			resolve_function_name((void *) notify_func, func_name_buf,
					      sizeof(func_name_buf));

		fprintf(stderr, "[hook]   [%d] %p (%s)\n", count,
			(void *) notify_func,
			func_name);
	}
}

static void resolve_real_emit(void) {
	if (real_emit) {
		return;
	}

	real_emit = (emit_func_t) dlsym(RTLD_NEXT, "wl_signal_emit_mutable");
	if (!real_emit) {
		fprintf(stderr, "[hook] dlsym failed: %s\n", dlerror());
		abort();
	}
}

void wl_signal_emit_mutable(struct wl_signal *signal, void *data) {
	resolve_real_emit();
	init_stack_depth();

	print_emit_callsite();
	print_stack_trace(stack_depth);
	print_listener_callbacks(signal);

	real_emit(signal, data);
}

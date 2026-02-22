#define _GNU_SOURCE

#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <grp.h>
#include <libgen.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>
#include <wordexp.h>

#ifndef LAST_EDITOR
#define LAST_EDITOR "vi"
#endif

#ifndef SECURE_PATH
#define SECURE_PATH                                                            \
	"/usr/local/sbin:/usr/local/bin:"                                      \
	"/usr/sbin:/usr/bin:"                                                  \
	"/sbin:/bin"
#endif

#ifndef PROG_NAME
#define PROG_NAME "keyi"
#endif

#ifndef PROG_VERSION
#define PROG_VERSION "0.2.0"
#endif

#define PROG_USAGE                                                             \
	"usage: " PROG_NAME " [NAME=VALUE] COMMAND [ARG]...\n"                 \
	"   or: " PROG_NAME " [NAME=VALUE] -i\n"                               \
	"   or: " PROG_NAME " -e FILE...\n"

// with errno
void debug(const char *fmt, ...) {
#ifdef NDEBUG
	return;
#endif
	va_list args;
	va_start(args, fmt);
	vwarn(fmt, args);
	va_end(args);
}

void debugx(const char *fmt, ...) {
#ifdef NDEBUG
	return;
#endif
	va_list args;
	va_start(args, fmt);
	vwarnx(fmt, args);
	va_end(args);
}

enum keyi_mode {
	KEYI_CMD = 0,
	KEYI_SHELL, // 1.shell 2.cd home
	KEYI_EDIT,
};

struct keyi_file {
	const char *old_path;
	char *tmp_path;
	int old_fd;
	int tmp_fd;
	struct timespec time;
};

uid_t ruid, euid, suid;
gid_t rgid, egid, sgid;

struct passwd *rpw, *epw;

const char *env_editor(void) {
	const char *items[] = {
		"EDITOR",
		"VISUAL",
		"SUDO_EDITOR",
		// others
		NULL,
	};
	const char *name = LAST_EDITOR;
	for (int i = 0; items[i]; i++) {
		const char *value = getenv(items[i]);
		if (value && *value) {
			name = value;
			break;
		}
	}

	debugx("using editor: %s", name);
	return name;
}

void env_root(void) {
	const char *term = getenv("TERM");

	clearenv();

	setenv("USER", epw->pw_name, true);
	setenv("LOGNAME", epw->pw_name, true);
	setenv("HOME", epw->pw_dir, true);

	// it's shell's duty to set SHELL
	// setenv("SHELL", pw->pw_shell, true);

	setenv("PATH", SECURE_PATH, true);

	setenv("LANG", "C.UTF-8", true);
	if (term) {
		setenv("TERM", term, true);
	}
}

// $ keyi FOO=BAR printenv FOO
int env_opts(int argc, char *argv[], bool write) {
	int i; // optind?
	for (i = 1; i < argc; i++) {
		const char *equal = strchr(argv[i], '=');
		if (!equal) {
			debugx("not env %s", argv[i]);
			return i;
		}
		if (!write) {
			continue;
		}
		char *key = strdup(argv[i]);
		char *value = key + (equal - argv[i]);
		*value++ = '\0';
		setenv(key, value, true);
		debugx("set env %s=%s", key, value);
		free(key);
	}
	return i;
}

static void clean_one(struct keyi_file *f, bool keep) {
	if (!f) {
		return;
	}
	if (f->old_path) {
		debugx("cleanup %s", f->old_path);
	}

	// ignore errors
	if (close(f->old_fd) == 0) {
		debugx("close old fd %d", f->old_fd);
	}
	if (close(f->tmp_fd) == 0) {
		debugx("close tmp fd %d", f->tmp_fd);
	}

	if (!keep && unlink(f->tmp_path) == 0) {
		debugx("delete %s", f->tmp_path);
	}

	free(f->tmp_path);
	free(f);
}

static void clean_all(struct keyi_file **files) {
	if (!files) {
		return;
	}
	for (int i = 0; files[i]; i++) {
		clean_one(files[i], false);
	}
	free(files);
}

struct keyi_file *copy_one(const char *path, const char *prefix) {
	assert(path);
	assert(prefix);

	struct keyi_file *f = calloc(1, sizeof(*f));
	f->old_path = path;

	f->old_fd = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
	if (f->old_fd == -1) {
		warn("failed to open %s", path);
		goto ret;
	}

	struct stat old_stat = {0};
	if (fstat(f->old_fd, &old_stat)) {
		warn("failed to fstat %s", path);
		goto ret;
	}
	if (!S_ISREG(old_stat.st_mode)) {
		warnx("not a regular file %s", path);
		goto ret;
	}

	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;

	char buff[PATH_MAX] = {0};
	snprintf(buff, PATH_MAX, "%s/" PROG_NAME ".XXXXXX%s", prefix, base);

	f->tmp_fd = mkostemps(buff, strlen(base), O_CLOEXEC);
	if (f->tmp_fd == -1) {
		warn("failed to mkstemps %s", buff);
		goto ret;
	}
	f->tmp_path = strdup(buff);

	off_t count = old_stat.st_size;
	if (count > 0x7FFFF000) {
		warnx("file too large %s", f->old_path);
		goto ret;
	}
	off_t offset = 0;
	ssize_t sent = sendfile(f->tmp_fd, f->old_fd, &offset, count);
	if (sent != count) {
		warn("failed to sendfile from %s to %s", f->old_path,
		     f->tmp_path);
		goto ret;
	}

	if (fchown(f->tmp_fd, ruid, rgid)) {
		warn("failed to fchown %s", f->tmp_path);
		goto ret;
	}

	struct stat tmp_stat = {0};
	fstat(f->tmp_fd, &tmp_stat);
	f->time = tmp_stat.st_mtim;

	debugx("copy file from %s to %s", path, f->tmp_path);
	return f;

ret:
	// in sudo, a file will be created if it does not exist,
	// but yeki does not implement. there are various cases:
	//
	// 1. file does not exist
	// 2. directory does not exist
	// 3. permissions are insufficient
	//
	// so this feature was not introduced for simplicity.

	clean_one(f, false);
	return NULL;
}

struct keyi_file **copy_all(int argc, char *argv[]) {
	const char *prefix = "/tmp";

	struct keyi_file **files = calloc(argc - optind + 1, sizeof(*files));

	int count = 0;
	for (int i = optind; i < argc; i++) {
		const char *name = argv[i];
		struct keyi_file *file = copy_one(name, prefix);
		if (!file) {
			goto err;
		}
		files[count++] = file;
	}

	assert(files[count] == NULL);
	return files;

err:
	clean_all(files);
	errx(1, "one of the file(s)");
}

bool save_one(const struct keyi_file *f) {
	struct stat tmp_stat = {0};
	if (fstat(f->tmp_fd, &tmp_stat)) {
		warn("failed to fstat %s", f->tmp_path);
		return false;
	}

	size_t count = tmp_stat.st_size;
	if (count > 0x7FFFF000) {
		warnx("file too large %s", f->tmp_path);
		return false;
	}

	struct timespec tmp_time = f->time;
	struct timespec new_time = tmp_stat.st_mtim;

	debugx("tmp timespec sec %ld, nsec %ld", tmp_time.tv_sec,
	       tmp_time.tv_nsec);
	debugx("new timespec sec %ld, nsec %ld", new_time.tv_sec,
	       new_time.tv_nsec);

	if (tmp_time.tv_sec == new_time.tv_sec &&
	    tmp_time.tv_nsec == new_time.tv_nsec) {
		warnx("unchanged %s", f->old_path);
		return true;
	}

	// necessary!
	if (lseek(f->old_fd, 0, SEEK_SET) == -1) {
		warn("failed to lseek %s", f->old_path);
		return false;
	}
	if (ftruncate(f->old_fd, 0)) {
		warn("failed to fstat %s", f->old_path);
		return false;
	}

	// copy
	off_t offset = 0;
	ssize_t sent = sendfile(f->old_fd, f->tmp_fd, &offset, count);
	if (sent != count) {
		warn("failed to sendfile from %s to %s", f->tmp_path,
		     f->old_path);
	}

	debugx("copy %s back to %s", f->tmp_path, f->old_path);
	return true;
}

bool save_all(struct keyi_file **files) {
	bool ret = true;

	for (int i = 0; files[i]; i++) {
		struct keyi_file *f = files[i];
		bool is_ok = save_one(f);
		if (!is_ok) {
			warnx("backup retained at %s", f->tmp_path);
			ret = false;
		}
		clean_one(f, !is_ok);
	}
	sync();

	return ret;
}

char **edit_argv(const char *editor, struct keyi_file **files) {

	// EDITOR="vim -u NONE"
	wordexp_t p;
	wordexp(editor, &p, WRDE_NOCMD);

	int count = 0;
	while (files[count]) {
		count++;
	}

	char **argv = calloc(p.we_wordc + count + 1, sizeof(*argv));

	for (size_t i = 0; i < p.we_wordc; i++) {
		argv[i] = strdup(p.we_wordv[i]);
		debugx("argv editor %s", p.we_wordv[i]);
	}

	for (int i = 0; i < count; i++) {
		struct keyi_file *f = files[i];
		argv[p.we_wordc + i] = strdup(f->tmp_path);
		debugx("argv tmp file %s", f->tmp_path);
	}

	assert(argv[p.we_wordc + count] == NULL);

	wordfree(&p);
	return argv;
}

void free_argv(char **argv) {
	if (!argv) {
		return;
	}
	for (int i = 0; argv[i]; i++) {
		free(argv[i]);
	}
	free(argv);
}

void set_root(void) {
	debugx("set root e(%d)", euid);

	if (setgid(euid) == -1) {
		err(1, "failed to setgid %d", euid);
	}
	if (setuid(euid) == -1) {
		err(1, "failed to setuid %d", euid);
	}

	if (initgroups(epw->pw_name, euid) == -1) {
		err(1, "failed to initgroups %d", euid);
	}
}

void set_user(void) {
	debugx("set user r(%d) ", ruid);

	// 1 st
	if (setgid(rgid) == -1) {
		err(1, "failed to setgid");
	}
	// 2 nd
	if (setuid(ruid) == -1) {
		err(1, "failed to setuid");
	}
}

[[noreturn]] void run_cmd(int argc, char *argv[]) {
	env_root();
	env_opts(argc, argv, true);

	const char *exec_argv = argv[optind];

	set_root();

	syslog(LOG_INFO | LOG_AUTH, "%s ran command %s as %s from %s",
	       rpw->pw_name, exec_argv, epw->pw_name, getcwd(NULL, 0));
	execvp(exec_argv, &argv[optind]);
	err(1, "failed to execvp %s", exec_argv);
}

[[noreturn]] void run_shell(int argc, char *argv[]) {
	env_root();
	env_opts(argc, argv, true);

	const char *shell = getenv("SHELL");
	if (!shell) {
		shell = epw->pw_shell;
	}

	const char *home = getenv("HOME");
	if (!home) {
		home = epw->pw_dir;
	}
	chdir(home);

	char name[PATH_MAX] = {0};
	snprintf(name, PATH_MAX, "-%s", strrchr(shell, '/') + 1);

	set_root();

	syslog(LOG_INFO | LOG_AUTH, "%s ran a shell as %s from %s",
	       rpw->pw_name, epw->pw_name, getcwd(NULL, 0));
	execlp(shell, name, NULL);
	err(1, "failed to exec shell");
}

int main(int argc, char *argv[]) {
	enum keyi_mode mode = KEYI_CMD;

	optind = env_opts(argc, argv, false);

	int opt;
	while ((opt = getopt(argc, argv, "+eihv")) != -1) {
		switch (opt) {
		case 'e':
			mode = KEYI_EDIT;
			break;
		case 'i':
			mode = KEYI_SHELL;
			break;
		case 'h':
			printf(PROG_USAGE);
			exit(0);
		case 'v':
			printf(PROG_NAME " version " PROG_VERSION "\n");
			exit(0);
		case '?':
		default:
			printf(PROG_USAGE);
			exit(1);
		}
	}

	if ((optind == argc) != (mode == KEYI_SHELL)) {
		printf(PROG_USAGE);
		exit(1);
	}

	if (getresuid(&ruid, &euid, &suid) == -1) {
		err(1, "failed to getresuid");
	}

	if (getresgid(&rgid, &egid, &sgid) == -1) {
		err(1, "failed to getresgid");
	}

	rpw = getpwuid(ruid);
	if (!rpw) {
		err(1, "failed to getpwuid");
	}

	epw = getpwuid(euid);
	if (!epw) {
		err(1, "failed to getpwuid");
	}

	if (euid != 0) {
		warnx("operation requires root EUID");
#ifdef NDEBUG
		exit(1);
#endif
	}

	switch (mode) {
	case KEYI_CMD:
		run_cmd(argc, argv);
		break;
	case KEYI_SHELL:
		run_shell(argc, argv);
		break;
	case KEYI_EDIT:
		break;
	}

	// necessary?
	// we already have the EUID
	// set_root_privilege();

	// [fork] one is user editor, one is root
	// [root] copy each file to /tmp/keyi.* (mkstemp)
	// [user] exec editor with these files
	// [root] wait user process quit
	// [root] copy back if modified (sendfile)

	struct keyi_file **files = copy_all(argc, argv);

	env_opts(argc, argv, true);
	const char *editor = env_editor();
	char **new_argv = edit_argv(editor, files);

	// fork
	pid_t pid = fork();
	int wstatus;
	switch (pid) {
	case -1:
		err(1, "failed to fork");
	case 0:
		set_user();
		execvp(new_argv[0], new_argv);
		err(1, "failed to exec editor %s", editor);
	default:
		debugx("waiting for editor exit...");
		// wait
		waitpid(pid, &wstatus, 0);
		free_argv(new_argv);
		save_all(files);
		exit(0);
	}
}

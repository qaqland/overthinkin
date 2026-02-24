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
#define PROG_VERSION "0.3.0"
#endif

#define PROG_USAGE                                                             \
	"usage: " PROG_NAME " [NAME=VALUE] COMMAND [ARG]...\n"                 \
	"   or: " PROG_NAME " [NAME=VALUE] -i\n"                               \
	"   or: " PROG_NAME " -e FILE\n"

// sendfile() will transfer at most 0x7ffff000 (2,147,479,552) bytes
#define MAX_FILE_SIZE (0x7FFFF000)

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
	KEYI_EDIT,  // only one file
};

struct keyi_file {
	const char *src_path;
	const char *tmp_path;
	int src_fd;
	// int tmp_fd; // some editors may create a new file
	ino_t ino;
	struct timespec time;
};

uid_t ruid, euid, suid;
gid_t rgid, egid, sgid;
char rpw_name[256];
struct passwd *epw;

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

	debugx("using editor %s", name);
	return name;
}

void env_root(void) {
	const char *term = getenv("TERM");

	debugx("clear environment and set for root");
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
			debugx("not an environment variable %s", argv[i]);
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

// in sudo, a file will be created if it does not exist,
// but yeki does not implement. there are various cases:
//
// 1. file does not exist
// 2. directory does not exist
// 3. permissions are insufficient
//
// so this feature was not introduced for simplicity.
//
bool copy_one(struct keyi_file *f, const char *prefix) {
	assert(f);
	assert(f->src_path);
	assert(prefix);

	const char *path = f->src_path;
	f->src_fd = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
	if (f->src_fd == -1) {
		warn("cannot open file %s", path);
		goto err_out;
	}

	struct stat src_stat = {0};
	if (fstat(f->src_fd, &src_stat)) {
		warn("cannot get file status %s", path);
		goto clean_src;
	}
	if (!S_ISREG(src_stat.st_mode)) {
		warnx("not a regular file %s", path);
		goto clean_src;
	}

	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;

	static char buff[PATH_MAX] = {0};
	snprintf(buff, sizeof(buff), "%s/" PROG_NAME ".XXXXXX%s", prefix, base);

	int tmp_fd = mkostemps(buff, strlen(base), O_CLOEXEC);
	if (tmp_fd == -1) {
		warn("cannot create temporary file %s", buff);
		goto clean_src;
	}

	f->tmp_path = buff;

	off_t count = src_stat.st_size;
	if (count > MAX_FILE_SIZE) {
		warnx("file too large %s", f->src_path);
		goto clean_tmp;
	}
	off_t offset = 0;
	ssize_t sent = sendfile(tmp_fd, f->src_fd, &offset, count);
	if (sent != count) {
		warn("cannot copy from %s to %s", f->src_path, f->tmp_path);
		goto clean_tmp;
	}

	if (fchown(tmp_fd, ruid, rgid)) {
		warn("cannot change ownership %s", f->tmp_path);
		goto clean_tmp;
	}

	struct stat tmp_stat = {0};
	fstat(tmp_fd, &tmp_stat);
	f->time = tmp_stat.st_mtim;
	f->ino = tmp_stat.st_ino;

	debugx("copy file from %s to %s", path, f->tmp_path);
	close(tmp_fd);
	return true;

clean_tmp:
	close(tmp_fd);
	unlink(f->tmp_path);
clean_src:
	close(f->src_fd);
err_out:
	return false;
}

bool save_one(const struct keyi_file *f) {
	struct stat new_stat = {0};
	if (stat(f->tmp_path, &new_stat)) {
		warn("cannot get temporary file status %s", f->tmp_path);
		return false;
	}

	off_t count = new_stat.st_size;
	if (count > MAX_FILE_SIZE) {
		warnx("file too large %s", f->tmp_path);
		return false;
	}
	if (count == 0) {
		warnx("zero length temporary file %s", f->tmp_path);
		// but it should work
		// return false;
	}

	struct timespec tmp_time = f->time;
	struct timespec new_time = new_stat.st_mtim;

	debugx("tmp ino=%ud, sec=%ld, nsec=%ld", f->ino, tmp_time.tv_sec,
	       tmp_time.tv_nsec);
	debugx("new ino=%ud, sec=%ld, nsec=%ld", new_stat.st_ino,
	       new_time.tv_sec, new_time.tv_nsec);

	if (tmp_time.tv_sec == new_time.tv_sec &&
	    tmp_time.tv_nsec == new_time.tv_nsec && f->ino == new_stat.st_ino) {
		warnx("unchanged %s", f->src_path);
		return true;
	}

	// necessary!
	if (lseek(f->src_fd, 0, SEEK_SET) == -1) {
		warn("cannot seek in file %s", f->src_path);
		return false;
	}
	if (ftruncate(f->src_fd, 0)) {
		warn("cannot truncate file %s", f->src_path);
		return false;
	}

	int tmp_fd = open(f->tmp_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (tmp_fd == -1) {
		warn("cannot open temporary file %s", f->tmp_path);
		return false;
	}

	// copy
	off_t offset = 0;
	ssize_t sent = sendfile(f->src_fd, tmp_fd, &offset, count);
	close(tmp_fd);
	if (sent != count) {
		warn("cannot copy from %s to %s", f->tmp_path, f->src_path);
		return false;
	}

	if (fsync(f->src_fd) == -1) {
		warn("cannot sync file %s", f->src_path);
		return false;
	}

	debugx("copy %s back to %s", f->tmp_path, f->src_path);
	return true;
}

void set_root(void) {
	debugx("set root effective UID %d", euid);

	if (setgid(euid) == -1) {
		err(1, "cannot set group ID %d", euid);
	}
	if (setuid(euid) == -1) {
		err(1, "cannot set user ID %d", euid);
	}

	if (initgroups(epw->pw_name, euid) == -1) {
		err(1, "cannot initialize groups %d", euid);
	}
}

void set_user(void) {
	debugx("set user real UID %d", ruid);

	// 1 st
	if (setgid(rgid) == -1) {
		err(1, "cannot set group ID");
	}
	// 2 nd
	if (setuid(ruid) == -1) {
		err(1, "cannot set user ID");
	}
}

[[noreturn]] void run_cmd(int argc, char *argv[]) {
	env_root();
	env_opts(argc, argv, true);

	const char *exec_argv = argv[optind];

	const char *cwd = "(failed)";
	char cwdpath[PATH_MAX] = {0};

	if (getcwd(cwdpath, sizeof(cwdpath))) {
		cwd = cwdpath;
	}

	set_root();

	syslog(LOG_INFO | LOG_AUTH, "%s ran command %s as %s from %s", rpw_name,
	       exec_argv, epw->pw_name, cwd);
	execvp(exec_argv, &argv[optind]);
	err(1, "cannot execute command %s", exec_argv);
}

[[noreturn]] void run_shell(int argc, char *argv[]) {
	env_root();
	env_opts(argc, argv, true);

	const char *shell = getenv("SHELL");
	if (!shell) {
		shell = epw->pw_shell;
	}

	const char *home = epw->pw_dir;
	chdir(home);

	const char *shellname = strrchr(shell, '/');
	if (shellname) {
		shellname++;
	} else {
		shellname = shell;
	}

	char name[PATH_MAX] = {0};
	snprintf(name, PATH_MAX, "-%s", shellname);

	set_root();

	syslog(LOG_INFO | LOG_AUTH, "%s ran shell %s as %s from %s", rpw_name,
	       shell, epw->pw_name, home);
	execlp(shell, name, NULL);
	err(1, "cannot execute shell %s", shell);
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

	if (getresuid(&ruid, &euid, &suid) == -1) {
		err(1, "cannot get real/effective/saved user IDs");
	}

	if (getresgid(&rgid, &egid, &sgid) == -1) {
		err(1, "cannot get real/effective/saved group IDs");
	}

	struct passwd *rpw = getpwuid(ruid);
	if (!rpw) {
		err(1, "cannot get password entry for real user ID");
	}
	strncpy(rpw_name, rpw->pw_name, sizeof(rpw_name));

	epw = getpwuid(euid); // rpw is no longer available
	if (!epw) {
		err(1, "cannot get password entry for effective user ID");
	}

	if (euid != 0) {
		warnx("operation requires root EUID");
#ifdef NDEBUG
		exit(1);
#endif
	}

	// expected keyi permissions are 4750 or 4754
	char exe[PATH_MAX] = {0};
	ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (len < 0) {
		err(1, "cannot read symlink /proc/self/exe");
	}

	struct stat exe_stat;
	stat(exe, &exe_stat);
	debugx("%s permissions %04o", exe, exe_stat.st_mode & 07777);

	if (exe_stat.st_mode & S_IXOTH) {
		errx(1, "other-executable bit must not be set");
	}

	switch (mode) {
	case KEYI_CMD:
		if (argc - optind == 0) {
			printf(PROG_USAGE);
			exit(1);
		}
		run_cmd(argc, argv);
		break;
	case KEYI_SHELL:
		if (argc - optind != 0) {
			printf(PROG_USAGE);
			exit(1);
		}
		run_shell(argc, argv);
		break;
	case KEYI_EDIT:
		if (argc - optind != 1) {
			printf(PROG_USAGE);
			exit(1);
		}
		break;
	}

	// necessary?
	// we already have the EUID
	// set_root_privilege();

	// [fork] one is user editor, one is root
	// [root] copy file to /tmp/keyi.* (mkstemp)
	// [user] exec editor
	// [root] wait user process quit
	// [root] copy back if modified, delete tmp

	bool is_ok = true;

	struct keyi_file file = {0};
	file.src_path = argv[optind];

	is_ok = copy_one(&file, "/tmp");
	if (!is_ok) {
		exit(1);
	}

	env_opts(argc, argv, true);
	const char *editor = env_editor();

	char buff[256] = {0};
	snprintf(buff, sizeof(buff), "%s \"$@\"", editor);

	// export EDITOR="vim -u NONE"
	// sh -c '$EDITOR "$@"' vim test.c

	pid_t pid = fork();
	if (pid == 0) {
		set_user();
		execlp("sh", "sh", "-c", buff, editor, file.tmp_path, NULL);
		err(1, "cannot open editor %s", editor);
	}

	debugx("waiting for editor exit...");

	int wstatus;
	waitpid(pid, &wstatus, 0);

	usleep(10000); // 10ms

	do {
		is_ok = WIFEXITED(wstatus) != 0;
		if (!is_ok) {
			break;
		}
		is_ok = WEXITSTATUS(wstatus) == 0;
		if (!is_ok) {
			break;
		}
		is_ok = save_one(&file);
		if (!is_ok) {
			break;
		}
	} while (0);

	syslog(LOG_INFO | LOG_AUTH, "%s edited %s as %s with %s [%s]",
	       rpw->pw_name, file.src_path, epw->pw_name, editor,
	       is_ok ? "success" : "failure");

	close(file.src_fd);

	if (is_ok) {
#ifdef NDEBUG
		unlink(file.tmp_path);
#endif
		warnx("delete temporary file %s", file.tmp_path);
	} else {
		warnx("backup retained at %s", file.tmp_path);
	}

	exit(is_ok ? 0 : 1);
}

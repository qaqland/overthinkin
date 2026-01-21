#define _GNU_SOURCE

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
	MODE_NULL = 0,
	MODE_FORCE,
	MODE_PROMPT,
	MODE_SKIP,
};

static int mode = MODE_NULL;
static bool use_symlink = false;
static char *base = NULL;
static int topic_prefix = 0;

static void fd_cleanup(int *pfd) {
	if (!pfd)
		return;
	if (*pfd >= 0) {
		int saved = errno;
		close(*pfd);
		errno = saved;
		*pfd = -1;
	}
}

#define AUTO_FD int __attribute__((cleanup(fd_cleanup)))

static void print_help(void) {
	printf("usage: mkdot [-fins] TOPIC... BASE\n"
	       "   or: mkdot [-fins] -t BASE TOPIC...\n"
	       "\n"
	       "install dotfiles from TOPIC(s) to BASE\n"
	       "\n"
	       "  -f      overwrite existing files (default)\n"
	       "  -i      prompt before overwriting (interactive)\n"
	       "  -n      no overwrite, skip existing files\n"
	       "  -s      create symbolic links instead of copying\n"
	       "  -t BASE specify BASE directory for all TOPICs\n"
	       "");
}

static bool copy_file(const char *src, const char *dst) {
	AUTO_FD src_fd = open(src, O_RDONLY);
	if (src_fd < 0) {
		return false;
	}

	struct stat statbuf;
	if (fstat(src_fd, &statbuf) != 0) {
		return false;
	}
	mode_t mode = statbuf.st_mode;

	AUTO_FD dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (dst_fd < 0) {
		return false;
	}

	char buf[128 * 1024];
	ssize_t r, w;
	while ((r = read(src_fd, buf, sizeof(buf))) > 0) {
		ssize_t off = 0;
		while (off < r) {
			w = write(dst_fd, buf + off, (size_t) (r - off));
			if (w < 0) {
				if (errno == EINTR)
					continue;
				return false;
			}
			off += w;
		}
	}

	if (r < 0) {
		return false;
	}

	if (fchmod(dst_fd, mode) != 0) {
		return false;
	}

	return true;
}

static bool link_file(const char *src, const char *dst) {
	return symlink(src, dst) == 0;
}

static bool ensure_dir(const char *path, mode_t mode) {
	if (mkdir(path, mode) == 0) {
		return true;
	}
	if (errno == EEXIST) {
		return true;
	}
	printf("mkdir %s: %s\n", path, strerror(errno));
	return false;
}

static bool user_skip(const char *path) {
	printf("overwrite %s? (y/n): ", path);
	fflush(stdout);

	char response;
	scanf(" %c", &response);
	if (tolower(response) == 'y') {
		return false;
	}
	return true;
}

static const char *target_dst(const char *src) {
	static char buf[PATH_MAX];
	char *ptr = buf;

	assert(base);
	assert(src[0] != '/');

	// topic/dot-config	->	base/.config
	// topic/dot-		->	base/dot-

	int s = snprintf(buf, sizeof(buf), "%s/", base);
	ptr += s;

	src += topic_prefix;

	if (*src == '/') {
		src++;
	}

	while (*src) {
		if (strncmp(src - 1, "/dot-", 5) == 0) {
			if (*(src + 5) != '\0') {
				*ptr++ = '.';
			} else {
				strncpy(ptr, "dot-", 4);
				ptr += 4;
			}
			src += 4;
		} else {
			*ptr++ = *src++;
		}
	}

	*ptr = '\0';

	return buf;
}

static int install_handle(const char *fpath, const struct stat *sb,
			  int typeflag) {
	const char *src = fpath;
	const char *dst = target_dst(src);

	if (typeflag == FTW_D) {
		ensure_dir(dst, sb->st_mode);
		return 0;
	}

	if (typeflag != FTW_F) {
		return 0;
	}

	printf("install %s -> %s\n", src, dst);

	if (access(dst, F_OK) != 0) {
		goto write;
	}

	if (mode == MODE_SKIP) {
		printf("skip %s\n", dst);
		return 0;
	}

	if (mode == MODE_PROMPT && user_skip(dst)) {
		return 0;
	}

	if (unlink(dst) != 0) {
		printf("remove failed: %s\n", strerror(errno));
		return 0;
	}

write:
	if (use_symlink) {
		link_file(src, dst);
	} else {
		copy_file(src, dst);
	}
	return 0;
}

static bool install_topic(const char *name) {

	assert(mode);
	assert(base);

	printf("install topic %s\n", name);

	struct stat statbuf;
	if (stat(name, &statbuf) != 0) {
		printf("stat error %s\n", strerror(errno));
		return false;
	}
	if (!S_ISDIR(statbuf.st_mode)) {
		printf("not dir\n");
		return false;
	}

	topic_prefix = strlen(name);

	ftw(name, install_handle, 16);

	return true;
}

int main(int argc, char *argv[]) {
	char **topics = NULL;

	int c;
	while ((c = getopt(argc, argv, "finsvht:")) != -1) {
		switch (c) {
		case 'f':
			if (mode != MODE_NULL) {
				return 1;
			}
			mode = MODE_FORCE;
			break;
		case 'i':
			if (mode != MODE_NULL) {
				return 1;
			}
			mode = MODE_PROMPT;
			break;
		case 'n':
			if (mode != MODE_NULL) {
				return 1;
			}
			mode = MODE_SKIP;
			break;
		case 's':
			use_symlink = true;
			break;
		case 'v':
			// print version
			break;
		case 't':
			if (!optarg) {
				return 1;
			}
			free(base);
			base = strdup(optarg);
			break;
		case 'h':
			print_help();
			return 0;
		default:
			print_help();
			return 1;
		}
	}

	int number = argc - optind;
	if (number < (base ? 1 : 2)) {
		print_help();
		return 1;
	}

	if (mode == MODE_NULL) {
		mode = MODE_FORCE;
	}

	if (mode == MODE_FORCE) {
		printf("force overwrite mode\n");
	}

	topics = calloc(number + 1, sizeof(char *));
	if (base) {
		memcpy(topics, argv + optind, number * sizeof(char *));
	} else {
		memcpy(topics, argv + optind, (number - 1) * sizeof(char *));
		base = strdup(argv[argc - 1]);
	}

	printf("base is %s\n", base);

	char *t = realpath(base, NULL);
	if (!t) {
		printf("realpath error: %s\n", strerror(errno));
		return 1;
	}
	free(base);
	base = t;

	for (int i = 0; topics[i]; i++) {
		install_topic(topics[i]);
	}

	free(base);
	free(topics);
	return 0;
}

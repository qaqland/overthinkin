/*
 * check_sed_mtime.c - 检查 sed -i 修改文件后 mtime 是否总是更新
 * 使用 fork() 和 execlp() 执行 sed 命令
 * 编译: gcc -o check_sed_mtime check_sed_mtime.c -D_GNU_SOURCE
 * 使用: ./check_sed_mtime [迭代次数]
 */

// 定义 _GNU_SOURCE 以启用 strdup, mkdtemp 等函数
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_ITERATIONS 1000
#define MAX_PATH 1024
#define MAX_CMD 4096

// 结构体存储文件时间信息
typedef struct {
	time_t sec; // 秒级时间戳
	long nsec;  // 纳秒部分
} file_time_t;

// 结构体存储测试结果
typedef struct {
	int total_iterations;
	int failed_iterations;
	int same_mtime_count;
} test_result_t;

// 获取文件的修改时间（秒+纳秒）- Linux专用版本
static int get_file_mtime(const char *filename, file_time_t *mtime) {
	struct stat st;

	if (stat(filename, &st) == -1) {
		perror("stat failed");
		return -1;
	}

	mtime->sec = st.st_mtime;
	// Linux uses st_mtim, while macOS uses st_mtimespec.
#if defined(__APPLE__)
	mtime->nsec = st.st_mtimespec.tv_nsec;
#else
	mtime->nsec = st.st_mtim.tv_nsec;
#endif

	return 0;
}

// 比较两个文件时间是否相等
static int is_same_mtime(const file_time_t *t1, const file_time_t *t2) {
	return (t1->sec == t2->sec && t1->nsec == t2->nsec);
}

// 使用 fork + execlp 执行 sed 命令
static int run_sed_command(const char *filename, int iteration) {
	pid_t pid;
	int status;
	char sed_cmd[256];

	// 构建 sed 命令参数：将文件内容替换为迭代次数
	snprintf(sed_cmd, sizeof(sed_cmd), "s/.*/%d/", iteration);

	pid = fork();
	if (pid == -1) {
		perror("fork failed");
		return -1;
	}

	if (pid == 0) {
		// 子进程：执行 sed 命令
		execlp("sed", "sed", "-i", sed_cmd, filename, NULL);

		// 如果 execlp 失败
		fprintf(stderr, "execlp failed: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	} else {
		// 父进程：等待子进程完成
		if (waitpid(pid, &status, 0) == -1) {
			perror("waitpid failed");
			return -1;
		}

        // usleep(1000); // 等待1毫秒，确保文件系统更新 mtime
        // sleep(2);

		if (WIFEXITED(status)) {
			int exit_code = WEXITSTATUS(status);
			if (exit_code != 0) {
				fprintf(stderr,
					"sed command failed with exit code "
					"%d\n",
					exit_code);
				return -1;
			}
			return 0; // 成功
		} else if (WIFSIGNALED(status)) {
			fprintf(stderr, "sed command terminated by signal %d\n",
				WTERMSIG(status));
			return -1;
		}
	}

	return -1;
}

// 创建临时目录
static char *create_temp_dir(void) {
	char *template = strdup("/tmp/sed_mtime_test_XXXXXX");
	if (!template) {
		perror("strdup failed");
		return NULL;
	}

	char *dir = mkdtemp(template);
	if (!dir) {
		perror("mkdtemp failed");
		free(template);
		return NULL;
	}

	return dir;
}
// 主测试函数
static int run_test(int iterations, test_result_t *result) {
	char *temp_dir = NULL;
	char test_file[MAX_PATH];
	FILE *fp;
	int i;
	file_time_t prev_mtime, curr_mtime;
	ino_t prev_inode, curr_inode;
	struct stat st;

	// 初始化结果
	memset(result, 0, sizeof(test_result_t));

	// 创建临时目录
	temp_dir = create_temp_dir();
	if (!temp_dir) {
		return -1;
	}

	printf("测试目录: %s\n", temp_dir);

	// 创建测试文件路径
	snprintf(test_file, sizeof(test_file), "%s/test.txt", temp_dir);
	printf("测试文件: %s\n", test_file);
	printf("迭代次数: %d\n\n", iterations);

	// 创建测试文件
	fp = fopen(test_file, "w");
	if (!fp) {
		perror("创建测试文件失败");
		free(temp_dir);
		return -1;
	}
	fprintf(fp, "Initial content\n");
	fclose(fp);

	// 获取初始文件信息
	if (get_file_mtime(test_file, &prev_mtime) == -1) {
		free(temp_dir);
		return -1;
	}
	if (stat(test_file, &st) == -1) {
		perror("获取inode失败");
		free(temp_dir);
		return -1;
	}
	prev_inode = st.st_ino;

	printf("开始测试，请稍候...\n");

	for (i = 1; i <= iterations; i++) {
		// 执行 sed 命令
		if (run_sed_command(test_file, i) == -1) {
			fprintf(stderr,
				"警告: sed 命令在第 %d 次迭代失败，跳过本次\n",
				i);
			result->failed_iterations++;
			continue;
		}

		// 获取当前文件信息
		if (get_file_mtime(test_file, &curr_mtime) == -1) {
			free(temp_dir);
			return -1;
		}
		if (stat(test_file, &st) == -1) {
			perror("获取inode失败");
			free(temp_dir);
			return -1;
		}
		curr_inode = st.st_ino;

		// 检查 mtime 是否变化
		if (is_same_mtime(&prev_mtime, &curr_mtime)) {
			result->same_mtime_count++;

			printf("===== 第 %d 次迭代：mtime 未变化 =====\n", i);
			printf("  前一次 mtime: %ld.%09ld  inode: %lu\n",
			       prev_mtime.sec, prev_mtime.nsec,
			       (unsigned long) prev_inode);
			printf("  当前  mtime: %ld.%09ld  inode: %lu\n",
			       curr_mtime.sec, curr_mtime.nsec,
			       (unsigned long) curr_inode);

			// 读取并显示文件内容
			fp = fopen(test_file, "r");
			if (fp) {
				char content[256];
				if (fgets(content, sizeof(content), fp)) {
					printf("  文件内容: %s", content);
				}
				fclose(fp);
			}
			printf("\n");
		}

		// 更新前一次的值
		prev_mtime = curr_mtime;
		prev_inode = curr_inode;
		result->total_iterations++;

		// 每100次迭代显示进度
		if (i % 100 == 0) {
			printf("进度: %d/%d (%.1f%%)\n", i, iterations,
			       (float) i / iterations * 100);
		}
	}

	// 清理临时目录
	printf("清理临时目录...\n");
	char rm_cmd[MAX_PATH + 20];
	snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf \"%s\"", temp_dir);
	system(rm_cmd);
	free(temp_dir);

	return 0;
}

// 打印测试结果
static void print_results(const test_result_t *result) {
	printf("\n=== 测试结果 ===\n");
	printf("总迭代次数: %d\n",
	       result->total_iterations + result->failed_iterations);
	printf("有效迭代次数: %d\n", result->total_iterations);
	printf("失败迭代次数: %d\n", result->failed_iterations);
	printf("mtime 未变化次数: %d\n", result->same_mtime_count);

	if (result->total_iterations > 0) {
		double probability = (double) result->same_mtime_count /
				     result->total_iterations * 100;
		printf("未变化概率: %.2f%%\n", probability);
	}

	if (result->failed_iterations > 0) {
		printf("\n警告: 有 %d 次迭代失败，可能影响测试结果\n",
		       result->failed_iterations);
	}
}

int main(int argc, char *argv[]) {
	int iterations = DEFAULT_ITERATIONS;
	test_result_t result;

	// 解析命令行参数
	if (argc > 1) {
		char *endptr;
		long val = strtol(argv[1], &endptr, 10);

		if (endptr == argv[1] || *endptr != '\0' || val <= 0) {
			fprintf(stderr, "错误: 迭代次数必须是正整数\n");
			fprintf(stderr, "使用方法: %s [迭代次数]\n", argv[0]);
			return EXIT_FAILURE;
		}

		if (val > 1000000) {
			fprintf(stderr,
				"警告: 迭代次数 %ld "
				"较大，测试可能需要较长时间\n",
				val);
		}

		iterations = (int) val;
	}

	printf("sed mtime 测试工具 (C语言版本)\n");
	printf("===============================\n\n");

	// 运行测试
	if (run_test(iterations, &result) == -1) {
		fprintf(stderr, "测试失败\n");
		return EXIT_FAILURE;
	}

	// 打印结果
	print_results(&result);

	return EXIT_SUCCESS;
}

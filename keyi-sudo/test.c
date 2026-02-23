
#include <err.h>
#include <stdlib.h>

#include <CUnit/Basic.h>
#include <CUnit/CUnit.h>
#include <CUnit/TestDB.h>

#define main main_dumb
#include "keyi.c"
#undef main

#ifdef NDEBUG
#undef NDEBUG
#endif

static void test_resugid(void) {
	debugx("{r,e,s}{u,g}id are always initialized to 0");

	CU_ASSERT_EQUAL(ruid, 0);
	CU_ASSERT_EQUAL(rgid, 0);

	CU_ASSERT_EQUAL(euid, 0);
	CU_ASSERT_EQUAL(egid, 0);

	CU_ASSERT_EQUAL(suid, 0);
	CU_ASSERT_EQUAL(sgid, 0);
}

static void test_env_opts(void) {
	int (*f)(int, char *[], bool write) = env_opts;
	{
		unsetenv("FOO");
		char *argv[] = {"keyi", "FOO=BAR", "whoami", NULL};
		int argc = sizeof(argv) / sizeof(argv[0]);

		int optind = f(argc, argv, true);

		CU_ASSERT_EQUAL(optind, 2);
		CU_ASSERT_STRING_EQUAL(argv[optind], "whoami");
		CU_ASSERT_STRING_EQUAL(getenv("FOO"), "BAR");
	}
	{
		unsetenv("FOO");
		char *argv[] = {"keyi", "FOO=BAR", "whoami", NULL};
		int argc = sizeof(argv) / sizeof(argv[0]);

		int optind = f(argc, argv, false);

		CU_ASSERT_EQUAL(optind, 2);
		CU_ASSERT_STRING_EQUAL(argv[optind], "whoami");
		CU_ASSERT_PTR_NULL(getenv("FOO"));
	}
}

static void test_env_editor(void) {
	const char *(*f)(void) = env_editor;
	const char *value = NULL;

	unsetenv("VISUAL");
	unsetenv("SUDO_EDITOR");

	setenv("EDITOR", "vim", 1);
	value = f();

	setenv("EDITOR", "vim -u NONE", 1);
	value = f();

	unsetenv("EDITOR");
	value = f();
	CU_ASSERT_STRING_EQUAL(value, "vi");
}

int main() {
	CU_initialize_registry();

	CU_pSuite base = CU_add_suite("base", NULL, NULL);
	CU_ADD_TEST(base, test_resugid);
	CU_ADD_TEST(base, test_env_opts);
	CU_ADD_TEST(base, test_env_editor);

	CU_basic_run_tests();
	CU_cleanup_registry();
	return 0;
}

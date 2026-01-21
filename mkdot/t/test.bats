setup() {
	bats_load_library bats-support
	bats_load_library bats-assert
	bats_load_library bats-file

	cd "$BATS_TEST_DIRNAME"/fixtures
	BASE=$(temp_make)
}

teardown() {
	temp_del "$BASE"	
}

# usage: mkdot [-fins] TOPIC... BASE
#    or: mkdot [-fins] -t BASE TOPIC...
#
# install dotfiles from TOPIC(s) to BASE
#
#   -f      overwrite existing files (default)
#   -i      prompt before overwriting (interactive)
#   -n      no overwrite, skip existing files
#   -s      create symbolic links instead of copying
#   -t BASE specify BASE directory for all TOPICs

@test "print help" {
	run ./mkdot -h
	assert_output -p 'usage'
}

@test "with -t BASE" {
	./mkdot -t "$BASE" my-topic
	assert_file_exists "$BASE"/my-config
}

@test "without -t" {
	./mkdot my-topic "$BASE"
	assert_file_exists "$BASE"/my-config
}

@test "use copy" {
	./mkdot -t "$BASE" my-topic
	assert_files_equal "$BASE"/my-config my-topic/my-config
}

@test "use link" {
	./mkdot -s -t "$BASE" my-topic
	assert_link_exists "$BASE"/my-config
	assert_symlink_to my-topic/my-config "$BASE"/my-config
}

@test "skip existing files" {
	./mkdot -f -t "$BASE" bk-topic
	assert_file_contains "$BASE"/my-config before

	./mkdot -n -t "$BASE" my-topic
	assert_file_contains "$BASE"/my-config before
}

@test "force overwrite" {
	./mkdot -f -t "$BASE" bk-topic
	assert_file_contains "$BASE"/my-config before

	./mkdot -f -t "$BASE" my-topic
	assert_file_contains "$BASE"/my-config hello
}

@test "/dot-config" {
	./mkdot -t "$BASE" dot-topic
	assert_file_exists "$BASE"/dot-
	assert_file_exists "$BASE"/.vimrc
}

@test "multi-topics" {
	./mkdot -t "$BASE" dot-topic bk-topic my-topic
}


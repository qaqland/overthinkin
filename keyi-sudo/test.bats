setup_file() {
	cd "$BATS_TEST_DIRNAME"/t || exit

	for file in *.orig; do
		[ -e "$file" ] || continue
		cp "$file" "${file%.orig}.got"
	done
}

# .orig
# .got
# .exp


teardown_file() {
	:
	rm /tmp/keyi.* || true
}

setup() {
	bats_load_library bats-support
	bats_load_library bats-assert
	bats_load_library bats-file
}

@test "print help" {
	run ./keyi -h
	assert_output -p 'usage'
}

@test "print env" {
	run ./keyi FOO=BAR printenv FOO
	assert_output -p BAR
}

@test "edit with false (exit 1)" {
	export EDITOR=false
	run ./keyi -e unchanged.got
	assert_output -p 'backup retained'
}

@test "edit with cat editor (unchanged)" {
	export EDITOR=cat
	run ./keyi -e unchanged.got
	assert_output -p 'unchanged'
	assert_files_equal unchanged.got unchanged.exp
}

@test "edit with sed insert at first line" {
	export EDITOR="sed -i '1iprefix'"
	run ./keyi -e prefix.got
	refute_output -p 'unchanged'
	assert_files_equal prefix.got prefix.exp
}

@test "edit with sed append at last line" {
	export EDITOR="sed -i '\$aappended line'"
	run ./keyi -e append.got
	refute_output -p 'unchanged'
	assert_files_equal append.got append.exp
}

@test "edit with sed delete first line" {
	export EDITOR="sed -i '1d'"
	run ./keyi -e delete-first.got
	refute_output -p 'unchanged'
	assert_files_equal delete-first.got delete-first.exp
}

@test "edit with sed delete last line" {
	export EDITOR="sed -i '\$d'"
	run ./keyi -e delete-last.got
	refute_output -p 'unchanged'
	assert_files_equal delete-last.got delete-last.exp
}

@test "edit with sed replace foo with bar" {
	export EDITOR="sed -i 's/foo/bar/g'"
	run ./keyi -e replace.got
	refute_output -p 'unchanged'
	assert_files_equal replace.got replace.exp
}


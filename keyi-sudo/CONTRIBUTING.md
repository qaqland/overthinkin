# Contributing

## CMake build

After `cmake --install`, you must manually set the target group and `suid` bit for `keyi`.

```
$ cmake -B build
$ cmake --build build

$ cmake -L -B build
$ ROOT cmake --install build

$ ROOT chown root:wheel /usr/local/bin/keyi
$ ROOT chmod u+s,o-x /usr/local/bin/keyi
```

## Makefile tests

Tests require extra dependencies:

- `cunit-dev`
- `bats-core`
- `bats-support`
- `bats-assert`
- `bats-file`

```
$ make -C tests keyi
$ make -C tests integration-test
$ make -C tests unit-test
```

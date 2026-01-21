# mkdot

dotfiles installer

```
$ mkdot -h
usage: mkdot [-fins] TOPIC... BASE
   or: mkdot [-fins] -t BASE TOPIC...

install dotfiles from TOPIC(s) to BASE

  -f      overwrite existing files (default)
  -i      prompt before overwriting (interactive)
  -n      no overwrite, skip existing files
  -s      create symbolic links instead of copying
  -t BASE specify BASE directory for all TOPICs
```

## example

prepare a topic

```
my-topic/
├── dot-vimrc
└── some-profile
```

install this topic with mkdot

```
$ mkdot -t /tmp/test my-topic
```

result

```
/tmp/test/
├── .vimrc
└── some-profile
```


# Git Protocol

`git-plain-clone` 通过 HTTP(S) smart Git 协议 v1 获取仓库快照。

## 协议版本

Git 支持多种传输后端：

- <https://git-scm.com/book/en/v2/Git-on-the-Server-The-Protocols>
- <https://git-scm.com/book/en/v2/Git-Internals-Transfer-Protocols>
- `man gitprotocol-http`
- `man gitprotocol-capabilities`

`git-plain-clone` 只实现 Smart HTTP 的读取子集（`git-upload-pack`），
不支持 SSH、git-daemon、Dumb HTTP 以及写入操作，所以使用 v1 版本协议。

## 调试观察

设置环境变量可以让官方 `git` 命令打印协议细节。为匹配本项目行为，建议显式
指定 protocol version 1：

```bash
export GIT_TRACE=1
export GIT_CURL_VERBOSE=1
export GIT_TRACE_PACKET=1
git -c protocol.version=1 clone --depth=1 https://github.com/qaqland/overthinkin.git
```

## git-server

本项目自带一个简易版 git-server，可以使用打印出的 URL 来测试

```bash
$ ./build/gitserver -root ~/overthinkin/ -timeout 1h
http://127.0.0.1:38941
```

GET 请求 `info/refs`：

```bash
$ curl -i http://127.0.0.1:38941/.git/info/refs?service=git-upload-pack 2>/dev/null --output -
HTTP/1.1 200 OK
Cache-Control: no-cache
Content-Type: application/x-git-upload-pack-advertisement
Date: Mon, 13 Jul 2026 03:04:46 GMT
Content-Length: 584

001e# service=git-upload-pack
00000111060c018be4b648c7dc1ed1274436e3aa74a31f9f HEADmulti_ack thin-pack side-band side-band-64k ofs-delta shallow deepen-since deepen-not deepen-relative no-progress include-tag multi_ack_detailed no-done symref=HEAD:refs/heads/main object-format=sha1 agent=git/2.54.0-Linux
003d060c018be4b648c7dc1ed1274436e3aa74a31f9f refs/heads/main
0046060c018be4b648c7dc1ed1274436e3aa74a31f9f refs/remotes/origin/HEAD
0046060c018be4b648c7dc1ed1274436e3aa74a31f9f refs/remotes/origin/main
004811766a443a189f582a6edc8e3c7310514bb63557 refs/tags/keyi-sudo-v0.3.3
0000
```

### upload-pack

TODO

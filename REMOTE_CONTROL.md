# WindowsTerminalRemote — Remote Control API

*[English](#english) · [中文](#中文)*

**WindowsTerminalRemote** is a fork of Windows Terminal that adds a **local Remote Control API**: a small,
localhost-only HTTP + WebSocket server, built into the terminal, that lets other
local programs **attach to a visible pane** — read its output, write input, and
stream it live — while you keep using the same pane normally. It is **off by
default**, **loopback-only**, and every request requires a token.

It is *not* a remote command executor and *not* an external hook: input goes
through the terminal's own connection (PTY/stdin) path and output comes from the
terminal's output stream. No global keyboard hooks, no DLL injection, no reading
other processes, no controlling other Windows Terminal instances.

---

## English

### Enabling it

Open **Settings → Remote control** (left sidebar) and:

1. Turn on **Enable the remote control API**.
2. Type an **access token** and click **Apply**. The token is stored only as a
   PBKDF2 hash (`pbkdf2$iterations$salt$hash`); the plaintext is never written to
   disk, so keep your own copy for your clients. The API will not start until a
   token is set, and no token is ever auto-generated.

Equivalent `settings.json` (global object):

```jsonc
{
    "remoteControl": {
        "enabled": false,          // off by default
        "host": "127.0.0.1",       // loopback only (shown read-only in the UI)
        "port": 9177,
        "token": "",               // a pbkdf2$… hash, set via the Settings UI
        "allowInput": true,        // when false: input/key/interrupt -> 403
        "allowSnapshot": true,     // when false: snapshot -> 403
        "allowWebSocket": true     // when false: stream -> 403
    }
}
```

> Security: the server only binds to `host`. The default is loopback. Binding to
> a non-loopback address (e.g. `0.0.0.0`) is possible but strongly discouraged
> and is logged as a warning. Plaintext tokens are rejected — a scraped settings
> file never yields a usable token.

### Authentication

Every request (HTTP and WebSocket) must present the token:

- Header: `Authorization: Bearer <token>` (recommended), or
- Query: `?token=<token>` (development convenience only).

Missing/invalid → `401`.

### REST endpoints (all under `/v1`)

| Method & path | Description |
|---|---|
| `GET /v1/health` | `{ "ok": true, "app": "WindowsTerminalRemote", "version": "0.1.0" }` |
| `GET /v1/sessions` | List attachable panes (see fields below). |
| `GET /v1/sessions/:id` | Metadata for one session (`404` if unknown). |
| `GET /v1/sessions/:id/snapshot` | Visible viewport snapshot. `?color=0` for plain text only. |
| `POST /v1/sessions/:id/input` | `{ "data": "echo hi\r" }` — raw input to the pane. |
| `POST /v1/sessions/:id/key` | `{ "key": "enter" }` — a named special key. |
| `POST /v1/sessions/:id/resize` | `{ "cols": 120, "rows": 30 }` — records the remote view size (does **not** resize the local PTY in v1). |
| `POST /v1/sessions/:id/interrupt` | Sends Ctrl-C. |
| `POST /v1/sessions/:id/close` | `501` (not implemented in v1). |
| `WS  /v1/sessions/:id/stream` | Live output stream (see below). |

Status codes: `401` (no/invalid token), `404` (unknown session), `403`
(`allowInput`/`allowSnapshot`/`allowWebSocket` disabled, or server locked down),
`501` (`close`).

**Session metadata**

```json
{
  "sessionId": "string", "windowId": "string", "tabId": "string", "paneId": "string",
  "title": "string", "profileName": "string",
  "processName": "string|null", "cwd": "string|null",
  "rows": 30, "cols": 120, "isFocused": true, "isAlive": true,
  "remoteAttachedCount": 0, "createdAt": "ISO-8601", "lastOutputAt": "ISO-8601|null"
}
```

**Snapshot** — `text` is always present; `cells` (rows of style runs) is included
unless `?color=0`:

```json
{
  "sessionId": "...", "cols": 120, "rows": 30, "cursorX": 0, "cursorY": 0,
  "text": "visible text…", "timestamp": "ISO-8601",
  "cells": [ [ { "text": "PS C:\\>", "fg": "#cccccc", "bg": "#0c0c0c", "bold": false, "italic": false, "underline": false } ] ]
}
```

Named keys for `/key` and WS `key`: `enter, tab, escape, ctrl-c, ctrl-d, up,
down, left, right, backspace, delete`.

### WebSocket stream

Connect to `WS /v1/sessions/:id/stream` (token via header or `?token=`). The
server sends an initial `snapshot`, then live `output`. The client may send
`input`, `key` and `resize`. Every server event shares the envelope
`{ type, sessionId, timestamp, … }`:

```json
{ "type": "snapshot", "sessionId": "...", "cols": 120, "rows": 30, "text": "...", "cursorX": 0, "cursorY": 0, "cells": [...], "timestamp": "ISO-8601" }
{ "type": "output",   "sessionId": "...", "data": "raw VT (with ANSI colors)", "timestamp": "ISO-8601" }
{ "type": "exit",     "sessionId": "...", "code": 0, "timestamp": "ISO-8601" }
{ "type": "error",    "sessionId": "...", "message": "...", "timestamp": "ISO-8601" }
```

Client → server:

```json
{ "type": "input",  "data": "ls\r" }
{ "type": "key",    "key": "ctrl-c" }
{ "type": "resize", "cols": 120, "rows": 30 }
```

### Safety: destructive-command kill-switch

Input is screened for commands that would wipe the **whole filesystem root or an
entire drive** — `rm -rf /`, `rm -rf /*`, `del`/`rd /s X:\`, `format X:`,
`Remove-Item -Recurse X:\` (including `sudo`/`env VAR=` wrappers). Deleting an
ordinary directory (e.g. `rm -rf ./build`) is **not** affected. On a hit the
server refuses the command, **drops every connection**, and **locks down** —
all requests then return `403` until Windows Terminal is restarted or the
`remoteControl` setting is toggled.

### Logging

Activity is written to `remote-control.log` in your settings folder
(`%LOCALAPPDATA%\Microsoft\Windows Terminal\`), size-capped and rotating.
Entries are structured (timestamp, level, event, fields) and include server
start/stop, non-loopback warnings, **auth failures with peer address**, input
**length only (never contents)**, and `CRITICAL` lock-down events.

### Example

```bash
TOKEN=your-token
BASE=http://127.0.0.1:9177/v1
curl -s -H "Authorization: Bearer $TOKEN" $BASE/sessions
SID=...   # a sessionId from above
curl -s -H "Authorization: Bearer $TOKEN" "$BASE/sessions/$SID/snapshot?color=0"
curl -s -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
     -d '{"data":"echo hello\r"}' $BASE/sessions/$SID/input
```

### Portable mode

The fork also builds a portable (no-install) ZIP. Unzip it and run
`WindowsTerminal.exe`; settings live in a `settings` folder next to the exe. The
Remote Control API works identically in packaged and portable modes.

---

## 中文

本仓库在 Windows Terminal 内置了一个**本地远程控制 API**:一个仅监听本机的
HTTP + WebSocket 小服务器,让本机其它程序可以**接管一个可见 pane** —— 读取输出、
写入输入、实时订阅 —— 同时你在本地照常使用同一个 pane。它**默认关闭**、**仅
loopback**,且每个请求都需要 token。

它**不是**远程命令执行器,也**不是**外部 hook:输入走终端自身的连接(PTY/stdin)
通路,输出来自终端的输出流。没有全局键盘 hook、没有 DLL 注入、不读其它进程、
不控制其它 Windows Terminal 实例。

### 开启

打开 **设置 → 远程控制(Remote control)**(左侧栏):

1. 打开 **启用远程控制 API**。
2. 填一个 **访问 token** 并点 **应用**。token 仅以 PBKDF2 哈希
   (`pbkdf2$迭代$盐$哈希`)存储,**明文绝不落盘**,所以请自己保存一份给客户端用。
   **没设 token 服务器不会启动,也绝不自动生成 token。**

对应的 `settings.json`(全局对象):

```jsonc
{
    "remoteControl": {
        "enabled": false,          // 默认关
        "host": "127.0.0.1",       // 仅本机(UI 中只读展示)
        "port": 9177,
        "token": "",               // pbkdf2$… 哈希,通过设置页写入
        "allowInput": true,        // false 时 input/key/interrupt 返回 403
        "allowSnapshot": true,     // false 时 snapshot 返回 403
        "allowWebSocket": true     // false 时 stream 返回 403
    }
}
```

> 安全:服务器只绑定 `host`,默认 loopback。绑定到非 loopback(如 `0.0.0.0`)
> 虽可但强烈不建议,且会记一条告警日志。明文 token 一律拒绝——即使 settings
> 文件被爬,也拿不到可用 token。

### 鉴权

每个请求(HTTP 与 WebSocket)都必须带 token:

- 请求头:`Authorization: Bearer <token>`(推荐),或
- 查询参数:`?token=<token>`(仅开发调试用)。

缺失/错误 → `401`。

### REST 接口(均在 `/v1` 下)

| 方法与路径 | 说明 |
|---|---|
| `GET /v1/health` | `{ "ok": true, "app": "WindowsTerminalRemote", "version": "0.1.0" }` |
| `GET /v1/sessions` | 列出可接管的 pane。 |
| `GET /v1/sessions/:id` | 单个会话元数据(未知 → `404`)。 |
| `GET /v1/sessions/:id/snapshot` | 可见区快照。`?color=0` 只要纯文本。 |
| `POST /v1/sessions/:id/input` | `{ "data": "echo hi\r" }` —— 原始输入。 |
| `POST /v1/sessions/:id/key` | `{ "key": "enter" }` —— 命名特殊键。 |
| `POST /v1/sessions/:id/resize` | `{ "cols": 120, "rows": 30 }` —— v1 仅记录远程视图尺寸,**不**改本地 PTY。 |
| `POST /v1/sessions/:id/interrupt` | 发送 Ctrl-C。 |
| `POST /v1/sessions/:id/close` | `501`(v1 未实现)。 |
| `WS  /v1/sessions/:id/stream` | 实时输出流。 |

状态码:`401`(无/错 token)、`404`(会话不存在)、`403`(对应 `allow*` 关闭,或
服务器已锁定)、`501`(`close`)。

**会话元数据**、**快照(含彩色 `cells`)**、**命名键** 字段同上方英文小节。

### WebSocket 流

连到 `WS /v1/sessions/:id/stream`(token 走 header 或 `?token=`)。服务器先发一次
`snapshot`,随后持续 `output`;客户端可发 `input`/`key`/`resize`。所有服务端事件
共用信封 `{ type, sessionId, timestamp, … }`(`snapshot`/`output`/`exit`/`error`)。
客户端消息:`{type:"input",data}` / `{type:"key",key}` / `{type:"resize",cols,rows}`。

### 安全:毁灭性命令熔断

会检查输入里是否有**清空整个文件系统根目录或整块磁盘**的命令 —— `rm -rf /`、
`rm -rf /*`、`del`/`rd /s X:\`、`format X:`、`Remove-Item -Recurse X:\`(能识破
`sudo`/`env VAR=` 前缀)。删除**普通目录**(如 `rm -rf ./build`)**不受影响**。
一旦命中:拒绝该命令、**打掉所有连接**、并**进入锁定**——之后所有请求返回 `403`,
直到重启 Windows Terminal 或重新开关 `remoteControl` 设置。

### 日志

活动写入设置目录下的 `remote-control.log`
(`%LOCALAPPDATA%\Microsoft\Windows Terminal\`),限大小并自动轮转。条目为结构化
(时间戳、级别、事件、字段),包含启动/停止、非 loopback 告警、**带来源 IP 的鉴权
失败**、输入**只记长度(绝不记内容)**、以及熔断的 `CRITICAL` 事件。

### Portable(免安装)

本仓库还会构建一个 portable(免安装)ZIP:解压后直接运行 `WindowsTerminal.exe`,
设置存在 exe 旁的 `settings` 文件夹里。远程控制 API 在打包版与 portable 版行为一致。

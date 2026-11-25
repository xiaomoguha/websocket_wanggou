# websocket_wanggou

基于 C 语言 + libwebsockets 的「一起听」音乐房间 WebSocket 服务器。支持多房间、实时播放同步、播放列表协作管理、房间聊天、歌曲点赞、按成员私人 FM 风格自动推荐等功能，为 [WangGouMusic](https://github.com/xiaomoguha/WangGouMusic) 客户端提供实时同步服务。

## 功能特性

- **多房间管理** — 客户端携带 `roomid` 连接，房间不存在时自动创建；房间内最后一个用户离开后自动销毁并释放资源；长时间无人添加歌曲的房间会被回收（详见[空闲回收](#空闲回收)）
- **播放列表协作** — 房间内任意成员都可添加、删除、置顶歌曲，操作实时广播给所有人；支持单曲和批量加歌
- **实时播放同步** — 服务端通过定时器跟踪播放进度，周期性广播进度百分比，所有客户端进度保持一致
- **房间推荐** — 没歌可播时自动补歌：优先按某位成员的私人 FM 风格推荐，失败则回退多来源的系统推荐（详见[房间推荐机制](#房间推荐机制)）
- **预取不插队** — 提前准备好的推荐歌曲只放在「待播位」，不进播放列表、不广播，等当前歌曲播完且没有下一首时才插入
- **歌曲点赞** — 房间内共享的点赞数和点赞人列表，再次点赞即取消；点赞 / 取消会写入房间操作记录
- **房间聊天** — 支持房间内实时文字聊天
- **操作历史** — 记录房间内的各类操作（加入 / 离开 / 切歌 / 加歌 / 点赞等），新成员加入时会收到历史记录
- **HTTP 房间列表** — 提供 `GET /rooms` 接口查询当前活跃房间
- **监控事件** — 把房间 / 成员 / 播放 / 聊天 / 消息收发等事件以 JSONL 追加写入文件，供外部采集
- **大消息处理** — 接收端按分片累积后再解析，单条消息最大 512KB
- **向下兼容** — 新动作 214 ~ 217 追加在枚举末尾，新增字段老客户端可直接忽略
- **反代支持** — 自动解析 `X-Real-IP` / `X-Forwarded-For` 头，适配 Nginx 反向代理部署
- **守护进程** — 支持以 daemon 模式后台运行，支持日志文件输出

## 技术栈

| 技术 | 说明 |
|------|------|
| C (C99) | 核心逻辑实现 |
| libwebsockets | WebSocket 服务框架，事件循环 + 定时器（`lws_sorted_usec_list_t`） |
| libcurl | HTTP 请求（获取歌曲 URL / 时长 / 推荐歌曲） |
| cJSON | JSON 解析与序列化（源码已内置） |
| pthread (mutex) | 线程安全互斥锁 |
| CMake | 构建系统（要求 ≥ 3.28） |

## 目录结构

```
websocket_wanggou/
├── CMakeLists.txt              # CMake 构建配置
├── include/
│   ├── types.h                 # 核心数据结构（房间 / 客户端 / 播放列表 / 点赞）与动作枚举
│   ├── websocket_service.h     # WebSocket 服务接口
│   ├── rooms.h                 # 房间管理接口
│   ├── playlist.h              # 播放列表管理接口（含数量上限常量）
│   ├── monitor_events.h        # 监控事件接口
│   ├── utf8.h                  # UTF-8 安全拷贝工具（不会截断在多字节字符中间）
│   └── cJSON.h                 # cJSON 库头文件
├── src/
│   ├── websocket_service.c     # 主程序：WebSocket 回调、消息路由、广播、房间推荐调度
│   ├── rooms.c                 # 房间链表管理、操作历史记录
│   ├── playlist.c              # 播放列表操作、歌曲推荐、点赞、播放信息
│   ├── monitor_events.c        # 监控事件（JSONL）实现
│   └── cJSON.c                 # cJSON 库实现
└── LICENSE                     # MIT 许可证
```

## 依赖安装

### macOS

```bash
brew install cmake libwebsockets curl
```

### Ubuntu / Debian

```bash
sudo apt install cmake libwebsockets-dev libcurl4-openssl-dev gcc make
```

> **注意**：需要 libwebsockets 4.x（线上部署版本为 4.3.5）。服务器会通过 HTTP 请求 `http://127.0.0.1:3000` 获取歌曲播放 URL、时长和推荐歌曲（`SERVICE_BASE_URL`，定义在 `src/playlist.c`），需要在该地址部署音乐数据 API 服务（如 `kugoumusicapi`）。

## 编译构建

```bash
# 创建构建目录
cmake -B build

# 编译
cmake --build build
```

编译完成后，可执行文件位于 `build/bin/websocket_service`。

构建产物（`build/`、`CMakeFiles/`、`bin/websocket_service` 等）已被 `.gitignore` 忽略，也可以直接在仓库根目录原地构建：

```bash
cmake . && cmake --build . -j4      # 产物在 bin/websocket_service
```

## 运行

```bash
# 使用默认端口 3001 启动
./build/bin/websocket_service

# 指定端口启动
./build/bin/websocket_service -p 8080

# 以守护进程模式运行，并输出日志到文件
./build/bin/websocket_service -d -l /var/log/websocket_service.log

# 查看帮助
./build/bin/websocket_service -h
```

**命令行参数**

| 参数 | 说明 |
|------|------|
| `-p <端口>` | 监听端口（默认 `3001`） |
| `-l <日志文件>` | 日志输出到指定文件 |
| `-d` | 以守护进程模式运行 |
| `-h` | 显示帮助信息 |

**环境变量**

| 变量 | 说明 |
|------|------|
| `MONITOR_EVENTS_FILE` | 监控事件输出文件（默认 `/app/monitor-events/ws.jsonl`） |

服务默认只监听 `127.0.0.1`，对外请通过 Nginx 等反向代理暴露（见下文）。线上以 Docker 容器运行：仓库目录挂载为 `/app`，容器工作目录 `/app/bin`，启动命令 `./websocket_service`；更新版本时在宿主机重新编译后重启容器即可。

## 连接方式

```
ws://<host>:<port>/ws?fm_ready=1&roomid=<房间ID>&userid=<用户ID>&nickname=<昵称>&avatar=<头像URL>
```

**参数说明**

| 参数 | 必填 | 说明 |
|------|:----:|------|
| `roomid` | ✅ | 房间 ID，不存在时自动创建 |
| `userid` | ✅ | 用户 ID |
| `nickname` | ❌ | 昵称 |
| `avatar` | ❌ | 头像 URL |
| `fm_ready` | ❌ | 固定传 `1`，表示该客户端已登录、可以代拉私人 FM；只有带此标记的客户端会收到房间 FM 推荐请求（216） |

- 路径不限（反代场景通常用 `/ws`），线上地址为 `wss://ws.special520.com/ws`。
- 新接入的客户端建议把 `fm_ready` 放在 query 最前面：昵称含中文、头像 URL 很长时，放在末尾可能解析失败。

## HTTP 接口

```
GET /rooms
```

返回当前活跃房间列表（带 `Access-Control-Allow-Origin: *`，可被浏览器直接请求；其它路径返回 404）：

```json
[
  {
    "room_id": "123456",
    "member_count": 2,
    "current_song": "晴天",
    "singername": "周杰伦",
    "cover_url": "https://example.com/cover.jpg"
  }
]
```

## 通信协议

所有消息均为 JSON 文本帧。

### 消息格式

客户端 → 服务端时，`userid` 必须放在顶层，业务参数统一放在 `params` 对象里，否则服务端会返回 `userid错误！` 或 `参数错误`：

```json
{ "userid": "user123", "action": 208, "params": {} }
```

### 客户端 → 服务端

| action | 名称 | 说明 | params |
|--------|------|------|--------|
| 200 | GET_CUR_SONG_INFO | 获取当前播放歌曲信息 | — |
| 201 | PLAY_NEXT_SONG | 切换到下一首 | — |
| 202 | PLAY_BY_SONG_HASH | 播放列表中指定 hash 的歌曲 | `songhash` |
| 203 | PAUSE_SONG | 暂停 | — |
| 204 | RESUME_SONG | 恢复播放 | — |
| 205 | ADD_SONG_TO_PLAYLIST | 添加单曲 | `songname` `songhash` `singername` `albumname` `duration` `coverurl` |
| 206 | REMOVE_SONG_FROM_PLAYLIST | 从播放列表移除 | `songhash` |
| 207 | UP_SONGBYHASH | 置顶歌曲 | `songhash` |
| 208 | GET_PLAYLIST | 获取播放列表 | — |
| 212 | GET_CLIENT_LIST | 获取房间内客户端列表 | — |
| 214 | ADD_SONGS_TO_PLAYLIST | 批量加歌（单次最多 500 首，整批只记一条操作日志） | `songs`：数组，每项字段同 205 |
| 215 | LIKE_SONG | 给歌曲点赞 / 取消点赞（toggle） | `songhash` |
| 217 | SUBMIT_FM_RECOMMEND | 回应 216：回传私人 FM 候选歌曲 | `request_id` `failed` `songs` |
| 300 | SEND_CHAT | 发送聊天消息 | `message`（长度上限 500，按字节计） |

房间内「未播放歌曲」最多 300 首，超出部分会被丢弃，加歌结果会通过操作记录提示。

**心跳**

```json
{ "type": "heartbeat" }
```

**205 添加歌曲**

```json
{"userid":"user123","action":205,"params":{"songname":"晴天","songhash":"abc123def","singername":"周杰伦","albumname":"叶惠美","duration":"04:30","coverurl":"https://example.com/cover.jpg"}}
```

**215 点赞**

```json
{"userid":"user123","action":215,"params":{"songhash":"abc123def"}}
```

**217 回传 FM 候选**（`songs` 每项的字段名与 205 不同，带下划线）

```json
{
  "userid": "user123",
  "action": 217,
  "params": {
    "request_id": "123456-1732458000-1",
    "failed": false,
    "songs": [
      {
        "songname": "晴天",
        "songhash": "abc123def",
        "singername": "周杰伦",
        "album_name": "叶惠美",
        "duration": "04:30",
        "cover_url": "https://example.com/cover.jpg",
        "mxid": "12345678"
      }
    ]
  }
}
```

客户端拉取失败时回 `"failed": true`（可不带 `songs`），服务端会立即换下一位成员。

### 服务端 → 客户端

| action | 名称 | 说明 |
|--------|------|------|
| 200 | GET_CUR_SONG_INFO | 返回当前播放歌曲信息，字段见下方 `song_info` |
| 209 | BROADCAST_SONG_INFO | 广播当前歌曲信息（切歌 / 暂停 / 恢复） |
| 210 | BROADCAST_SONG_LIST | 广播播放列表（加歌 / 删歌 / 置顶 / 点赞后） |
| 211 | BROADCAST_CLIENT_LIST | 广播房间内客户端列表（成员加入 / 离开时） |
| 212 | GET_CLIENT_LIST | 应答 212：返回房间内客户端列表 |
| 213 | BROADCAST_SONG_PROGRESS | 广播播放进度 |
| 216 | REQUEST_FM_RECOMMEND | 请求客户端代拉私人 FM（只发给声明了 `fm_ready=1` 的客户端） |
| 301 | BROADCAST_CHAT | 广播聊天消息 |
| 302 | BROADCAST_ROOM_ACTION | 广播房间操作记录 |

普通应答的格式为 `{"error_code":0,"status":"success","message":"...","action":205}`。

**合并下发**：一次变更的多份内容可能合并在同一条 JSON 里下发，键名分别为 `playlist`、`song_info`、`actions`，客户端按键取用即可。

**213 播放进度**

```json
{"error_code":0,"status":"success","action":213,"data":{"songhash":"abc123def","played_percent":0.35,"is_playing":1}}
```

**210 播放列表**

```json
{
  "error_code": 0,
  "status": "success",
  "action": 210,
  "playlist": [
    {
      "songname": "晴天",
      "songhash": "abc123def",
      "singername": "周杰伦",
      "album_name": "叶惠美",
      "duration": "04:30",
      "mxid": "12345678",
      "cover_url": "https://example.com/cover.jpg",
      "added_by_nickname": "user123",
      "added_by_avatar": "https://example.com/avatar.png",
      "recommend_source": "",
      "like_count": 2,
      "liked_by": ["user123", "user456"]
    }
  ]
}
```

**216 FM 推荐请求**

```json
{"action":216,"request_id":"123456-1732458000-1","timeout_ms":3000,"want":5}
```

客户端需在 `timeout_ms` 内用 217 回应，最多回传 `want` 首候选歌曲。

**301 聊天消息**（`time` 为 Unix 秒级时间戳）

```json
{"action":301,"status":"success","data":{"userid":"user123","nickname":"小明","avatar_url":"https://example.com/avatar.png","message":"这首歌不错","time":1732458000}}
```

**`song_info` / `playlist[]` 公共字段**

| 字段 | 说明 |
|------|------|
| `songname` `songhash` `singername` `album_name` | 歌曲信息 |
| `duration` | 时长，`mm:ss` 或纯秒数 |
| `mxid` | 用于获取评论的 album_audio_id |
| `cover_url` | 封面 URL |
| `added_by_nickname` | 添加人昵称；成员私人 FM 推荐时为该成员昵称 |
| `recommend_source` | 推荐来源，取值见下表 |

`song_info` 另有 `song_url`、`played_percent`、`is_playing`；`playlist[]` 另有 `added_by_avatar`、`like_count`、`liked_by`（点赞用户 ID 数组）。

| `recommend_source` | 含义 |
|--------------------|------|
| `""` | 成员手动点歌 |
| `热歌榜` / `每日风格推荐` / `每日推荐` / `AI 推荐` / `私人FM` | 系统推荐（兜底算法），`added_by_nickname` 为 `系统推荐` |
| `私人FM` + 成员昵称 | 按该成员的私人 FM 风格推荐 |

## 架构概述

```
┌──────────────────────────────────────────────────┐
│                  WebSocket 服务器                  │
│              (libwebsockets 事件循环)              │
│                                                   │
│  ┌─────────┐   ┌─────────┐   ┌─────────┐         │
│  │ 房间 A   │   │ 房间 B   │   │ 房间 C   │  ...   │
│  │┌───────┐│   │┌───────┐│   │┌───────┐│         │
│  ││客户端链││   ││客户端链││   ││客户端链││         │
│  │├───────┤│   │├───────┤│   │├───────┤│         │
│  ││播放列表││   ││播放列表││   ││播放列表││         │
│  │├───────┤│   │├───────┤│   │├───────┤│         │
│  ││播放信息││   ││播放信息││   ││播放信息││         │
│  ││+定时器 ││   ││+定时器 ││   ││+定时器 ││         │
│  │├───────┤│   │├───────┤│   │├───────┤│         │
│  ││操作历史││   ││操作历史││   ││操作历史││         │
│  │└───────┘│   │└───────┘│   │└───────┘│         │
│  └─────────┘   └─────────┘   └─────────┘         │
│                        │                          │
│             ┌──────────┴──────────┐               │
│             │  libcurl HTTP 请求   │               │
│             │  (歌曲URL / 推荐)    │               │
│             └──────────┬──────────┘               │
└────────────────────────┼──────────────────────────┘
                          │
                          ▼
               ┌─────────────────────┐
               │  音乐数据 API 服务   │
               │  (127.0.0.1:3000)   │
               └─────────────────────┘
```

- **房间**：每个房间是一条独立的链表节点，维护自己的客户端链表、播放列表、当前播放信息、点赞记录、操作历史和定时器，互不影响。
- **广播**：每个客户端有一个发送队列（最多 8 条）；房间级消息只保存一份最新内容，按广播版本号去重后下发。
- **进度同步**：由服务端的定时器统一推进播放进度，客户端只负责同步到该进度。

### 播放进度同步机制

1. 客户端发送 `203` 暂停后，服务端停止进度累计，广播 `209` 通知所有客户端暂停
2. 客户端发送 `204` 恢复后，服务端继续进度累计，广播 `209` 通知所有客户端恢复
3. 进度达到 100% 时，服务端自动切换到下一首；没有下一首时自动触发房间推荐

### 房间推荐机制

房间里的歌播完后服务端会自动补歌，整体流程不会阻塞事件循环：

1. **触发时机** — 建房 / 进房时房间里没有歌；当前歌曲播完且没有下一首；队列剩余不足 3 首时提前预取。
2. **成员私人 FM（优先）** — 服务端随机挑一位握手时声明了 `fm_ready=1` 且本轮还没问过的成员，发送 `216`（带 `request_id`），随即返回事件循环；该成员的客户端用**自己的账号**拉取私人 FM，通过 `217` 回传最多 5 首候选。服务端只收到歌曲元数据，**不接触任何 Token / Cookie**。
3. **预取不插队** — 预取到的推荐歌曲先放在「待播位」：不进播放列表、不广播、不计入队列。期间有人点歌它就继续等，直到当前歌曲播完且没有下一首时，才插入并成为当前歌曲，同时写入操作记录「根据「某成员」风格自动推荐」。
4. **异常处理** — 某位客户端 3 秒内没回应或回应失败，自动换下一位，最多问 3 位；全部失败或房间里没有新客户端，回退系统推荐。预取失败时若房间还有歌在播，冷却 10 秒后再试；房间已经没歌则立即回退，不会出现没人播的空档。
5. **系统推荐（兜底）** — 随机从热歌榜 / 每日风格推荐 / 每日推荐 / AI 推荐 / 私人 FM 拉取候选，排除最近推荐过的 50 首和列表里已有的歌。

### 空闲回收

以房间内最后一次「成员添加歌曲」的时间计时（系统推荐不刷新计时）。超过 3 小时没有成员加歌，服务端会广播：

```json
{"error_code":1,"status":"room_closed","message":"房间已关闭：3 小时无人添加歌曲"}
```

随后 1 秒断开该房间的全部连接并回收房间。

### 监控事件

`monitor_events.c` 会把以下事件按 JSONL 追加写入 `MONITOR_EVENTS_FILE`：

- 房间：创建、加入、离开、销毁
- 播放：切歌（自动 / 手动 / 指定播放）、加歌、删歌、置顶、暂停 / 恢复、进度快照
- 聊天与消息收发摘要

单文件 50MB 轮转为 `.1`；进度类事件有节流（进度 ≥ 10 秒、快照 ≥ 60 秒）。它只是旁路记录：文件打不开或写入失败时静默停用，不影响业务。

### 兼容性说明

- 新动作 `214` ~ `217` 追加在枚举末尾，老客户端不会发送；`216` 只发给声明了 `fm_ready=1` 的客户端，老客户端收不到。
- 新增字段（`mxid`、`recommend_source`、`like_count`、`liked_by` 等）对老客户端是多余字段，可直接忽略。
- 房间里没有任何成员使用新客户端时，直接走系统推荐，不会等待。

## Nginx 反向代理

```nginx
server {
    listen 80;
    server_name your-domain.com;

    location /ws {
        proxy_pass http://127.0.0.1:3001;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    }
}
```

## 许可证

本项目基于 [MIT License](LICENSE) 开源 © 2025 无聊长了蘑菇

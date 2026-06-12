# websocket_wanggou

基于 C 语言 + libwebsockets 的「一起听」音乐房间 WebSocket 服务器。支持多房间、实时播放同步、播放列表协作管理、房间聊天及系统智能推荐等功能，为 WangGouMusic 客户端提供实时同步服务。

## 功能特性

- **多房间管理** — 客户端携带 `roomid` 连接，房间不存在时自动创建；房间内最后一个用户离开后自动销毁并释放资源
- **播放列表协作** — 房间内任意成员可添加、删除、置顶歌曲，操作实时广播给所有人
- **实时播放同步** — 服务端通过定时器跟踪播放进度，周期性广播进度百分比，所有客户端进度保持一致
- **系统智能推荐** — 播放列表为空或歌曲播完时，自动从多源（热歌榜、每日风格推荐、每日推荐、AI 推荐、私人 FM）随机拉取并推荐歌曲，附带去重机制
- **房间聊天** — 支持房间内实时文字聊天，消息长度上限 500 字
- **操作历史** — 记录房间内所有操作（加入/离开/切歌/聊天等），新成员加入时可获取历史记录
- **HTTP 房间列表** — 提供 `GET /rooms` 接口查询当前所有活跃房间的信息
- **反代支持** — 自动解析 `X-Real-IP` / `X-Forwarded-For` 头，适配 Nginx 反向代理部署
- **守护进程** — 支持以 daemon 模式后台运行，支持日志文件输出

## 技术栈

| 组件 | 说明 |
|------|------|
| C (C99) | 核心开发语言 |
| libwebsockets | WebSocket 服务框架，单线程事件循环 |
| libcurl | HTTP 请求（获取歌曲 URL、推荐歌曲） |
| cJSON | JSON 解析与序列化（已内置） |
| pthread (mutex) | 线程安全互斥锁 |
| CMake | 构建系统（要求 ≥ 3.28） |

## 目录结构

```
websocket_wanggou/
├── CMakeLists.txt          # CMake 构建配置
├── Makefile                # CMake 生成的 Makefile（可直接 make）
├── include/
│   ├── types.h             # 核心数据结构定义（房间/客户端/播放列表/操作枚举）
│   ├── websocket_service.h # WebSocket 服务接口
│   ├── rooms.h             # 房间管理接口
│   ├── playlist.h          # 播放列表管理接口
│   └── cJSON.h             # cJSON 库头文件
├── src/
│   ├── websocket_service.c # 主程序：WebSocket 回调、消息路由、广播机制
│   ├── rooms.c             # 房间链表 CRUD、操作历史记录
│   ├── playlist.c          # 播放列表操作、歌曲推荐、进度同步
│   └── cJSON.c             # cJSON 库实现
└── LICENSE                 # MIT 许可证
```

## 依赖安装

**macOS (Homebrew):**

```bash
brew install cmake libwebsockets curl
```

**Ubuntu / Debian:**

```bash
sudo apt install cmake libwebsockets-dev libcurl4-openssl-dev gcc make
```

> 此外，服务器会通过 HTTP 请求 `http://127.0.0.1:3000` 获取歌曲播放 URL 和推荐歌曲，需在该地址部署音乐数据 API 服务（如项目中的 `kugoumusicapi`）。

## 编译构建

```bash
cd websocket_wanggou
cmake -B build
cmake --build build
```

编译产物位于 `build/bin/websocket_service`。

## 运行

```bash
# 默认端口 3001 运行
./build/bin/websocket_service

# 指定端口
./build/bin/websocket_service -p 8080

# 后台守护进程模式 + 日志文件
./build/bin/websocket_service -d -l /var/log/websocket_service.log

# 查看帮助
./build/bin/websocket_service -h
```

### 命令行参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-p <端口>` | WebSocket 监听端口 | `3001` |
| `-l <文件>` | 日志输出文件路径 | 仅 stderr |
| `-d` | 以守护进程模式运行 | 否 |
| `-h` | 显示帮助信息 | — |

## 连接方式

### WebSocket 连接

```
ws://<host>:<port>/?roomid=<房间ID>&userid=<用户ID>&nickname=<昵称>&avatar=<头像URL>
```

`roomid` 和 `userid` 为必填参数，`nickname` 和 `avatar` 为可选。

### HTTP 接口

```
GET http://<host>:<port>/rooms
```

返回所有活跃房间的列表（房间ID、成员数、当前播放歌曲信息）。

## 通信协议

所有消息均为 JSON 格式，通过 `action` 字段区分操作类型。

### 客户端 → 服务端

| action 值 | 名称 | 说明 | params 字段 |
|-----------|------|------|-------------|
| 200 | `GET_CUR_SONG_INFO` | 获取当前播放歌曲信息 | — |
| 201 | `PLAY_NEXT_SONG` | 播放下一首 | — |
| 202 | `PLAY_BY_SONG_HASH` | 指定歌曲播放 | `songhash` |
| 203 | `PAUSE_SONG` | 暂停播放 | — |
| 204 | `RESUME_SONG` | 继续播放 | — |
| 205 | `ADD_SONG_TO_PLAYLIST` | 添加歌曲到播放列表 | `songname, songhash, singername, albumname, duration, coverurl` |
| 206 | `REMOVE_SONG_FROM_PLAYLIST` | 从播放列表删除歌曲 | `songhash` |
| 207 | `UP_SONGBYHASH` | 置顶歌曲（插到当前播放之后） | `songhash` |
| 208 | `GET_PLAYLIST` | 获取播放列表 | — |
| 211 | `GET_CLIENT_LIST` | 获取房间内客户端列表 | — |
| 300 | `SEND_CHAT` | 发送聊天消息 | `message`（≤500字） |

此外，发送 `{"type": "heartbeat"}` 可触发心跳响应。

**请求示例：**

```json
{
  "userid": "user123",
  "action": 205,
  "params": {
    "songname": "晴天",
    "songhash": "abc123def",
    "singername": "周杰伦",
    "albumname": "叶惠美",
    "duration": "04:30",
    "coverurl": "https://example.com/cover.jpg"
  }
}
```

### 服务端 → 客户端

服务端消息同样通过 `action` 字段标识，主要广播类型：

| action 值 | 说明 |
|-----------|------|
| 200 | 当前歌曲信息 |
| 209 | 广播歌曲信息变更（切歌/暂停/恢复） |
| 210 | 广播播放列表变更 |
| 211 | 广播客户端列表变更 |
| 213 | 广播播放进度同步（周期性） |
| 301 | 广播聊天消息 |
| 302 | 广播房间操作记录 |

**响应示例（播放进度同步）：**

```json
{
  "error_code": 0,
  "status": "success",
  "action": 213,
  "data": {
    "songhash": "abc123def",
    "played_percent": 0.35,
    "is_playing": 1
  }
}
```

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

### 播放进度同步机制

服务端不直接播放音频，而是通过定时器（`lws_sul`）模拟播放进度：

- **播放中** — 每 5 秒广播一次进度，进度 ≥95% 时缩短为每 0.5 秒
- **暂停时** — 每 15 秒检查一次（不更新进度）
- **播放完成** — 进度 ≥100% 时自动切到下一首，播放列表为空则触发系统推荐

## Nginx 反向代理配置（可选）

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

[MIT License](LICENSE) © 2025 无聊长了蘑菇

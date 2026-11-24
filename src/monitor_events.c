#include "monitor_events.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <libwebsockets.h>

// ── 事件文件 ──
// 默认路径对应容器内挂载点（working_dir=/app/bin，仓库整体挂载在 /app），
// 宿主机侧即 ~/websocket_wanggou/monitor-events/ws.jsonl
#define MON_DEFAULT_FILE "/app/monitor-events/ws.jsonl"
#define MON_ROTATE_SIZE (50LL * 1024 * 1024) // 超过 50MB 轮转为 .1
#define MON_PROGRESS_INTERVAL 10             // progress 事件最小间隔（秒）
#define MON_SNAPSHOT_INTERVAL 60             // 房间全量快照间隔（秒）
static FILE *mon_file = NULL;
static int mon_disabled = 0;
static pthread_mutex_t mon_lock = PTHREAD_MUTEX_INITIALIZER;
static char mon_path[512] = {0};
static long long mon_size = 0;

// 每房间节流状态（progress ≥10s、snapshot ≥60s 一条；房间数极少，静态表足够）
typedef struct
{
    rooms_t *room;
    time_t last_progress;
    time_t last_snapshot;
    time_t last_io_progress; // 交互流里的进度广播摘要 ≥30s 一条
} mon_room_state_t;
#define MON_MAX_ROOMS 64
static mon_room_state_t mon_states[MON_MAX_ROOMS];

static long long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void mon_open_locked(void)
{
    mon_file = fopen(mon_path, "a");
    if (mon_file)
    {
        fseek(mon_file, 0, SEEK_END);
        mon_size = ftell(mon_file);
    }
    else
    {
        mon_disabled = 1;
    }
}

void monitor_events_init(void)
{
    const char *p = getenv("MONITOR_EVENTS_FILE");
    if (!p || !strlen(p))
        p = MON_DEFAULT_FILE;
    strncpy(mon_path, p, sizeof(mon_path) - 1);
    // 逐级创建目录（等价 mkdir -p，只对路径前缀做）
    char tmp[512];
    strncpy(tmp, mon_path, sizeof(tmp) - 1);
    for (char *slash = strchr(tmp + 1, '/'); slash; slash = strchr(slash + 1, '/'))
    {
        *slash = '\0';
        mkdir(tmp, 0755); // 已存在返回 EEXIST，忽略
        *slash = '/';
    }
    pthread_mutex_lock(&mon_lock);
    mon_open_locked();
    pthread_mutex_unlock(&mon_lock);
}

// 核心写出：root 为完整事件对象，由本函数释放；写失败后永久禁用（静默）
static void mon_write_locked(cJSON *root)
{
    if (mon_disabled)
    {
        cJSON_Delete(root);
        return;
    }
    if (!mon_file)
        mon_open_locked();
    if (!mon_file)
    {
        cJSON_Delete(root);
        return;
    }
    char *line = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!line)
        return;
    size_t len = strlen(line);
    if (fwrite(line, 1, len, mon_file) == len && fputc('\n', mon_file) != EOF)
    {
        fflush(mon_file);
        mon_size += (long long)len + 1;
    }
    free(line);
    // 简单轮转：超限后关闭并改名 .1，下一次写时自动重开新文件
    if (mon_size > MON_ROTATE_SIZE)
    {
        fclose(mon_file);
        mon_file = NULL;
        char old_path[600];
        snprintf(old_path, sizeof(old_path), "%s.1", mon_path);
        rename(mon_path, old_path);
    }
}

// data 会被并入事件并释放
void monitor_emit(const char *type, const char *room_id, cJSON *data)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
    {
        cJSON_Delete(data);
        return;
    }
    cJSON_AddNumberToObject(root, "ts", (double)now_ms());
    cJSON_AddStringToObject(root, "type", type);
    if (room_id)
        cJSON_AddStringToObject(root, "room", room_id);
    if (data)
        cJSON_AddItemToObject(root, "data", data);
    pthread_mutex_lock(&mon_lock);
    mon_write_locked(root);
    pthread_mutex_unlock(&mon_lock);
}

// ── 小工具：填充通用字段 ──
static cJSON *mon_user_obj(const client_info_t *c)
{
    cJSON *u = cJSON_CreateObject();
    cJSON_AddStringToObject(u, "userid", c ? c->userId : "");
    cJSON_AddStringToObject(u, "nickname", c ? c->nickname : "");
    cJSON_AddStringToObject(u, "avatar_url", c ? c->avatar_url : "");
    cJSON_AddStringToObject(u, "ip", c ? c->ip : "");
    return u;
}

static cJSON *mon_song_obj(const playlist_t *s)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "songname", s ? s->song_name : "");
    cJSON_AddStringToObject(o, "songhash", s ? s->song_hash : "");
    cJSON_AddStringToObject(o, "singername", s ? s->singer_name : "");
    cJSON_AddStringToObject(o, "albumname", s ? s->album_name : "");
    cJSON_AddStringToObject(o, "duration", s ? s->duration : "");
    cJSON_AddStringToObject(o, "cover_url", s ? s->cover_url : "");
    cJSON_AddStringToObject(o, "mxid", s ? s->mxid : "");
    cJSON_AddBoolToObject(o, "is_system", s ? s->is_system : 0);
    return o;
}

void monitor_emit_room_create(const rooms_t *room, const char *userid, const char *nickname, const char *ip)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "creater_id", room ? room->creater_id : (userid ? userid : ""));
    cJSON_AddStringToObject(d, "creater_nickname", nickname ? nickname : "");
    cJSON_AddStringToObject(d, "ip", ip ? ip : "");
    monitor_emit("room_create", room ? room->room_id : NULL, d);
}

void monitor_emit_join(const char *room_id, const client_info_t *client)
{
    monitor_emit("join", room_id, mon_user_obj(client));
    if (client && client->room)
        monitor_tick_room(client->room); // 立即补一张最新快照（把该成员算进去）
}

void monitor_emit_leave(const rooms_t *room, const client_info_t *client)
{
    monitor_emit("leave", room ? room->room_id : NULL, mon_user_obj(client));
    if (room)
        monitor_tick_room(room); // 快照里已是移除后的成员列表
}

void monitor_emit_room_destroy(const char *room_id)
{
    monitor_emit("room_destroy", room_id, NULL);
}

void monitor_forget_room(const rooms_t *room)
{
    for (int i = 0; i < MON_MAX_ROOMS; i++)
    {
        if (mon_states[i].room == room)
        {
            memset(&mon_states[i], 0, sizeof(mon_states[i]));
            break;
        }
    }
}

void monitor_emit_song_change(const rooms_t *room, const char *cause, const client_info_t *by)
{
    if (!room)
        return;
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "cause", cause ? cause : "");
    cJSON_AddItemToObject(d, "by", mon_user_obj(by));
    cJSON_AddItemToObject(d, "song", mon_song_obj(room->current_song));
    // 附带最新播放列表：系统自动切歌会改变列表，监控端无需等 60s 快照
    cJSON *playlist = cJSON_CreateArray();
    int count = 0;
    for (playlist_t *s = room->playlist_head ? room->playlist_head->next : NULL; s && count < 200; s = s->next, count++)
        cJSON_AddItemToArray(playlist, mon_song_obj(s));
    cJSON_AddItemToObject(d, "playlist", playlist);
    monitor_emit("song_change", room->room_id, d);
}

void monitor_emit_song_add(const client_info_t *client, const char *songname, const char *songhash,
                           const char *singername, const char *albumname, const char *duration,
                           const char *coverurl)
{
    if (!client)
        return;
    playlist_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    strncpy(tmp.song_name, songname ? songname : "", sizeof(tmp.song_name) - 1);
    strncpy(tmp.song_hash, songhash ? songhash : "", sizeof(tmp.song_hash) - 1);
    strncpy(tmp.singer_name, singername ? singername : "", sizeof(tmp.singer_name) - 1);
    strncpy(tmp.album_name, albumname ? albumname : "", sizeof(tmp.album_name) - 1);
    strncpy(tmp.duration, duration ? duration : "", sizeof(tmp.duration) - 1);
    strncpy(tmp.cover_url, coverurl ? coverurl : "", sizeof(tmp.cover_url) - 1);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddItemToObject(d, "by", mon_user_obj(client));
    cJSON_AddItemToObject(d, "song", mon_song_obj(&tmp));
    monitor_emit("song_add", client->room ? client->room->room_id : NULL, d);
}

void monitor_emit_song_op(const client_info_t *client, const char *song_op_type, const char *songhash)
{
    if (!client)
        return;
    cJSON *d = cJSON_CreateObject();
    cJSON_AddItemToObject(d, "by", mon_user_obj(client));
    cJSON_AddStringToObject(d, "songhash", songhash ? songhash : "");
    monitor_emit(song_op_type, client->room ? client->room->room_id : NULL, d);
}

void monitor_emit_play_state(const client_info_t *client, int is_playing)
{
    if (!client)
        return;
    cJSON *d = cJSON_CreateObject();
    cJSON_AddItemToObject(d, "by", mon_user_obj(client));
    cJSON_AddBoolToObject(d, "is_playing", is_playing);
    monitor_emit("play_state", client->room ? client->room->room_id : NULL, d);
}

void monitor_emit_chat(const client_info_t *client, const char *message)
{
    if (!client)
        return;
    cJSON *d = mon_user_obj(client);
    cJSON_AddStringToObject(d, "message", message ? message : "");
    monitor_emit("chat", client->room ? client->room->room_id : NULL, d);
}

// 每房间找/占一个节流槽位
static mon_room_state_t *mon_state_of(rooms_t *room)
{
    mon_room_state_t *free_slot = NULL;
    for (int i = 0; i < MON_MAX_ROOMS; i++)
    {
        if (mon_states[i].room == room)
            return &mon_states[i];
        if (!mon_states[i].room && !free_slot)
            free_slot = &mon_states[i];
    }
    if (free_slot)
    {
        free_slot->room = room;
        return free_slot;
    }
    return NULL; // 表满：退化为全量上报
}

// 读当前播放状态（playing_info 加锁短暂读取，避免撕裂）
static void mon_read_playing(rooms_t *room, playlist_t *song_out, double *percent, int *is_playing)
{
    memset(song_out, 0, sizeof(*song_out));
    *percent = 0;
    *is_playing = 0;
    if (!room->current_song)
        return;
    *song_out = *room->current_song;
    pthread_mutex_lock(&room->playing_info.lock);
    *percent = room->playing_info.played_percent;
    *is_playing = room->playing_info.is_playing;
    pthread_mutex_unlock(&room->playing_info.lock);
}

void monitor_tick_room(rooms_t *room)
{
    if (!room)
        return;
    time_t now = time(NULL);
    mon_room_state_t *st = mon_state_of(room);
    time_t since_progress = st ? now - st->last_progress : MON_PROGRESS_INTERVAL + 1;
    time_t since_snapshot = st ? now - st->last_snapshot : MON_SNAPSHOT_INTERVAL + 1;
    int want_progress = since_progress > MON_PROGRESS_INTERVAL;
    int want_snapshot = since_snapshot > MON_SNAPSHOT_INTERVAL;
    if (!want_progress && !want_snapshot)
        return;

    playlist_t cur;
    double percent = 0;
    int is_playing = 0;
    mon_read_playing(room, &cur, &percent, &is_playing);

    if (want_progress && room->current_song)
    {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddItemToObject(d, "song", mon_song_obj(&cur));
        cJSON_AddNumberToObject(d, "percent", percent);
        cJSON_AddBoolToObject(d, "is_playing", is_playing);
        monitor_emit("progress", room->room_id, d);
        if (st)
            st->last_progress = now;
    }
    if (want_snapshot)
    {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "member_count", room->client_counter);
        cJSON *members = cJSON_CreateArray();
        for (client_info_t *c = room->client_info ? room->client_info->next : NULL; c; c = c->next)
            cJSON_AddItemToArray(members, mon_user_obj(c));
        cJSON_AddItemToObject(d, "members", members);
        cJSON *playlist = cJSON_CreateArray();
        int count = 0;
        for (playlist_t *s = room->playlist_head ? room->playlist_head->next : NULL; s && count < 200; s = s->next, count++)
            cJSON_AddItemToArray(playlist, mon_song_obj(s));
        cJSON_AddItemToObject(d, "playlist", playlist);
        if (room->current_song)
        {
            cJSON_AddItemToObject(d, "current", mon_song_obj(&cur));
            cJSON_AddNumberToObject(d, "percent", percent);
            cJSON_AddBoolToObject(d, "is_playing", is_playing);
        }
        monitor_emit("snapshot", room->room_id, d);
        if (st)
            st->last_snapshot = now;
    }
}

// ── WebSocket 交互流：把协议消息翻译成人类可读摘要 ──

// 消息种类（决定是否节流）
typedef enum
{
    MON_MSG_OTHER = 0,
    MON_MSG_PROGRESS, // 播放进度广播（高频）
    MON_MSG_HEARTBEAT // 心跳回执（直接丢弃）
} mon_msg_kind_t;

// 操作码 → 中文动作名
static const char *mon_action_name(int action)
{
    switch (action)
    {
    case 200: return "获取当前歌曲";
    case 201: return "请求切下一首";
    case 202: return "点播指定歌曲";
    case 203: return "暂停播放";
    case 204: return "恢复播放";
    case 205: return "添加歌曲到播放列表";
    case 206: return "从播放列表移除歌曲";
    case 207: return "置顶歌曲";
    case 208: return "获取播放列表";
    case 212: return "获取成员列表";
    case 300: return "发送聊天消息";
    default:  return "未知操作";
    }
}

// 缩短文本（UTF-8 安全截断，按字节 ≤ limit，避免砍出半个字）
static void mon_brief(char *out, size_t out_size, const char *src, size_t limit)
{
    if (!src)
    {
        out[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len <= limit)
    {
        snprintf(out, out_size, "%s", src);
        return;
    }
    size_t cut = limit;
    while (cut > 0 && ((unsigned char)src[cut] & 0xC0) == 0x80)
        cut--; // 回退 UTF-8 续字节
    snprintf(out, out_size, "%.*s…", (int)cut, src);
}

// 把一条协议消息翻译成摘要（返回 malloc 串，调用方 free；kind 供节流判断）
static char *mon_describe_message(const char *msg, mon_msg_kind_t *kind)
{
    *kind = MON_MSG_OTHER;
    cJSON *root = cJSON_Parse(msg);
    if (!root)
        return strdup("非JSON消息");

    char text[256] = {0};
    char brief[128];
    cJSON *status = cJSON_GetObjectItem(root, "status");
    cJSON *action = cJSON_GetObjectItem(root, "action");
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *actions = cJSON_GetObjectItem(root, "actions");
    const char *status_str = cJSON_IsString(status) ? status->valuestring : NULL;
    int action_no = cJSON_IsNumber(action) ? action->valueint : -1;

    do
    {
        // 房间关闭通知
        if (status_str && !strcmp(status_str, "room_closed"))
        {
            snprintf(text, sizeof(text), "房间已关闭通知");
            break;
        }
        // 心跳回执：不入库
        if (!cJSON_IsNumber(action) && status_str && !strcmp(status_str, "success"))
        {
            cJSON *m = cJSON_GetObjectItem(root, "message");
            if (cJSON_IsString(m) && !strcmp(m->valuestring, "heartbeat"))
            {
                *kind = MON_MSG_HEARTBEAT;
                break;
            }
        }
        // 聊天广播
        cJSON *message_item = data ? cJSON_GetObjectItem(data, "message") : NULL;
        if (action_no == 301 && cJSON_IsString(message_item))
        {
            cJSON *nick = data ? cJSON_GetObjectItem(data, "nickname") : NULL;
            mon_brief(brief, sizeof(brief), cJSON_IsString(nick) ? nick->valuestring : "?", 24);
            char msg_brief[160];
            mon_brief(msg_brief, sizeof(msg_brief), message_item->valuestring, 80);
            snprintf(text, sizeof(text), "聊天 · %s：%s", brief, msg_brief);
            break;
        }
        // 播放进度广播
        cJSON *percent_item = data ? cJSON_GetObjectItem(data, "played_percent") : NULL;
        if (action_no == 213 || (percent_item && cJSON_IsNumber(percent_item)))
        {
            double p = cJSON_IsNumber(percent_item) ? percent_item->valuedouble : 0;
            cJSON *playing = data ? cJSON_GetObjectItem(data, "is_playing") : NULL;
            snprintf(text, sizeof(text), "播放进度 · %.1f%%（%s）", p * 100,
                     (cJSON_IsNumber(playing) ? playing->valueint : 1) ? "播放中" : "已暂停");
            *kind = MON_MSG_PROGRESS;
            break;
        }
        // 成员列表
        cJSON *client_list = cJSON_GetObjectItem(root, "client_list");
        if (cJSON_IsArray(client_list))
        {
            int n = cJSON_GetArraySize(client_list);
            char names[96] = {0};
            int count = 0;
            for (cJSON *c = client_list->child; c && count < 3; c = c->next, count++)
            {
                cJSON *nick = cJSON_GetObjectItem(c, "nickname");
                if (!cJSON_IsString(nick))
                    continue;
                char one[32];
                mon_brief(one, sizeof(one), nick->valuestring, 12);
                strncat(names, strlen(names) ? "、" : "", sizeof(names) - strlen(names) - 1);
                strncat(names, one, sizeof(names) - strlen(names) - 1);
            }
            snprintf(text, sizeof(text), "成员列表 · %d 人%s%s", n, n ? "：" : "", names);
            break;
        }
        // 播放列表
        cJSON *playlist = cJSON_GetObjectItem(root, "playlist");
        if (cJSON_IsArray(playlist))
        {
            snprintf(text, sizeof(text), "播放列表 · %d 首", cJSON_GetArraySize(playlist));
            break;
        }
        // 歌曲（当前歌曲信息 / 点播回执）
        cJSON *songname = data ? cJSON_GetObjectItem(data, "songname") : cJSON_GetObjectItem(root, "songname");
        cJSON *singer = data ? cJSON_GetObjectItem(data, "singername") : cJSON_GetObjectItem(root, "singername");
        if (cJSON_IsString(songname) && strlen(songname->valuestring))
        {
            mon_brief(brief, sizeof(brief), songname->valuestring, 32);
            if (cJSON_IsString(singer) && strlen(singer->valuestring))
                snprintf(text, sizeof(text), "歌曲 · %s - %s", brief, singer->valuestring);
            else
                snprintf(text, sizeof(text), "歌曲 · %s", brief);
            break;
        }
        // 操作回执
        if (status_str)
        {
            cJSON *m = cJSON_GetObjectItem(root, "message");
            mon_brief(brief, sizeof(brief), cJSON_IsString(m) ? m->valuestring : "", 48);
            snprintf(text, sizeof(text), "回执 · %s%s%s",
                     !strcmp(status_str, "success") ? "成功" : "失败",
                     strlen(brief) ? "：" : "", brief);
            break;
        }
        snprintf(text, sizeof(text), "消息");
    } while (0);

    // 合并消息附带了操作历史：追加说明
    if (cJSON_IsArray(actions))
    {
        char tail[48];
        snprintf(tail, sizeof(tail), " ＋操作记录×%d", cJSON_GetArraySize(actions));
        strncat(text, tail, sizeof(text) - strlen(text) - 1);
    }
    cJSON_Delete(root);
    return strdup(text);
}

// 统一的交互事件出口：dir ∈ "in"（客户端→服务器）| "out"（定向下发）| "broadcast"（全员广播）
static void mon_emit_ws_io(const char *dir, const char *room_id, const client_info_t *client, const char *text)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "dir", dir);
    cJSON_AddStringToObject(d, "ip", client ? client->ip : "");
    cJSON_AddStringToObject(d, "userid", client ? client->userId : "");
    cJSON_AddStringToObject(d, "nickname", client ? client->nickname : "");
    cJSON_AddStringToObject(d, "text", text ? text : "");
    monitor_emit("ws_io", room_id, d);
}

void monitor_emit_ws_in(const client_info_t *client, const cJSON *root)
{
    if (!client || !root)
        return;
    cJSON *action = cJSON_GetObjectItem(root, "action");
    cJSON *params = cJSON_GetObjectItem(root, "params");
    int action_no = cJSON_IsNumber(action) ? action->valueint : -1;
    char text[192];
    char brief[96];
    // 附带关键参数：点播/移除/置顶带 hash，聊天带内容
    const char *sep = "";
    brief[0] = '\0';
    if (params && cJSON_IsObject(params))
    {
        cJSON *songhash = cJSON_GetObjectItem(params, "songhash");
        cJSON *message = cJSON_GetObjectItem(params, "message");
        cJSON *songname = cJSON_GetObjectItem(params, "songname");
        cJSON *singername = cJSON_GetObjectItem(params, "singername");
        if (cJSON_IsString(songname) && strlen(songname->valuestring))
        {
            char sb[48];
            mon_brief(sb, sizeof(sb), songname->valuestring, 24);
            strncat(brief, sb, sizeof(brief) - strlen(brief) - 1);
            if (cJSON_IsString(singername) && strlen(singername->valuestring))
            {
                strncat(brief, " - ", sizeof(brief) - strlen(brief) - 1);
                mon_brief(sb, sizeof(sb), singername->valuestring, 16);
                strncat(brief, sb, sizeof(brief) - strlen(brief) - 1);
            }
            sep = "：";
        }
        else if (cJSON_IsString(songhash) && strlen(songhash->valuestring))
        {
            mon_brief(brief, sizeof(brief), songhash->valuestring, 16);
            sep = "：hash ";
        }
        else if (cJSON_IsString(message) && strlen(message->valuestring))
        {
            mon_brief(brief, sizeof(brief), message->valuestring, 64);
            sep = "：";
        }
    }
    snprintf(text, sizeof(text), "%s%s%s", mon_action_name(action_no), sep, brief);
    mon_emit_ws_io("in", client->room ? client->room->room_id : NULL, client, text);
}

void monitor_emit_ws_broadcast(const rooms_t *room, const char *msg)
{
    if (!room || !msg)
        return;
    mon_msg_kind_t kind;
    char *text = mon_describe_message(msg, &kind);
    if (kind == MON_MSG_HEARTBEAT)
    {
        free(text);
        return;
    }
    // 进度广播高频：每房间 ≥30s 记一条
    if (kind == MON_MSG_PROGRESS)
    {
        time_t now = time(NULL);
        mon_room_state_t *st = mon_state_of((rooms_t *)room);
        if (st)
        {
            if (now - st->last_io_progress < 30)
            {
                free(text);
                return;
            }
            st->last_io_progress = now;
        }
    }
    mon_emit_ws_io("broadcast", room->room_id, NULL, text);
    free(text);
}

void monitor_emit_ws_send(const client_info_t *client, const char *msg)
{
    if (!client || !msg)
        return;
    mon_msg_kind_t kind;
    char *text = mon_describe_message(msg, &kind);
    if (kind == MON_MSG_HEARTBEAT)
    {
        free(text);
        return;
    }
    mon_emit_ws_io("out", client->room ? client->room->room_id : NULL, client, text);
    free(text);
}

#include "websocket_service.h"
#include "rooms.h"
#include <sys/socket.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "types.h"
#include <stdbool.h>
#include "playlist.h"
#include "utf8.h"

int callback_echo(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len);
static void success_response(client_info_t *client, const char *msg);
static void error_response(client_info_t *client, const char *msg);
static void send_message_to_client(client_info_t *client, const char *msg);

struct lws_context *context = NULL;
static int interrupted = 0;

rooms_t *g_rooms_list = NULL; // 房间链表

// 定义协议处理结构
static struct lws_protocols protocols[] = {
    {
        .name = "ctrl-protocol", // 协议名称
        .callback = callback_echo,   // 回调函数
        .per_session_data_size = 0,  // 每个连接的用户数据大小
        .rx_buffer_size = 4096,      // 接收缓冲区大小
    },
    {0} // 协议列表结束标记
};

// 头插法插入客户端节点
bool insert_client_node(client_info_t *head, client_info_t *new_node)
{
    if (head == NULL || new_node == NULL)
    {
        return false;
    }
    new_node->next = head->next;
    new_node->prev = head;
    if (head->next != NULL)
    {
        head->next->prev = new_node;
    }
    head->next = new_node;
    return true;
}

// 申请节点并填充客户端信息,返回改节点指针
client_info_t *insert_client_info(struct lws *wsi, const char *ip, rooms_t *room,
                                   const char *userId, const char *nickname, const char *avatar_url)
{
    client_info_t *new_node = (client_info_t *)malloc(sizeof(client_info_t));
    if (!new_node)
    {
        lwsl_err("Failed to allocate memory for client_info_t\n");
        return NULL;
    }
    memset(new_node, 0, sizeof(client_info_t));
    pthread_mutex_init(&new_node->lock, NULL);
    new_node->wsi = wsi;
    strncpy(new_node->ip, ip, INET_ADDRSTRLEN - 1);
    new_node->room = room;
    strncpy(new_node->userId, userId, 63);
    if (nickname) copy_utf8_bounded(new_node->nickname, nickname, sizeof(new_node->nickname));
    if (avatar_url) copy_utf8_bounded(new_node->avatar_url, avatar_url, sizeof(new_node->avatar_url));
    new_node->next = NULL;
    new_node->prev = NULL;
    if (!insert_client_node(room->client_info, new_node))
    {
        lwsl_err("Failed to insert client node into room's client list\n");
        free(new_node);
        return NULL;
    }
    room->client_counter++;
    return new_node;
}

// 对应房间发送广播信息
static void broadcast_response_room(rooms_t *room, const char *msg)
{
    if (!room || !msg)
        return;
    pthread_mutex_lock(&room->lock);
    free(room->latest_msg);
    room->latest_msg = strdup(msg);
    room->broadcast_version++;
    pthread_mutex_unlock(&room->lock);

    // 遍历所有用户
    for (client_info_t *cur = room->client_info->next; cur; cur = cur->next)
    {
        if (cur->wsi)
        {
            lws_callback_on_writable(cur->wsi);
        }
    }
}

// 将数据 JSON 与操作日志 JSON 合并为一条消息（避免连续广播互相覆盖）
static char *combine_json_with_actions(const char *data_json, const char *actions_json_str)
{
    if (!data_json)
        return NULL;

    cJSON *data_root = cJSON_Parse(data_json);
    if (!data_root)
        return NULL;

    if (actions_json_str)
    {
        cJSON *actions_root = cJSON_Parse(actions_json_str);
        if (actions_root)
        {
            cJSON *actions_arr = cJSON_DetachItemFromObject(actions_root, "actions");
            if (actions_arr)
            {
                cJSON_AddItemToObject(data_root, "actions", actions_arr);
            }
            cJSON_Delete(actions_root);
        }
    }

    char *result = cJSON_PrintUnformatted(data_root);
    cJSON_Delete(data_root);
    return result;
}

// 操作回复广播（操作者回复成功与否，其他客户端回复最新数据）
static void operation_response(client_info_t *client, const char *msg)
{
    if (!msg || !client)
        return;

    // 操作客户端回复
    success_response(client, "操作成功");

    pthread_mutex_lock(&client->room->lock);
    free(client->room->latest_msg);
    client->room->latest_msg = strdup(msg);
    client->room->broadcast_version++;
    pthread_mutex_unlock(&client->room->lock);

    // 遍历该房间客户端链表，唤醒对应客户端发送信息（除创建者）
    for (client_info_t *cur = client->room->client_info->next; cur != NULL; cur = cur->next)
    {
        if (cur->wsi && client != cur)
        {
            lws_callback_on_writable(cur->wsi);
        }
    }
}

// 信号处理函数，用于优雅退出（SIGINT / SIGTERM 共用）
static void signal_handler(int sig)
{
    (void)sig;
    interrupted = 1;
}

// 日志文件指针
static FILE *log_file = NULL;

// 自定义日志输出函数（同时输出到 stderr 和日志文件）
static void log_emit_function(int level, const char *line)
{
    (void)level;
    // 输出到 stderr
    fprintf(stderr, "%s", line);

    // 输出到日志文件
    if (log_file)
    {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        char timebuf[32];
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", t);
        fprintf(log_file, "[%s] %s", timebuf, line);
        fflush(log_file);
    }
}

static void print_room_info(rooms_t *room)
{
    if (!room)
        return;
    lwsl_notice("房间ID: %s, 创建者ID: %s, 客户端数量: %u\n", room->room_id, room->creater_id, room->client_counter);
    lwsl_notice("客户端列表:\n");
    for (client_info_t *client = room->client_info->next; client != NULL; client = client->next)
    {
        lwsl_notice("  客户端IP: %s, 用户ID: %s\n", client->ip, client->userId);
    }
}

// 定时更新进度
void timer_callback(lws_sorted_usec_list_t *sul)
{
    float duration = 0;
    playing_info_t *playing_info = lws_container_of(sul, playing_info_t, timer);
    int callback_time = playing_info->is_playing ? 5000 : 15000;
    if (playing_info->is_playing)
    {
        pthread_mutex_lock(&playing_info->lock);
        // 解析 duration：支持 "mm:ss" 和纯秒数两种格式
        {
            char *colon = strchr(playing_info->duration, ':');
            if (colon)
            {
                int min = atoi(playing_info->duration);
                int sec = atoi(colon + 1);
                duration = min * 60 + sec;
            }
            else
            {
                duration = atof(playing_info->duration);
            }
        }
        // 兜底：时长为空/非法（如早期私人FM加的歌没带 duration）时，下面的
        // 除法会除零得 inf，进度瞬间爆表 → 服务器连环切歌、客户端跟着疯狂
        // 切歌（死循环）。按 240s 常规歌曲时长推进，保证至少能正常轮转。
        if (duration <= 0)
        {
            duration = 240;
        }
        time_t now = time(NULL);
        // 更新进度偏移
        double offset = (now - playing_info->last_update_time) / duration;
        playing_info->played_percent += offset;
        playing_info->last_update_time = now;
        // 要是接近播放完成的话，加快广播频率
        if (playing_info->played_percent >= 0.95)
        {
            callback_time = 500;
        }
        pthread_mutex_unlock(&playing_info->lock);
    }
    int song_changed = 0;
    if (playing_info->played_percent >= 1)
    {
        song_changed = 1;
        play_next_song_bysystem(playing_info->room);
    }
    // 有歌曲的时候广播
    if (playing_info->room->current_song)
    {
        if (song_changed)
        {
            // 切歌，广播歌曲信息 + 播放列表（系统推荐会改变列表）
            const char *combined = get_playlist_and_song_info_json(playing_info->room);
            const char *actions_json = get_room_actions_json(playing_info->room, 1);
            char *final = combine_json_with_actions(combined, actions_json);
            broadcast_response_room(playing_info->room, final ? final : combined);
            free(final);
            free((void *)combined);
            free((void *)actions_json);
        }
        else
        {
            // 正常进度同步
            const char *progress_json = get_cur_song_progress(playing_info->room);
            broadcast_response_room(playing_info->room, progress_json);
            free((void *)progress_json);
        }
    }

    lws_sul_schedule(context, 0, sul, timer_callback, callback_time * LWS_US_PER_MS);
}

static int client_callback_established(struct lws *wsi)
{
    lwsl_notice("新的客户端连接建立\n");
    char roomid[64] = {0};
    char userId[64] = {0};
    char nickname[128] = {0};
    char avatar_url[512] = {0};
    char client_ip[64] = {0};
    rooms_t *new_room = NULL;
    client_info_t *new_client = NULL;

    if (g_rooms_list == NULL)
    {
        lwsl_err("房间链表未初始化\n");
        return -1;
    }

    // 优先从 X-Real-IP / X-Forwarded-For 头获取真实客户端 IP（nginx 反向代理场景）
    {
        char hdr_buf[256] = {0};
        // 尝试读取 X-Real-IP
        int n = lws_hdr_copy(wsi, hdr_buf, sizeof(hdr_buf), WSI_TOKEN_HTTP_X_REAL_IP);
        if (n > 0)
        {
            strncpy(client_ip, hdr_buf, sizeof(client_ip) - 1);
            lwsl_notice("从 X-Real-IP 获取真实 IP: %s\n", client_ip);
        }
        else
        {
            // 尝试读取 X-Forwarded-For
            n = lws_hdr_copy(wsi, hdr_buf, sizeof(hdr_buf), WSI_TOKEN_X_FORWARDED_FOR);
            if (n > 0)
            {
                // X-Forwarded-For 可能有多个 IP，取第一个
                char *comma = strchr(hdr_buf, ',');
                if (comma) *comma = '\0';
                strncpy(client_ip, hdr_buf, sizeof(client_ip) - 1);
                lwsl_notice("从 X-Forwarded-For 获取真实 IP: %s\n", client_ip);
            }
        }
    }
    // 如果没有代理头，则直接取 TCP 层 IP
    if (!strlen(client_ip))
    {
        lws_get_peer_simple(wsi, client_ip, sizeof(client_ip));
    }
    if (!strlen(client_ip))
    {
        lwsl_err("无法获取客户端IP，断开连接\n");
        return -1;
    }

    lws_get_urlarg_by_name(wsi, "roomid", roomid, sizeof(roomid));
    lws_get_urlarg_by_name(wsi, "userid", userId, sizeof(userId));
    lws_get_urlarg_by_name(wsi, "nickname", nickname, sizeof(nickname));
    lws_get_urlarg_by_name(wsi, "avatar", avatar_url, sizeof(avatar_url));

    if (!strlen(roomid) || !strlen(userId))
    {
        lwsl_err("缺少必要的查询参数，断开连接\n");
        return -1;
    }

    for (rooms_t *room = g_rooms_list->next; room != NULL; room = room->next)
    {
        if (strcmp(room->room_id, roomid) == 0)
        {
            if (!(new_client = insert_client_info(wsi, client_ip, room, userId, nickname, avatar_url)))
            {
                lwsl_err("Failed to insert client info\n");
                return -1;
            }
            lws_set_opaque_user_data(wsi, new_client);
            lwsl_notice("客户端加入房间: %s\n", roomid);
            // 如果房间没有歌曲，自动推荐一首
            if (!room->current_song && !room->playlist_head->next)
            {
                auto_recommend_song(room);
            }
            // 打印房间信息以及客户端信息
            for (rooms_t *room = g_rooms_list->next; room != NULL; room = room->next)
            {
                print_room_info(room);
            }
            // 记录用户加入操作
            {
                char join_msg[256] = {0};
                snprintf(join_msg, sizeof(join_msg), "加入了房间");
                init_room_action(room, userId, nickname, avatar_url, 0, join_msg);
            }
            // 合并操作日志和用户列表广播，避免覆盖
            {
                const char *client_list_json = get_client_list_json(room, BROADCAST_CLIENT_LIST);
                const char *actions_json = get_room_actions_json(room, 1);
                char *combined = combine_json_with_actions(client_list_json, actions_json);
                broadcast_response_room(room, combined ? combined : client_list_json);
                free(combined);
                free((void *)actions_json);
                free((void *)client_list_json);
            }
            // 向新客户端发送合并的初始同步（播放列表 + 歌曲信息 + 操作历史）
            {
                const char *playlist_song = get_playlist_and_song_info_json(room);
                const char *actions_json = get_room_actions_json(room, 100);
                char *sync_msg = combine_json_with_actions(playlist_song, actions_json);
                send_message_to_client(new_client, sync_msg ? sync_msg : playlist_song);
                free(sync_msg);
                free((void *)actions_json);
                free((void *)playlist_song);
            }
            return 0;
        }
    }
    // 创建新房间
    lwsl_notice("创建新房间: %s\n", roomid);
    if (!(new_room = insert_room_info(roomid, userId, g_rooms_list)))
    {
        lwsl_err("Failed to create new room\n");
        return -1;
    }
    lwsl_notice("创建房间成功，启动定时器\n");
    lws_sul_schedule(context, 0, &new_room->playing_info.timer, timer_callback, LWS_US_PER_SEC * 5);
    // 自动推荐第一首歌
    auto_recommend_song(new_room);
    if (!(new_client = insert_client_info(wsi, client_ip, new_room, userId, nickname, avatar_url)))
    {
        lwsl_err("Failed to insert client info\n");
        return -1;
    }
    lws_set_opaque_user_data(wsi, new_client);
    lwsl_notice("客户端加入房间: %s\n", roomid);
    // 打印房间信息以及客户端信息
    for (rooms_t *room = g_rooms_list->next; room != NULL; room = room->next)
    {
        print_room_info(room);
    }
    // 记录创建者加入操作
    {
        char join_msg[256] = {0};
        snprintf(join_msg, sizeof(join_msg), "创建了房间");
        init_room_action(new_room, userId, nickname, avatar_url, 0, join_msg);
    }
    // 合并操作日志和客户端信息广播
    {
        const char *client_list_json = get_client_list_json(new_room, BROADCAST_CLIENT_LIST);
        const char *actions_json = get_room_actions_json(new_room, 1);
        char *combined = combine_json_with_actions(client_list_json, actions_json);
        broadcast_response_room(new_room, combined ? combined : client_list_json);
        free(combined);
        free((void *)actions_json);
        free((void *)client_list_json);
    }
    // 向创建者发送合并的初始同步（播放列表 + 歌曲信息 + 操作历史）
    {
        const char *playlist_song = get_playlist_and_song_info_json(new_room);
        const char *actions_json = get_room_actions_json(new_room, 100);
        char *sync_msg = combine_json_with_actions(playlist_song, actions_json);
        send_message_to_client(new_client, sync_msg ? sync_msg : playlist_song);
        free(sync_msg);
        free((void *)actions_json);
        free((void *)playlist_song);
    }
    return 0;
}

static int client_callback_closed(struct lws *wsi)
{
    lwsl_notice("客户端连接关闭\n");
    // 清理客户端节点
    client_info_t *client = (client_info_t *)lws_get_opaque_user_data(wsi);
    if (!client)
    {
        lwsl_err("Client info is NULL\n");
        return -1;
    }
    rooms_t *room = client->room;
    if (room)
    {
        pthread_mutex_lock(&client->room->lock);
        client_info_t *prev = client->prev;
        client_info_t *next = client->next;
        if (prev)
        {
            prev->next = next;
        }
        if (next)
        {
            next->prev = prev;
        }
        if (client->room->client_info == client)
        {
            client->room->client_info = next; // 如果是头节点，更新头节点
        }
        client->room->client_counter--;
        pthread_mutex_unlock(&client->room->lock);

        // 记录退出操作并广播给房间内其他人
        char leave_msg[256] = {0};
        snprintf(leave_msg, sizeof(leave_msg), "离开了房间");
        init_room_action(room, client->userId, client->nickname, client->avatar_url, BROADCAST_ROOM_ACTION, leave_msg);

        const char *actions_json = get_room_actions_json(room, 1);
        const char *client_list_json = get_client_list_json(room, BROADCAST_CLIENT_LIST);
        char *combined = combine_json_with_actions(client_list_json, actions_json);
        broadcast_response_room(room, combined ? combined : client_list_json);
        free(combined);
        free((void *)actions_json);
        free((void *)client_list_json);

        // 释放消息队列中剩余的消息
        for (int i = 0; i < CLIENT_MSG_QUEUE_SIZE; i++)
        {
            free(client->msg_queue[i]);
        }
        free(client);
        lwsl_notice("客户端信息已清理\n");
    }
    lws_set_opaque_user_data(wsi, NULL);

    // 如果房间已经没有客户端，则删除房间信息
    if (room && room->client_counter == 0)
    {
        remove_room_node(g_rooms_list, room);
        lwsl_notice("房间信息已清理\n");
    }

    // 打印房间信息以及客户端信息
    for (rooms_t *room = g_rooms_list->next; room != NULL; room = room->next)
    {
        print_room_info(room);
    }
    lws_cancel_service(context); // 触发服务循环处理
    return 0;
}

static void error_response(client_info_t *client, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return;

    cJSON_AddNumberToObject(root, "error_code", -FAIL);
    cJSON_AddStringToObject(root, "status", "error");
    cJSON_AddStringToObject(root, "message", msg);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    send_message_to_client(client, json_str);
    free(json_str);
}

static void error_response_with_action(client_info_t *client, const char *msg, int action)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return;

    cJSON_AddNumberToObject(root, "error_code", -FAIL);
    cJSON_AddStringToObject(root, "status", "error");
    cJSON_AddStringToObject(root, "message", msg);
    cJSON_AddNumberToObject(root, "action", action);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    send_message_to_client(client, json_str);
    free(json_str);
}

static void success_response(client_info_t *client, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return;

    cJSON_AddNumberToObject(root, "error_code", SUCCESS);
    cJSON_AddStringToObject(root, "status", "success");
    cJSON_AddStringToObject(root, "message", msg);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    send_message_to_client(client, json_str);
    free(json_str);
}
// 某客户端单独发送信息（入队，动态分配）
static void send_message_to_client(client_info_t *client, const char *msg)
{
    if (!client || !msg)
        return;
    pthread_mutex_lock(&client->lock);
    if (client->msg_queue_count < CLIENT_MSG_QUEUE_SIZE)
    {
        int slot = (client->msg_queue_head + client->msg_queue_count) % CLIENT_MSG_QUEUE_SIZE;
        client->msg_queue[slot] = strdup(msg);
        client->msg_queue_count++;
    }
    else
    {
        // 队列满，丢弃最旧的消息，推入新消息
        free(client->msg_queue[client->msg_queue_head]);
        client->msg_queue[client->msg_queue_head] = strdup(msg);
        client->msg_queue_head = (client->msg_queue_head + 1) % CLIENT_MSG_QUEUE_SIZE;
    }
    pthread_mutex_unlock(&client->lock);
    lws_callback_on_writable(client->wsi);
    lws_cancel_service(context);
}

static int client_callback_receive(struct lws *wsi, void *in, size_t len)
{
    client_info_t *client = (client_info_t *)lws_get_opaque_user_data(wsi);
    if (!client)
    {
        lwsl_err("Client info is NULL\n");
        return -1;
    }
    // 复制到本地缓冲区，避免修改 libwebsockets 的只读输入缓冲区
    if (len >= 65536)
    {
        lwsl_err("消息过长，丢弃 (长度: %zu)\n", len);
        return 0;
    }
    char *msg_buf = (char *)malloc(len + 1);
    if (!msg_buf)
    {
        lwsl_err("内存分配失败\n");
        return 0;
    }
    memcpy(msg_buf, in, len);
    msg_buf[len] = '\0';
    lwsl_notice("收到%s消息: %s (长度: %zu)\n", client->ip, msg_buf, len);

    cJSON *root = cJSON_Parse(msg_buf);
    free(msg_buf);
    if (!root)
    {
        const char *error_ptr = cJSON_GetErrorPtr();
        {
            char msg[128] = {0};
            if (error_ptr)
                snprintf(msg, sizeof(msg), "JSON 解析错误:%s", error_ptr);
            else
                snprintf(msg, sizeof(msg), "JSON 解析错误");
            lwsl_err("JSON 解析错误: %s\n", error_ptr ? error_ptr : "unknown");
            error_response(client, msg);
        }
        return 0;
    }
    cJSON *userid = cJSON_GetObjectItem(root, "userid");
    cJSON *action = cJSON_GetObjectItem(root, "action");
    cJSON *params = cJSON_GetObjectItem(root, "params");
    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type) && !strncmp(type->valuestring, "heartbeat", 9))
    {
        success_response(client, "heartbeat");
        return 0;
    }

    if (!cJSON_IsNumber(action))
    {
        lwsl_err("action类型错误！");
        error_response(client, "action类型错误！");
        return 0;
    }
    if (!cJSON_IsString(userid) || strncmp(userid->valuestring, client->userId, strlen(userid->valuestring)))
    {
        lwsl_err("userid错误！");
        error_response(client, "userid错误！");
        return 0;
    }
    switch (action->valueint)
    {
    case GET_CUR_SONG_INFO:
        {
            const char *cur_song_info_json = get_cur_song_info(client->room, GET_CUR_SONG_INFO);
            cur_song_info_json ? send_message_to_client(client, cur_song_info_json) : error_response(client, "fail!");
            free((void *)cur_song_info_json);
        }
        break;
    case PLAY_NEXT_SONG:
        if (play_next_song(client) >= 0)
        {
            const char *combined_json = get_playlist_and_song_info_json(client->room);
            const char *actions_json = get_room_actions_json(client->room, 1);
            // 操作者收到 playlist + 操作日志
            const char *operator_actions = get_room_actions_json(client->room, 1);
            {
                char *op_combined = combine_json_with_actions(combined_json, operator_actions);
                send_message_to_client(client, op_combined ? op_combined : combined_json);
                free(op_combined);
            }
            free((void *)operator_actions);
            // 其他客户端
            {
                char *final = combine_json_with_actions(combined_json, actions_json);
                const char *broadcast_msg = final ? final : combined_json;
                broadcast_response_room(client->room, broadcast_msg);
                free(final);
            }
            free((void *)combined_json);
            free((void *)actions_json);
        }
        else
        {
            error_response(client, "fail!");
        }
        break;
    case PLAY_BY_SONG_HASH:
        if (cJSON_IsObject(params))
        {
            cJSON *songhash = cJSON_GetObjectItem(params, "songhash");
            if (cJSON_IsString(songhash))
            {
                if (playbysonghash(client, songhash->valuestring) >= 0)
                {
                    const char *cur_song_info_json = get_cur_song_info(client->room, BROADCAST_SONG_INFO);
                    const char *actions_json = get_room_actions_json(client->room, 1);
                    char *combined = combine_json_with_actions(cur_song_info_json, actions_json);
                    operation_response(client, combined ? combined : cur_song_info_json);
                    if (actions_json) { send_message_to_client(client, actions_json); }
                    free(combined);
                    free((void *)cur_song_info_json);
                    free((void *)actions_json);
                    return 0;
                }
            }
            else
            {
                lwsl_err("参数错误！");
                error_response(client, "参数错误！");
                return 0;
            }
        }
        error_response(client, "fail!");
        break;
    case PAUSE_SONG:
        if (pause_song(client) >= 0)
        {
            const char *cur_song_info_json = get_cur_song_info(client->room, BROADCAST_SONG_INFO);
            const char *actions_json = get_room_actions_json(client->room, 1);
            char *combined = combine_json_with_actions(cur_song_info_json, actions_json);
            broadcast_response_room(client->room, combined ? combined : cur_song_info_json);
            free(combined);
            free((void *)cur_song_info_json);
            free((void *)actions_json);
        }
        else
        {
            error_response(client, "fail!");
        }
        break;
    case RESUME_SONG:
        if (resume_song(client) >= 0)
        {
            const char *cur_song_info_json = get_cur_song_info(client->room, BROADCAST_SONG_INFO);
            const char *actions_json = get_room_actions_json(client->room, 1);
            char *combined = combine_json_with_actions(cur_song_info_json, actions_json);
            broadcast_response_room(client->room, combined ? combined : cur_song_info_json);
            free(combined);
            free((void *)cur_song_info_json);
            free((void *)actions_json);
        }
        else
        {
            error_response(client, "fail!");
        }
        break;
    case ADD_SONG_TO_PLAYLIST:
        if (cJSON_IsObject(params))
        {
            char *songname = cJSON_GetObjectItem(params, "songname") ? cJSON_GetObjectItem(params, "songname")->valuestring : "";
            char *songhash = cJSON_GetObjectItem(params, "songhash") ? cJSON_GetObjectItem(params, "songhash")->valuestring : "";
            char *singername = cJSON_GetObjectItem(params, "singername") ? cJSON_GetObjectItem(params, "singername")->valuestring : "";
            char *albumname = cJSON_GetObjectItem(params, "albumname") ? cJSON_GetObjectItem(params, "albumname")->valuestring : "";
            char *duration = cJSON_GetObjectItem(params, "duration") ? cJSON_GetObjectItem(params, "duration")->valuestring : "";
            char *coverurl = cJSON_GetObjectItem(params, "coverurl") ? cJSON_GetObjectItem(params, "coverurl")->valuestring : "";
            if (insert_song_to_playlist(client, songname, songhash, singername, albumname, duration, coverurl) >= 0)
            {
                // 操作者收到 playlist + 操作日志
                const char *playlist_json = get_playlist_json(client->room, BROADCAST_SONG_LIST);
                send_message_to_client(client, playlist_json);
                const char *operator_actions = get_room_actions_json(client->room, 1);
                if (operator_actions) { send_message_to_client(client, operator_actions); free((void *)operator_actions); }
                free((void *)playlist_json);

                // 其他客户端收到 playlist + song_info + 操作日志（合并为一条消息避免覆盖）
                const char *combined_json = get_playlist_and_song_info_json(client->room);
                const char *actions_json = get_room_actions_json(client->room, 1);
                char *final = combine_json_with_actions(combined_json, actions_json);
                const char *broadcast_msg = final ? final : combined_json;

                broadcast_response_room(client->room, broadcast_msg);
                free(final);
                free((void *)combined_json);
                free((void *)actions_json);
            }
            else
            {
                error_response_with_action(client, "该歌曲已在播放列表中", ADD_SONG_TO_PLAYLIST);
            }
        }
        else
        {
            error_response(client, "参数错误！");
        }
        break;
    case REMOVE_SONG_FROM_PLAYLIST:
        if (cJSON_IsObject(params))
        {
            cJSON *rm_hash = cJSON_GetObjectItem(params, "songhash");
            if (cJSON_IsString(rm_hash) && remove_song_from_playlist(client, rm_hash->valuestring) >= 0)
            {
                const char *cur_playlist_json = get_playlist_json(client->room, BROADCAST_SONG_LIST);
                const char *actions_json = get_room_actions_json(client->room, 1);
                char *combined = combine_json_with_actions(cur_playlist_json, actions_json);
                broadcast_response_room(client->room, combined ? combined : cur_playlist_json);
                free(combined);
                free((void *)cur_playlist_json);
                free((void *)actions_json);
            }
            else
            {
                error_response(client, "fail!");
            }
        }
        else
        {
            error_response(client, "参数错误！");
        }
        break;
    case UP_SONGBYHASH:
        if (cJSON_IsObject(params))
        {
            cJSON *up_hash = cJSON_GetObjectItem(params, "songhash");
            if (cJSON_IsString(up_hash) && upsongbyhash(client, up_hash->valuestring) >= 0)
            {
                const char *cur_playlist_json = get_playlist_json(client->room, BROADCAST_SONG_LIST);
                const char *actions_json = get_room_actions_json(client->room, 1);
                char *combined = combine_json_with_actions(cur_playlist_json, actions_json);
                broadcast_response_room(client->room, combined ? combined : cur_playlist_json);
                free(combined);
                free((void *)cur_playlist_json);
                free((void *)actions_json);
            }
            else
            {
                error_response(client, "fail!");
            }
        }
        else
        {
            error_response(client, "参数错误！");
        }
        break;
    case GET_PLAYLIST:
        {
            const char *playlist_json = get_playlist_json(client->room, GET_PLAYLIST);
            playlist_json ? send_message_to_client(client, playlist_json) : error_response(client, "fail!");
            free((void *)playlist_json);
        }
        break;
    case GET_CLIENT_LIST:
        {
            const char *client_list_json = get_client_list_json(client->room, GET_CLIENT_LIST);
            client_list_json ? send_message_to_client(client, client_list_json) : error_response(client, "fail!");
            free((void *)client_list_json);
        }
        break;
    case SEND_CHAT:
        {
            cJSON *msg_item = params ? cJSON_GetObjectItem(params, "message") : NULL;
            if (cJSON_IsString(msg_item) && strlen(msg_item->valuestring) > 0 && strlen(msg_item->valuestring) <= 500)
            {
                cJSON *chat_root = cJSON_CreateObject();
                cJSON *chat_data = cJSON_CreateObject();
                cJSON_AddNumberToObject(chat_root, "action", BROADCAST_CHAT);
                cJSON_AddStringToObject(chat_root, "status", "success");
                cJSON_AddStringToObject(chat_data, "userid", client->userId);
                cJSON_AddStringToObject(chat_data, "nickname", client->nickname);
                cJSON_AddStringToObject(chat_data, "avatar_url", client->avatar_url);
                cJSON_AddStringToObject(chat_data, "message", msg_item->valuestring);
                cJSON_AddNumberToObject(chat_data, "time", (double)time(NULL));
                cJSON_AddItemToObject(chat_root, "data", chat_data);
                char *chat_json = cJSON_PrintUnformatted(chat_root);
                cJSON_Delete(chat_root);
                broadcast_response_room(client->room, chat_json);
                free(chat_json);
                // 存入操作历史，供新加入用户查看
                init_room_action(client->room, client->userId, client->nickname, client->avatar_url, SEND_CHAT, msg_item->valuestring);
            }
            else
            {
                int msg_len = (msg_item && cJSON_IsString(msg_item)) ? (int)strlen(msg_item->valuestring) : 0;
                error_response(client, msg_len > 500 ? "消息过长，最多500字" : "消息不能为空");
            }
        }
        break;
    default:
        lwsl_err("未识别的操作！");
        error_response(client, "未识别的操作！");
        break;
    }
    return 0;
}

static int client_callback_wirtable(struct lws *wsi)
{
    char *local_msg = NULL;

    client_info_t *client = (client_info_t *)lws_get_opaque_user_data(wsi);
    if (!client)
    {
        lwsl_err("Client info is NULL\n");
        return -1;
    }

    // 优先发送个人消息队列
    pthread_mutex_lock(&client->lock);
    if (client->msg_queue_count > 0)
    {
        local_msg = client->msg_queue[client->msg_queue_head];
        client->msg_queue[client->msg_queue_head] = NULL;
        client->msg_queue_head = (client->msg_queue_head + 1) % CLIENT_MSG_QUEUE_SIZE;
        client->msg_queue_count--;
    }
    pthread_mutex_unlock(&client->lock);

    if (local_msg)
    {
        size_t msg_len = strlen(local_msg);
        unsigned char *client_msg = (unsigned char *)malloc(LWS_PRE + msg_len);
        if (!client_msg)
        {
            free(local_msg);
            return -1;
        }
        memcpy(&client_msg[LWS_PRE], local_msg, msg_len);
        lws_write(wsi, &client_msg[LWS_PRE], msg_len, LWS_WRITE_TEXT);
        lwsl_notice("向%s发送消息: %s\n", client->ip, local_msg);
        free(client_msg);
        free(local_msg);

        // 队列还有消息，继续触发可写回调
        if (client->msg_queue_count > 0)
        {
            lws_callback_on_writable(wsi);
        }
        return 0;
    }

    // 没有个人消息，发送房间广播（仅当内容更新时）
    pthread_mutex_lock(&client->room->lock);
    if (client->room->broadcast_version != client->last_broadcast_version)
    {
        local_msg = client->room->latest_msg ? strdup(client->room->latest_msg) : NULL;
        client->last_broadcast_version = client->room->broadcast_version;
    }
    pthread_mutex_unlock(&client->room->lock);

    if (!local_msg)
    {
        return 0;
    }

    size_t msg_len = strlen(local_msg);
    unsigned char *buffer = (unsigned char *)malloc(LWS_PRE + msg_len);
    if (!buffer)
    {
        free(local_msg);
        return -1;
    }
    memcpy(&buffer[LWS_PRE], local_msg, msg_len);
    lws_write(wsi, &buffer[LWS_PRE], msg_len, LWS_WRITE_TEXT);
    lwsl_notice("向%s发送广播消息: %s\n", client->ip, local_msg);
    free(buffer);
    free(local_msg);
    return 0;
}

// WebSocket 回调函数，处理各种事件
int callback_echo(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
    (void)user;
    int ret = 0;
    switch (reason)
    {
    // HTTP 请求处理（房间列表接口）
    case LWS_CALLBACK_HTTP:
    {
        char url[128] = {0};
        lws_hdr_copy(wsi, url, sizeof(url) - 1, WSI_TOKEN_GET_URI);

        if (strcmp(url, "/rooms") == 0)
        {
            cJSON *arr = cJSON_CreateArray();
            for (rooms_t *room = g_rooms_list->next; room != NULL; room = room->next)
            {
                cJSON *r = cJSON_CreateObject();
                cJSON_AddStringToObject(r, "room_id", room->room_id);
                cJSON_AddNumberToObject(r, "member_count", room->client_counter);
                if (room->current_song)
                {
                    cJSON_AddStringToObject(r, "current_song", room->current_song->song_name);
                    cJSON_AddStringToObject(r, "singername", room->current_song->singer_name);
                    cJSON_AddStringToObject(r, "cover_url", room->current_song->cover_url);
                }
                cJSON_AddItemToArray(arr, r);
            }
            char *json_str = cJSON_PrintUnformatted(arr);
            cJSON_Delete(arr);
            size_t json_len = strlen(json_str);

            size_t buf_size = LWS_PRE + 512 + json_len;
            unsigned char *buf = (unsigned char *)malloc(buf_size);
            if (!buf)
            {
                free(json_str);
                return -1;
            }
            unsigned char *p = &buf[LWS_PRE];
            int hdr_len = snprintf((char *)p, buf_size - LWS_PRE,
                                   "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: application/json\r\n"
                                   "Access-Control-Allow-Origin: *\r\n"
                                   "Connection: close\r\n"
                                   "Content-Length: %zu\r\n"
                                   "\r\n",
                                   json_len);
            lws_write(wsi, p, hdr_len, LWS_WRITE_HTTP_HEADERS);

            p = &buf[LWS_PRE];
            memcpy(p, json_str, json_len);
            lws_write(wsi, p, json_len, LWS_WRITE_HTTP);

            free(buf);
            free(json_str);
            if (lws_http_transaction_completed(wsi)) return -1;
            return 0;
        }

        lws_return_http_status(wsi, 404, "Not Found");
        if (lws_http_transaction_completed(wsi)) return -1;
        return 0;
    }
    // 过滤新连接请求
    case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
        break;
    // 新连接建立
    case LWS_CALLBACK_ESTABLISHED:
        ret = client_callback_established(wsi);
        break;
    // 接收到客户端消息
    case LWS_CALLBACK_RECEIVE:
        ret = client_callback_receive(wsi, in, len);
        break;
    // 这里可以处理需要发送的数据
    case LWS_CALLBACK_SERVER_WRITEABLE:
        ret = client_callback_wirtable(wsi);
        break;
    // 连接关闭
    case LWS_CALLBACK_CLOSED:
        ret = client_callback_closed(wsi);
        break;

    default:
        break;
    }

    return ret;
}

// 打印使用帮助
static void print_usage(const char *prog)
{
    fprintf(stderr, "用法: %s [选项]\n", prog);
    fprintf(stderr, "  -p <端口>    监听端口 (默认: 3375)\n");
    fprintf(stderr, "  -l <文件>    日志输出文件 (默认: 仅 stderr)\n");
    fprintf(stderr, "  -d           以守护进程模式运行\n");
    fprintf(stderr, "  -h           显示帮助\n");
}

// 服务器主函数
int main(int argc, const char **argv)
{
    struct lws_context_creation_info info;
    const char *iface = "0.0.0.0";
    int port = 3001;
    int opts = 0;
    int daemon_mode = 0;
    const char *log_path = NULL;
    int opt;

    // 解析命令行参数
    while ((opt = getopt(argc, (char *const *)argv, "p:l:dh")) != -1)
    {
        switch (opt)
        {
        case 'p':
            port = atoi(optarg);
            if (port <= 0 || port > 65535)
            {
                fprintf(stderr, "无效端口号: %s\n", optarg);
                return 1;
            }
            break;
        case 'l':
            log_path = optarg;
            break;
        case 'd':
            daemon_mode = 1;
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    // 打开日志文件
    if (log_path)
    {
        log_file = fopen(log_path, "a");
        if (!log_file)
        {
            fprintf(stderr, "无法打开日志文件: %s\n", log_path);
            return 1;
        }
    }

    // 设置日志输出
    lws_set_log_level(LLL_NOTICE | LLL_ERR, log_emit_function);

    // 守护进程模式
    if (daemon_mode)
    {
        if (daemon(1, 0) < 0)
        {
            lwsl_err("daemon() 失败\n");
            return 1;
        }
        lwsl_notice("已切换为守护进程模式\n");
    }

    curl_global_init(CURL_GLOBAL_ALL);
    srand((unsigned int)time(NULL));

    g_rooms_list = init_rooms();
    if (!g_rooms_list)
    {
        lwsl_err("Failed to initialize rooms\n");
        return -1;
    }

    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 初始化上下文创建信息
    memset(&info, 0, sizeof info);
    info.port = port;
    info.iface = iface;
    info.protocols = protocols;
    info.options = opts;

    // 创建上下文
    context = lws_create_context(&info);
    if (!context)
    {
        lwsl_err("创建上下文失败\n");
        return 1;
    }

    lwsl_notice("WebSocket 服务器已启动，监听端口 %d\n", port);
    lwsl_notice("按 Ctrl+C 或发送 SIGTERM 退出...\n");

    // 事件循环
    while (!interrupted)
    {
        lws_service(context, 10);
    }

    // 清理资源
    lwsl_notice("服务器正在关闭...\n");
    lws_context_destroy(context);
    curl_global_cleanup();

    if (log_file)
    {
        fclose(log_file);
    }

    return 0;
}

#include "websocket_service.h"
#include "rooms.h"
#include <sys/socket.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include "types.h"
#include <stdbool.h>
#include "playlist.h"

int callback_echo(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len);
static void success_response(client_info_t *client, const char *msg);
static void error_response(client_info_t *client, const char *msg);

struct lws_context *context = NULL;
static int interrupted = 0;

rooms_t *g_rooms_list = NULL; // 房间链表

// 定义协议处理结构
static struct lws_protocols protocols[] = {
    {
        "ctrl-protocol", // 协议名称
        callback_echo,   // 回调函数
        0,               // 每个连接的用户数据大小
        1024,            // 接收缓冲区大小
    },
    {NULL, NULL, 0, 0} // 协议列表结束标记
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
client_info_t *insert_client_info(struct lws *wsi, const char *ip, rooms_t *room, const char *userId)
{
    client_info_t *new_node = (client_info_t *)malloc(sizeof(client_info_t));
    if (!new_node)
    {
        lwsl_err("Failed to allocate memory for client_info_t\n");
        return false;
    }
    memset(new_node, 0, sizeof(client_info_t));
    pthread_mutex_init(&new_node->lock, NULL);
    new_node->wsi = wsi;
    strncpy(new_node->ip, ip, INET_ADDRSTRLEN - 1);
    new_node->room = room;
    strncpy(new_node->userId, userId, 63);
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

// 对应房间客户端发送广播消息
void submit_broadcast_message(struct lws *wsi, const char *msg)
{
    if (!msg)
    {
        lwsl_err("Message is NULL\n");
        return;
    }
    client_info_t *client = (client_info_t *)lws_get_opaque_user_data(wsi);
    if (!client)
    {
        lwsl_err("Client info is NULL\n");
        return;
    }
    pthread_mutex_lock(&client->room->lock);
    strncpy(client->room->latest_msg, msg, sizeof(client->room->latest_msg) - 1);
    client->room->latest_msg[sizeof(client->room->latest_msg) - 1] = '\0';
    pthread_mutex_unlock(&client->room->lock);

    // 遍历该房间客户端链表，唤醒对应客户端发送信息
    for (client_info_t *cur = client->room->client_info->next; cur != NULL; cur = cur->next)
    {
        if (cur->wsi)
        {
            lws_callback_on_writable(cur->wsi);
        }
    }
    lws_cancel_service(context);
}

// 对应房间发送广播信息
static void broadcast_response_room(rooms_t *room, const char *msg)
{
    pthread_mutex_lock(&room->lock);
    strncpy(room->latest_msg, msg, sizeof(room->latest_msg));
    room->latest_msg[sizeof(room->latest_msg) - 1] = '\0';
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

// 操作回复广播（操作者回复成功与否，其他客户端回复最新数据）
static void operation_response(client_info_t *client, const char *msg)
{
    if (!msg || !client)
        return;

    // 操作客户端回复
    success_response(client, "操作成功");

    pthread_mutex_lock(&client->room->lock);
    strncpy(client->room->latest_msg, msg, sizeof(client->room->latest_msg) - 1);
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

// 信号处理函数，用于优雅退出
static void sigint_handler(int sig)
{
    interrupted = 1;
}

static int client_callback_filter(struct lws *wsi)
{
    lwsl_notice("新的客户端申请连接\n");
    return 0;
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
        duration = atof(playing_info->duration);
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
    if (playing_info->played_percent >= 1)
    {
        play_next_song_bysystem(playing_info->room);
    }
    // 广播播放信息(有歌曲的时候)
    if (playing_info->room->current_song)
    {
        const char *cur_song_info_json = get_cur_played_percent(playing_info->room);
        broadcast_response_room(playing_info->room, cur_song_info_json);
    }

    lws_sul_schedule(context, 0, sul, timer_callback, callback_time * LWS_US_PER_MS);
}

static int client_callback_established(struct lws *wsi)
{
    lwsl_notice("新的客户端连接建立\n");
    char roomid[64] = {0};
    char userId[64] = {0};
    char client_ip[64] = {0};
    rooms_t *new_room = NULL;
    client_info_t *new_client = NULL;

    if (g_rooms_list == NULL)
    {
        lwsl_err("房间链表未初始化\n");
        return -1;
    }

    lws_get_peer_simple(wsi, client_ip, sizeof(client_ip));
    if (!strlen(client_ip))
    {
        lwsl_err("无法获取客户端IP，断开连接\n");
        return -1;
    }

    lws_get_urlarg_by_name(wsi, "roomid", roomid, sizeof(roomid));
    lws_get_urlarg_by_name(wsi, "userid", userId, sizeof(userId));

    if (!strlen(roomid) || !strlen(userId))
    {
        lwsl_err("缺少必要的查询参数，断开连接\n");
        return -1;
    }

    for (rooms_t *room = g_rooms_list->next; room != NULL; room = room->next)
    {
        if (strcmp(room->room_id, roomid) == 0)
        {
            if (!(new_client = insert_client_info(wsi, client_ip, room, userId)))
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
            // 广播新的客户端信息
            broadcast_response_room(room, get_client_list_json(room, BROADCAST_CLIENT_LIST));
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
    if (!(new_client = insert_client_info(wsi, client_ip, new_room, userId)))
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
    return 0;
}

static int client_callback_closed(struct lws *wsi)
{
    lwsl_notice("客户端连接关闭\n");
    // 清理客户端节点
    client_info_t *client = (client_info_t *)lws_get_opaque_user_data(wsi);
    rooms_t *room = client->room;
    if (!client)
    {
        lwsl_err("Client info is NULL\n");
        return -1;
    }
    if (client->room)
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
        free(client);
        lwsl_notice("客户端信息已清理\n");
    }
    lws_set_opaque_user_data(wsi, NULL);

    // 如果房间已经没有客户端，则删除房间信息
    if (room->client_counter == 0)
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
    // 1. 创建根对象 {}
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return;

    // 2. 添加字段
    cJSON_AddNumberToObject(root, "error_code", -FAIL);
    cJSON_AddStringToObject(root, "status", "error");
    cJSON_AddStringToObject(root, "message", msg);

    // 3. 转为字符串（注意：返回的字符串需要 free()）
    char *json_str = cJSON_PrintUnformatted(root);

    // 4. 释放 cJSON 对象（但保留字符串）
    cJSON_Delete(root);

    pthread_mutex_lock(&client->lock);
    strncpy(client->latest_msg, json_str, sizeof(client->latest_msg) - 1);
    client->is_data_to_send = 1;
    pthread_mutex_unlock(&client->lock);
    lws_callback_on_writable(client->wsi);
    lws_cancel_service(context);
}

static void success_response(client_info_t *client, const char *msg)
{
    // 1. 创建根对象 {}
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return;

    // 2. 添加字段
    cJSON_AddNumberToObject(root, "error_code", SUCCESS);
    cJSON_AddStringToObject(root, "status", "success");
    cJSON_AddStringToObject(root, "message", msg);

    // 3. 转为字符串（注意：返回的字符串需要 free()）
    char *json_str = cJSON_PrintUnformatted(root);

    // 4. 释放 cJSON 对象（但保留字符串）
    cJSON_Delete(root);

    pthread_mutex_lock(&client->lock);
    strncpy(client->latest_msg, json_str, sizeof(client->latest_msg) - 1);
    client->is_data_to_send = 1;
    pthread_mutex_unlock(&client->lock);
    lws_callback_on_writable(client->wsi);
    lws_cancel_service(context);
}
// 某客户端单独发送信息
static void send_message_to_client(client_info_t *client, const char *msg)
{
    if (!client)
        return;
    pthread_mutex_lock(&client->lock);
    strncpy(client->latest_msg, msg, sizeof(client->latest_msg) - 1);
    client->is_data_to_send = 1;
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
    ((char *)in)[len] = '\0'; // 确保消息以null结尾
    lwsl_notice("收到%s消息: %s (长度: %zu)\n", client->ip, (char *)in, len);

    cJSON *root = cJSON_Parse((char *)in);
    if (!root)
    {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL)
        {
            char msg[128] = {0};
            sprintf(msg, "JSON 解析错误:%s", error_ptr);
            lwsl_err("JSON 解析错误: %s\n", error_ptr);
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
        const char *cur_song_info_json = get_cur_song_info(client->room, GET_CUR_SONG_INFO);
        cur_song_info_json ? send_message_to_client(client, cur_song_info_json) : error_response(client, "fail!");
        break;
    case PLAY_NEXT_SONG:
        if (play_next_song(client) >= 0)
        {
            const char *cur_song_info_json = get_cur_song_info(client->room, BROADCAST_SONG_INFO);
            operation_response(client, cur_song_info_json);
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
                    operation_response(client, cur_song_info_json);
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
            operation_response(client, cur_song_info_json);
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
            operation_response(client, cur_song_info_json);
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
                const char *cur_playlist_json = get_playlist_json(client->room, BROADCAST_SONG_LIST);
                operation_response(client, cur_playlist_json);
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
    case REMOVE_SONG_FROM_PLAYLIST:
        if (cJSON_IsObject(params))
        {
            if (remove_song_from_playlist(client, cJSON_GetObjectItem(params, "songhash")->valuestring) >= 0)
            {
                const char *cur_playlist_json = get_playlist_json(client->room, BROADCAST_SONG_LIST);
                operation_response(client, cur_playlist_json);
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
            if (upsongbyhash(client, cJSON_GetObjectItem(params, "songhash")->valuestring) >= 0)
            {
                const char *cur_playlist_json = get_playlist_json(client->room, BROADCAST_SONG_LIST);
                operation_response(client, cur_playlist_json);
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
        const char *playlist_json = get_playlist_json(client->room, GET_PLAYLIST);
        playlist_json ? send_message_to_client(client, playlist_json) : error_response(client, "fail!");
        break;
    case GET_CLEIENT_LIST:
        const char *client_list_json = get_client_list_json(client->room, GET_CLEIENT_LIST);
        client_list_json ? send_message_to_client(client, client_list_json) : error_response(client, "fail!");
    default:
        lwsl_err("未识别的操作！");
        error_response(client, "未识别的操作！");
        break;
    }
    return 0;
}

static int client_callback_wirtable(struct lws *wsi)
{
    char local_msg[1024] = {0};
    client_info_t *client = (client_info_t *)lws_get_opaque_user_data(wsi);
    if (!client)
    {
        lwsl_err("Client info is NULL\n");
        return -1;
    }
    pthread_mutex_lock(&client->lock);
    if (client->is_data_to_send)
    {
        strcpy(local_msg, client->latest_msg);
        client->is_data_to_send = 0;
    }
    pthread_mutex_unlock(&client->lock);

    if (strlen(local_msg))
    {
        unsigned char client_msg[LWS_PRE + sizeof(local_msg)];
        unsigned char *clnent_p = &client_msg[LWS_PRE];
        size_t client_n = strlen(local_msg);
        memcpy(clnent_p, local_msg, client_n);
        lws_write(wsi, clnent_p, client_n, LWS_WRITE_TEXT);
        lwsl_notice("向%s发送消息: %s\n", client->ip, local_msg);
        return 0;
    }

    pthread_mutex_lock(&client->room->lock);
    strcpy(local_msg, client->room->latest_msg);
    pthread_mutex_unlock(&client->room->lock);

    unsigned char buffer[LWS_PRE + sizeof(local_msg)];
    unsigned char *p = &buffer[LWS_PRE];
    size_t n = strlen(local_msg);

    memcpy(p, local_msg, n);
    lws_write(wsi, p, n, LWS_WRITE_TEXT);
    lwsl_notice("向%s发送广播消息: %s\n", client->ip, local_msg);
    return 0;
}

// WebSocket 回调函数，处理各种事件
int callback_echo(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
    int ret = 0;
    switch (reason)
    {
    // 过滤新连接请求
    case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
        // ret = client_callback_filter(wsi);
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

// 服务器主函数
int main(int argc, const char **argv)
{
    struct lws_context_creation_info info;
    const char *iface = NULL;
    int port = 3375;
    int opts = 0;

    // 初始化日志系统
    lws_set_log_level(LLL_NOTICE | LLL_ERR, NULL);
    // 初始化 http—get
    curl_global_init(CURL_GLOBAL_ALL);

    g_rooms_list = init_rooms();
    if (!g_rooms_list)
    {
        lwsl_err("Failed to initialize rooms\n");
        return -1;
    }

    // 设置信号处理
    signal(SIGINT, sigint_handler);

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
    lwsl_notice("按 Ctrl+C 退出...\n");

    // 事件循环
    while (!interrupted)
    {
        // 处理网络事件，超时设置为 10 毫秒
        lws_service(context, 10);
    }

    // 清理资源
    lwsl_notice("服务器正在关闭...\n");
    lws_context_destroy(context);

    return 0;
}

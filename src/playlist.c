#include <stdlib.h>
#include <libwebsockets.h>
#include "cJSON.h"
#include "playlist.h"
#include "websocket_service.h"
#include "rooms.h"

#define SERVICE_BASE_URL "https://xjt-togethertracks.top/api"

extern struct lws_context *context;

// 内存结构体
struct ResponseData
{
    char *data;
    size_t size;
};

// 写入回调函数
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    struct ResponseData *mem = (struct ResponseData *)userp;

    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr)
        return 0;

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;

    return realsize;
}

// 通用的HTTP请求函数
struct ResponseData *http_request(const char *url,
                                  const char *method,
                                  const char *post_data,
                                  struct curl_slist *headers)
{
    CURL *curl;
    CURLcode res;
    struct ResponseData *response;

    response = malloc(sizeof(struct ResponseData));
    response->data = malloc(1);
    response->size = 0;

    curl = curl_easy_init();
    if (!curl)
    {
        free(response->data);
        free(response);
        return NULL;
    }

    // 基本设置
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)response);

    // 方法设置
    if (strcmp(method, "POST") == 0)
    {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (post_data)
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
    }
    else if (strcmp(method, "PUT") == 0)
    {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        if (post_data)
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
    }
    else if (strcmp(method, "DELETE") == 0)
    {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    }

    // 头部设置
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // 其他选项
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "MyCurlClient/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    // 执行请求
    res = curl_easy_perform(curl);

    if (res != CURLE_OK)
    {
        lwsl_err("Failed to perform HTTP request: %s--:%s", url, curl_easy_strerror(res));
        free(response->data);
        free(response);
        response = NULL;
    }

    curl_easy_cleanup(curl);
    return response;
}

// 获取歌曲 url（始终返回可 free 的堆内存，调用者必须 free）
char *get_song_url(const char *song_hash)
{
    struct ResponseData *response;
    char url[512] = {0};
    char *song_url = (char *)malloc(1024);
    if (!song_url)
        return NULL;
    memset(song_url, 0, 1024);
    if (!song_hash)
    {
        return song_url; // 空字符串
    }
    // 拼接url
    snprintf(url, sizeof(url), "%s/song/url?hash=%s", SERVICE_BASE_URL, song_hash);
    response = http_request(url, "GET", NULL, NULL);
    if (!response)
    {
        return song_url; // 空字符串
    }
    cJSON *root = cJSON_Parse(response->data);
    if (!root)
    {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL)
        {
            lwsl_err("JSON 解析错误: %s\n", error_ptr);
        }
        free(response->data);
        free(response);
        return song_url; // 空字符串
    }
    cJSON *urls = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsArray(urls))
    {
        lwsl_err("JSON 解析错误: url 不是数组");
        cJSON_Delete(root);
        free(response->data);
        free(response);
        return song_url; // 空字符串
    }
    cJSON *url_obj = cJSON_GetArrayItem(urls, 0);
    if (url_obj && url_obj->valuestring)
    {
        strncpy(song_url, url_obj->valuestring, 1023);
    }
    free(response->data);
    free(response);
    cJSON_Delete(root);
    return song_url;
}

// 获取该房间所有的客户端信息
const char *get_client_list_json(rooms_t *room, enum ctrl cmd)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *client_list = cJSON_CreateArray();
    client_info_t *client = room->client_info->next;
    if (!room || !root)
        return NULL;

    pthread_mutex_lock(&room->lock);
    while (client)
    {
        cJSON *client_info = cJSON_CreateObject();
        cJSON_AddStringToObject(client_info, "ip", client->ip);
        cJSON_AddStringToObject(client_info, "userId", client->userId);
        cJSON_AddStringToObject(client_info, "nickname", client->nickname);
        cJSON_AddStringToObject(client_info, "avatar_url", client->avatar_url);
        cJSON_AddNumberToObject(client_info, "client_counter", room->client_counter);
        cJSON_AddItemToArray(client_list, client_info);
        client = client->next;
    }
    pthread_mutex_unlock(&room->lock);
    cJSON_AddItemToObject(root, "client_list", client_list);
    cJSON_AddNumberToObject(root, "action", cmd);
    cJSON_AddStringToObject(root, "status", "success");
    cJSON_AddNumberToObject(root, "error_code", SUCCESS);
    const char *json = cJSON_Print(root);
    cJSON_Delete(root);
    return json;
}

int insert_song_to_playlist(client_info_t *client, const char *song_name, const char *song_hash,
                            const char *singer_name, const char *album_name,
                            const char *duration, const char *cover_url)
{
    rooms_t *room = client->room;
    if (!room || !song_name || !song_hash)
    {
        return -1;
    }

    // 检查歌曲是否已存在
    pthread_mutex_lock(&room->lock);
    for (playlist_t *cur = room->playlist_head->next; cur != NULL; cur = cur->next)
    {
        if (strcmp(cur->song_hash, song_hash) == 0)
        {
            pthread_mutex_unlock(&room->lock);
            return -2; // 歌曲已存在
        }
    }
    pthread_mutex_unlock(&room->lock);

    playlist_t *new_song = (playlist_t *)malloc(sizeof(playlist_t));
    if (!new_song)
    {
        lwsl_err("Failed to allocate memory for playlist_t\n");
        return -1;
    }
    memset(new_song, 0, sizeof(playlist_t));
    strncpy(new_song->song_name, song_name, sizeof(new_song->song_name) - 1);
    strncpy(new_song->song_hash, song_hash, sizeof(new_song->song_hash) - 1);
    strncpy(new_song->singer_name, singer_name, sizeof(new_song->singer_name) - 1);
    strncpy(new_song->album_name, album_name, sizeof(new_song->album_name) - 1);
    strncpy(new_song->duration, duration, sizeof(new_song->duration) - 1);
    strncpy(new_song->cover_url, cover_url, sizeof(new_song->cover_url) - 1);
    strncpy(new_song->added_by_nickname, client->nickname, sizeof(new_song->added_by_nickname) - 1);
    strncpy(new_song->added_by_avatar, client->avatar_url, sizeof(new_song->added_by_avatar) - 1);
    new_song->next = NULL;

    // 插入到播放列表末尾
    pthread_mutex_lock(&room->lock);
    playlist_t *tail = room->playlist_tail;
    tail->next = new_song;
    room->playlist_tail = new_song;

    // 如果是第一首歌曲，则更新当前歌曲信息
    if (room->current_song == NULL)
    {
        room->current_song = new_song;
        pthread_mutex_unlock(&room->lock);
        update_playing_info(room);
    }
    else
    {
        pthread_mutex_unlock(&room->lock);
    }
    char message[128] = {0};
    snprintf(message, sizeof(message), "添加歌曲：%s", song_name);
    init_room_action(room, client->userId, client->nickname, client->avatar_url, ADD_SONG_TO_PLAYLIST, message);
    return 0;
}

int remove_song_from_playlist(client_info_t *client, const char *song_hash)
{
    if (!client)
        return -1;
    rooms_t *room = client->room;

    if (!room || !song_hash)
    {
        return -1;
    }

    pthread_mutex_lock(&room->lock);
    playlist_t *prev = room->playlist_head;
    playlist_t *curr = prev->next;

    while (curr)
    {
        if (strcmp(curr->song_hash, song_hash) == 0)
        {
            char message[256] = {0};
            snprintf(message, sizeof(message), "删除歌曲：%s", curr->song_name);

            // 如果删除的是当前正在播放的歌曲，切换到下一首
            int was_current = (room->current_song == curr);
            playlist_t *next_song = curr->next;

            prev->next = next_song;
            if (curr == room->playlist_tail)
            {
                room->playlist_tail = prev;
            }
            free(curr);

            if (was_current)
            {
                room->current_song = next_song ? next_song : room->playlist_head->next;
                pthread_mutex_unlock(&room->lock);
                init_room_action(room, client->userId, client->nickname, client->avatar_url, REMOVE_SONG_FROM_PLAYLIST, message);
                if (room->current_song)
                {
                    update_playing_info(room);
                }
                return 0;
            }

            pthread_mutex_unlock(&room->lock);
            init_room_action(room, client->userId, client->nickname, client->avatar_url, REMOVE_SONG_FROM_PLAYLIST, message);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }
    pthread_mutex_unlock(&room->lock);

    return -1; // 未找到歌曲
}
// 切换正在播放的歌曲信息
int update_playing_info(rooms_t *room)
{
    if (!room || !room->current_song)
    {
        return -1;
    }

    playlist_t *curr = room->current_song;
    playing_info_t *playing_info = &room->playing_info;

    pthread_mutex_lock(&playing_info->lock);

    strncpy(playing_info->song_name, curr->song_name, sizeof(playing_info->song_name) - 1);
    strncpy(playing_info->song_hash, curr->song_hash, sizeof(playing_info->song_hash) - 1);
    strncpy(playing_info->singer_name, curr->singer_name, sizeof(playing_info->singer_name) - 1);
    strncpy(playing_info->album_name, curr->album_name, sizeof(playing_info->album_name) - 1);
    strncpy(playing_info->duration, curr->duration, sizeof(playing_info->duration) - 1);
    strncpy(playing_info->cover_url, curr->cover_url, sizeof(playing_info->cover_url) - 1);
    // 获取歌曲 url 填充进去
    char *song_url = get_song_url(curr->song_hash);
    strncpy(playing_info->song_url, song_url, sizeof(playing_info->song_url) - 1);
    free(song_url);
    playing_info->played_percent = 0; // 重置播放进度
    playing_info->is_playing = 1;     // 设置为正在播放
    playing_info->start_time = time(NULL);
    playing_info->last_update_time = playing_info->start_time;
    lws_sul_schedule(context, 0, &playing_info->timer, timer_callback, 1 * LWS_US_PER_SEC);
    pthread_mutex_unlock(&playing_info->lock);

    return 0;
}

// 系统播放下一首
int play_next_song_bysystem(rooms_t *room)
{
    if (!room || !room->current_song)
        return -1;
    pthread_mutex_lock(&room->lock);
    room->current_song = room->current_song->next;
    if (!room->current_song)
    {
        // 播放列表结束，重置为头节点
        room->current_song = room->playlist_head->next;
    }
    pthread_mutex_unlock(&room->lock);
    update_playing_info(room);
    return 0;
}

int play_next_song(client_info_t *client)
{
    if (!client)
        return -1;
    rooms_t *room = client->room;

    if (!room || !room->current_song)
    {
        return -1;
    }
    pthread_mutex_lock(&room->lock);
    room->current_song = room->current_song->next;
    if (!room->current_song)
    {
        // 播放列表结束，重置为头节点
        room->current_song = room->playlist_head->next;
    }
    char message[1024] = {0};
    snprintf(message, sizeof(message), "播放了下一首:%s", room->current_song->song_name);
    init_room_action(room, client->userId, client->nickname, client->avatar_url, PLAY_BY_SONG_HASH, message);
    pthread_mutex_unlock(&room->lock);
    update_playing_info(room);

    return 0;
}
int playbysonghash(client_info_t *client, const char *song_hash)
{
    if (!client)
        return -1;
    rooms_t *room = client->room;

    if (!room || !song_hash)
    {
        return -1;
    }

    playlist_t *curr = room->playlist_head->next;
    pthread_mutex_lock(&room->lock);
    while (curr)
    {
        if (strcmp(curr->song_hash, song_hash) == 0)
        {
            room->current_song = curr;
            char message[1024] = {0};
            snprintf(message, sizeof(message), "播放了%s", room->current_song->song_name);
            init_room_action(room, client->userId, client->nickname, client->avatar_url, PLAY_BY_SONG_HASH, message);
            pthread_mutex_unlock(&room->lock);
            update_playing_info(room);
            return 0;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&room->lock);

    return -1; // 未找到歌曲
}
// 将歌曲置顶
int upsongbyhash(client_info_t *client, const char *song_hash)
{
    if (!client)
        return -1;
    rooms_t *room = client->room;
    if (!room || !song_hash)
    {
        return -1;
    }

    playlist_t *prev = room->playlist_head;
    playlist_t *curr = prev->next;

    pthread_mutex_lock(&room->lock);

    while (curr)
    {
        if (strcmp(curr->song_hash, song_hash) == 0)
        {
            char message[256] = {0};
            snprintf(message, sizeof(message), "将歌曲置顶：%s", curr->song_name);
            init_room_action(room, client->userId, client->nickname, client->avatar_url, UP_SONGBYHASH, message);
            // 找到歌曲，进行置顶操作
            prev->next = curr->next;
            if (curr == room->playlist_tail)
            {
                room->playlist_tail = prev;
            }
            // 插入到头节点后面
            curr->next = room->playlist_head->next;
            room->playlist_head->next = curr;
            pthread_mutex_unlock(&room->lock);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }
    pthread_mutex_unlock(&room->lock);

    return -1; // 未找到歌曲
}

// 获取当前歌曲进度同步信息（轻量级，仅 songhash + played_percent + is_playing）
const char *get_cur_song_progress(rooms_t *room)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return NULL;

    cJSON *data = cJSON_CreateObject();
    if (!data)
    {
        cJSON_Delete(root);
        return NULL;
    }

    pthread_mutex_lock(&room->playing_info.lock);

    cJSON_AddNumberToObject(root, "error_code", SUCCESS);
    cJSON_AddStringToObject(root, "status", "success");
    cJSON_AddNumberToObject(root, "action", BROADCAST_SONG_PROGRESS);
    cJSON_AddStringToObject(data, "songhash", room->playing_info.song_hash);
    cJSON_AddNumberToObject(data, "played_percent", room->playing_info.played_percent);
    cJSON_AddNumberToObject(data, "is_playing", room->playing_info.is_playing);
    cJSON_AddItemToObject(root, "data", data);

    pthread_mutex_unlock(&room->playing_info.lock);

    const char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

// 获取当前播放进度，用于JSON广播
const char *get_cur_played_percent(rooms_t *room)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
    {
        return NULL;
    }
    cJSON *data = cJSON_CreateObject();
    if (!data)
    {
        cJSON_Delete(root);
        return NULL;
    }

    pthread_mutex_lock(&room->playing_info.lock);

    cJSON_AddNumberToObject(data, "played_percent", room->playing_info.played_percent);
    cJSON_AddNumberToObject(root, "error_code", SUCCESS);
    cJSON_AddStringToObject(root, "status", "success");
    cJSON_AddNumberToObject(root, "action", BROADCAST_SONG_INFO);
    cJSON_AddItemToObject(root, "data", data);

    pthread_mutex_unlock(&room->playing_info.lock);

    const char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    return json_str;
}

const char *get_cur_song_info(rooms_t *room, enum ctrl cmd)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return NULL;

    cJSON *data = cJSON_CreateObject();
    if (!data)
    {
        cJSON_Delete(root);
        return NULL;
    }

    pthread_mutex_lock(&room->playing_info.lock);

    cJSON_AddNumberToObject(root, "error_code", SUCCESS);
    cJSON_AddStringToObject(root, "status", "success");
    cJSON_AddNumberToObject(root, "action", cmd);
    cJSON_AddStringToObject(data, "songname", room->playing_info.song_name);
    cJSON_AddStringToObject(data, "songhash", room->playing_info.song_hash);
    cJSON_AddStringToObject(data, "singername", room->playing_info.singer_name);
    cJSON_AddStringToObject(data, "album_name", room->playing_info.album_name);
    cJSON_AddStringToObject(data, "duration", room->playing_info.duration);
    cJSON_AddStringToObject(data, "song_url", room->playing_info.song_url);
    cJSON_AddStringToObject(data, "cover_url", room->playing_info.cover_url);
    cJSON_AddNumberToObject(data, "played_percent", room->playing_info.played_percent);
    cJSON_AddNumberToObject(data, "is_playing", room->playing_info.is_playing);
    cJSON_AddItemToObject(root, "data", data);

    pthread_mutex_unlock(&room->playing_info.lock);

    const char *json_str = cJSON_PrintUnformatted(root);

    cJSON_Delete(root);
    return json_str;
}
int pause_song(client_info_t *client)
{
    if (!client)
        return -1;
    rooms_t *room = client->room;
    if (!room)
    {
        return -1;
    }
    pthread_mutex_lock(&room->playing_info.lock);
    room->playing_info.is_playing = 0;
    room->playing_info.last_update_time = time(NULL);
    pthread_mutex_unlock(&room->playing_info.lock);
    init_room_action(room, client->userId, client->nickname, client->avatar_url, PAUSE_SONG, "暂停播放");
    return 0;
}
int resume_song(client_info_t *client)
{
    if (!client)
        return -1;
    rooms_t *room = client->room;
    if (!room)
    {
        return -1;
    }
    pthread_mutex_lock(&room->playing_info.lock);
    room->playing_info.is_playing = 1;
    room->playing_info.last_update_time = time(NULL);
    pthread_mutex_unlock(&room->playing_info.lock);
    init_room_action(room, client->userId, client->nickname, client->avatar_url, RESUME_SONG, "继续播放");
    lws_sul_schedule(context, 0, &(room->playing_info).timer, timer_callback, 1 * LWS_US_PER_SEC);
    return 0;
}
// 获取当前房间播放列表
const char *get_playlist_json(rooms_t *room, enum ctrl cmd)
{
    playlist_t *curr = room->playlist_head->next;
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return NULL;
    // 创建一个json数组对象
    cJSON *playlist = cJSON_CreateArray();
    pthread_mutex_lock(&room->lock);
    while (curr)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "songname", curr->song_name);
        cJSON_AddStringToObject(item, "songhash", curr->song_hash);
        cJSON_AddStringToObject(item, "singername", curr->singer_name);
        cJSON_AddStringToObject(item, "album_name", curr->album_name);
        cJSON_AddStringToObject(item, "duration", curr->duration);
        cJSON_AddStringToObject(item, "cover_url", curr->cover_url);
        cJSON_AddStringToObject(item, "added_by_nickname", curr->added_by_nickname);
        cJSON_AddStringToObject(item, "added_by_avatar", curr->added_by_avatar);
        cJSON_AddItemToArray(playlist, item);
        curr = curr->next;
    }
    pthread_mutex_unlock(&room->lock);
    // 添加到json对象中
    cJSON_AddItemToObject(root, "playlist", playlist);
    cJSON_AddNumberToObject(root, "error_code", SUCCESS);
    cJSON_AddStringToObject(root, "status", "success");
    cJSON_AddNumberToObject(root, "action", cmd);
    const char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

// 获取播放列表和当前歌曲信息的合并JSON（用于添加歌曲后的广播）
const char *get_playlist_and_song_info_json(rooms_t *room)
{
    playlist_t *curr = room->playlist_head->next;
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return NULL;

    cJSON *playlist = cJSON_CreateArray();
    pthread_mutex_lock(&room->lock);
    while (curr)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "songname", curr->song_name);
        cJSON_AddStringToObject(item, "songhash", curr->song_hash);
        cJSON_AddStringToObject(item, "singername", curr->singer_name);
        cJSON_AddStringToObject(item, "album_name", curr->album_name);
        cJSON_AddStringToObject(item, "duration", curr->duration);
        cJSON_AddStringToObject(item, "cover_url", curr->cover_url);
        cJSON_AddStringToObject(item, "added_by_nickname", curr->added_by_nickname);
        cJSON_AddStringToObject(item, "added_by_avatar", curr->added_by_avatar);
        cJSON_AddItemToArray(playlist, item);
        curr = curr->next;
    }
    pthread_mutex_unlock(&room->lock);

    cJSON_AddItemToObject(root, "playlist", playlist);
    cJSON_AddNumberToObject(root, "error_code", SUCCESS);
    cJSON_AddStringToObject(root, "status", "success");
    cJSON_AddNumberToObject(root, "action", BROADCAST_SONG_LIST);

    // 如果有正在播放的歌曲，附加歌曲信息
    if (room->current_song)
    {
        cJSON *data = cJSON_CreateObject();
        pthread_mutex_lock(&room->playing_info.lock);
        cJSON_AddStringToObject(data, "songname", room->playing_info.song_name);
        cJSON_AddStringToObject(data, "songhash", room->playing_info.song_hash);
        cJSON_AddStringToObject(data, "singername", room->playing_info.singer_name);
        cJSON_AddStringToObject(data, "album_name", room->playing_info.album_name);
        cJSON_AddStringToObject(data, "duration", room->playing_info.duration);
        cJSON_AddStringToObject(data, "song_url", room->playing_info.song_url);
        cJSON_AddStringToObject(data, "cover_url", room->playing_info.cover_url);
        cJSON_AddNumberToObject(data, "played_percent", room->playing_info.played_percent);
        cJSON_AddNumberToObject(data, "is_playing", room->playing_info.is_playing);
        pthread_mutex_unlock(&room->playing_info.lock);
        cJSON_AddItemToObject(root, "song_info", data);
    }

    const char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

// 获取房间最近 N 条操作记录
const char *get_room_actions_json(rooms_t *room, int max_count)
{
    if (!room || !room->room_ctrl_head)
        return NULL;

    cJSON *root = cJSON_CreateObject();
    cJSON *actions = cJSON_CreateArray();

    pthread_mutex_lock(&room->lock);
    int count = 0;
    for (room_ctrl_t *cur = room->room_ctrl_head->next; cur && count < max_count; cur = cur->next, count++)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "userid", cur->userid);
        cJSON_AddStringToObject(item, "nickname", cur->nickname);
        cJSON_AddStringToObject(item, "avatar_url", cur->avatar_url);
        cJSON_AddNumberToObject(item, "action", cur->action);
        cJSON_AddStringToObject(item, "message", cur->action_message);
        cJSON_AddNumberToObject(item, "time", (double)cur->action_time);
        cJSON_AddItemToArray(actions, item);
    }
    pthread_mutex_unlock(&room->lock);

    cJSON_AddItemToObject(root, "actions", actions);
    cJSON_AddNumberToObject(root, "action", BROADCAST_ROOM_ACTION);
    cJSON_AddStringToObject(root, "status", "success");

    const char *json_str2 = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str2;
}
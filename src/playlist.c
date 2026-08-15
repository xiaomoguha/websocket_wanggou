#include <stdlib.h>
#include <time.h>
#include <ctype.h>
#include <string.h>
#include <libwebsockets.h>
#include "cJSON.h"
#include "playlist.h"
#include "websocket_service.h"
#include "rooms.h"
#include "utf8.h"

#define SERVICE_BASE_URL "http://127.0.0.1:3000"

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
    if (!response)
        return NULL;
    response->data = malloc(1);
    if (!response->data)
    {
        free(response);
        return NULL;
    }
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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);

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
        copy_utf8_bounded(song_url, url_obj->valuestring, 1024);
    }
    free(response->data);
    free(response);
    cJSON_Delete(root);
    return song_url;
}

// 获取该房间所有的客户端信息
const char *get_client_list_json(rooms_t *room, enum ctrl cmd)
{
    if (!room)
        return NULL;
    cJSON *root = cJSON_CreateObject();
    cJSON *client_list = cJSON_CreateArray();
    if (!root)
        return NULL;

    pthread_mutex_lock(&room->lock);
    client_info_t *client = room->client_info->next;
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

// 解析时长字符串（"mm:ss" 或纯秒数）为秒；非法/空返回 0
static int parse_duration_secs(const char *dur)
{
    if (!dur)
        return 0;
    const char *colon = strchr(dur, ':');
    if (colon)
        return atoi(dur) * 60 + atoi(colon + 1);
    return (int)atof(dur);
}

// UTF-8 百分号编码（保留字母数字与 -_.~）
static void url_encode_buf(const char *in, char *out, int out_len)
{
    static const char hex[] = "0123456789ABCDEF";
    int o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o < out_len - 4; p++)
    {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~')
            out[o++] = (char)*p;
        else
        {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 0xF];
        }
    }
    out[o] = '\0';
}

// 时长缺失时按 hash 现查真实时长：用「歌名 歌手」调 /search，
// 结果里按 hash 精确匹配（避免模糊命中 Live/伴奏版），取 duration(秒)。
// 查到返回 1 并写入 out("mm:ss")；失败返回 0（调用方走 240s 兜底）。
static int fetch_real_duration(const char *song_name, const char *singer_name,
                               const char *song_hash, char *out, int out_len)
{
    char keywords[384] = {0};
    char enc[1152] = {0};
    snprintf(keywords, sizeof(keywords), "%s %s",
             song_name ? song_name : "", singer_name ? singer_name : "");
    url_encode_buf(keywords, enc, sizeof(enc));

    char url[1600] = {0};
    snprintf(url, sizeof(url), "%s/search?keywords=%s&page=1&pagesize=30", SERVICE_BASE_URL, enc);

    struct ResponseData *resp = http_request(url, "GET", NULL, NULL);
    if (!resp || !resp->data || resp->size == 0)
    {
        if (resp)
        {
            free(resp->data);
            free(resp);
        }
        return 0;
    }
    cJSON *root = cJSON_Parse(resp->data);
    free(resp->data);
    free(resp);
    if (!root)
        return 0;

    int found = 0;
    cJSON *info = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "data"), "info");
    int n = cJSON_IsArray(info) ? cJSON_GetArraySize(info) : 0;
    for (int i = 0; i < n; i++)
    {
        cJSON *item = cJSON_GetArrayItem(info, i);
        cJSON *h = cJSON_GetObjectItem(item, "hash");
        if (!cJSON_IsString(h) || !h->valuestring || strcmp(h->valuestring, song_hash) != 0)
            continue;
        cJSON *d = cJSON_GetObjectItem(item, "duration");
        if (cJSON_IsNumber(d) && d->valueint > 0)
        {
            snprintf(out, out_len, "%02d:%02d", d->valueint / 60, d->valueint % 60);
            found = 1;
        }
        break;
    }
    cJSON_Delete(root);
    return found;
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

    // 时长缺失/非法（空串、"--:--"、"00:00"）：按 hash 现查一次补齐。
    // 仍匹配不到则直接拒收——没有时长的歌进列表会让进度推进/同步失真。
    char real_duration[16] = {0};
    if (parse_duration_secs(duration) <= 0)
    {
        if (!fetch_real_duration(song_name, singer_name, song_hash, real_duration, sizeof(real_duration)))
        {
            lwsl_notice("insert_song: 拒收无时长歌曲 %s (hash=%s 搜索未按 hash 匹配到)\n",
                        song_name, song_hash);
            return -3;
        }
        lwsl_notice("insert_song: 时长缺失已按 hash 回填 %s <- %s\n", real_duration, song_name);
        duration = real_duration;
    }

    playlist_t *new_song = (playlist_t *)malloc(sizeof(playlist_t));
    if (!new_song)
    {
        lwsl_err("Failed to allocate memory for playlist_t\n");
        return -1;
    }
    memset(new_song, 0, sizeof(playlist_t));
    copy_utf8_bounded(new_song->song_name, song_name, sizeof(new_song->song_name));
    copy_utf8_bounded(new_song->song_hash, song_hash, sizeof(new_song->song_hash));
    copy_utf8_bounded(new_song->singer_name, singer_name, sizeof(new_song->singer_name));
    copy_utf8_bounded(new_song->album_name, album_name, sizeof(new_song->album_name));
    copy_utf8_bounded(new_song->duration, duration, sizeof(new_song->duration));
    copy_utf8_bounded(new_song->cover_url, cover_url, sizeof(new_song->cover_url));
    copy_utf8_bounded(new_song->added_by_nickname, client->nickname, sizeof(new_song->added_by_nickname));
    copy_utf8_bounded(new_song->added_by_avatar, client->avatar_url, sizeof(new_song->added_by_avatar));
    new_song->is_system = 0;
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
    char message[256] = {0};
    snprintf(message, sizeof(message), "添加歌曲：%s", song_name);
    init_room_action(room, client->userId, client->nickname, client->avatar_url, ADD_SONG_TO_PLAYLIST, message);
    return 0;
}

// 系统推荐歌曲插入（无需 client_info_t）
int insert_system_song(rooms_t *room, const char *song_name, const char *song_hash,
                       const char *singer_name, const char *album_name,
                       const char *duration, const char *cover_url)
{
    if (!room || !song_name || !song_hash)
        return -1;

    playlist_t *new_song = (playlist_t *)malloc(sizeof(playlist_t));
    if (!new_song)
        return -1;
    memset(new_song, 0, sizeof(playlist_t));
    copy_utf8_bounded(new_song->song_name, song_name, sizeof(new_song->song_name));
    copy_utf8_bounded(new_song->song_hash, song_hash, sizeof(new_song->song_hash));
    copy_utf8_bounded(new_song->singer_name, singer_name, sizeof(new_song->singer_name));
    copy_utf8_bounded(new_song->album_name, album_name, sizeof(new_song->album_name));
    copy_utf8_bounded(new_song->duration, duration, sizeof(new_song->duration));
    copy_utf8_bounded(new_song->cover_url, cover_url, sizeof(new_song->cover_url));
    copy_utf8_bounded(new_song->added_by_nickname, "系统推荐", sizeof(new_song->added_by_nickname));
    new_song->is_system = 1;
    new_song->next = NULL;

    pthread_mutex_lock(&room->lock);
    room->playlist_tail->next = new_song;
    room->playlist_tail = new_song;

    // 如果是第一首歌曲，更新当前播放
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

    // 记录推荐 hash 用于去重
    int slot = room->recommended_count % 50;
    strncpy(room->recommended_hashes[slot], song_hash, sizeof(room->recommended_hashes[slot]) - 1);
    room->recommended_count++;

    char message[256] = {0};
    snprintf(message, sizeof(message), "推荐了歌曲：%s", song_name);
    init_room_action(room, "system", "系统", "", BROADCAST_ROOM_ACTION, message);

    return 0;
}

// 检查 hash 是否已推荐过
static int is_hash_recommended(rooms_t *room, const char *hash)
{
    int count = room->recommended_count < 50 ? room->recommended_count : 50;
    for (int i = 0; i < count; i++)
    {
        if (strcmp(room->recommended_hashes[i], hash) == 0)
            return 1;
    }
    return 0;
}

// 检查 hash 是否已在播放列表中
static int is_hash_in_playlist(rooms_t *room, const char *hash)
{
    for (playlist_t *cur = room->playlist_head->next; cur; cur = cur->next)
    {
        if (strcmp(cur->song_hash, hash) == 0)
            return 1;
    }
    return 0;
}

// 推荐源类型
enum recommend_source
{
    SRC_TOP_SONG = 0,
    SRC_EVERYDAY_STYLE,
    SRC_EVERYDAY_REC,
    SRC_AI_REC,
    SRC_PERSONAL_FM,
    SRC_COUNT
};

static const char *source_names[] = {"top_song", "everyday_style", "everyday_rec", "ai_rec", "personal_fm"};

// 从 cJSON 歌曲对象提取通用字段
static void extract_song_fields(cJSON *song, char *singer_name, int singer_len,
                                char *album_name, int album_len,
                                char *cover_url, int cover_len,
                                char *duration_str, int dur_len,
                                int *is_ms_duration)
{
    // 歌手名：优先 author_name，其次 authors[].author_name
    cJSON *author = cJSON_GetObjectItem(song, "author_name");
    if (cJSON_IsString(author) && strlen(author->valuestring) > 0)
    {
        copy_utf8_bounded(singer_name, author->valuestring, singer_len);
    }
    else
    {
        cJSON *authors = cJSON_GetObjectItem(song, "authors");
        if (cJSON_IsArray(authors) && cJSON_GetArraySize(authors) > 0)
        {
            cJSON *first = cJSON_GetArrayItem(authors, 0);
            cJSON *name = cJSON_GetObjectItem(first, "author_name");
            if (cJSON_IsString(name))
                copy_utf8_bounded(singer_name, name->valuestring, singer_len);
        }
    }

    // 专辑名
    cJSON *album = cJSON_GetObjectItem(song, "album_name");
    if (cJSON_IsString(album))
        copy_utf8_bounded(album_name, album->valuestring, album_len);

    // 封面：sizable_cover > trans_param.union_cover > album_sizable_cover
    cJSON *sizable = cJSON_GetObjectItem(song, "sizable_cover");
    if (cJSON_IsString(sizable) && strlen(sizable->valuestring) > 0)
    {
        copy_utf8_bounded(cover_url, sizable->valuestring, cover_len);
    }
    else
    {
        cJSON *tp = cJSON_GetObjectItem(song, "trans_param");
        if (tp)
        {
            cJSON *uc = cJSON_GetObjectItem(tp, "union_cover");
            if (cJSON_IsString(uc) && strlen(uc->valuestring) > 0)
                copy_utf8_bounded(cover_url, uc->valuestring, cover_len);
        }
    }
    if (strlen(cover_url) == 0)
    {
        cJSON *asc = cJSON_GetObjectItem(song, "album_sizable_cover");
        if (cJSON_IsString(asc))
            copy_utf8_bounded(cover_url, asc->valuestring, cover_len);
    }

    // 替换 {size} 占位符为实际尺寸（"400" 比 "{size}" 短，结果必不长于原串）
    if (strlen(cover_url) > 0)
    {
        char *pos = strstr(cover_url, "{size}");
        if (pos)
        {
            int prefix_len = (int)(pos - cover_url);
            int suffix_len = (int)strlen(pos + 6); // 跳过 "{size}"
            int needed = prefix_len + 3 /*"400"*/ + suffix_len;
            char *tmp = (char *)malloc(needed + 1);
            if (tmp)
            {
                snprintf(tmp, needed + 1, "%.*s400%s", prefix_len, cover_url, pos + 6);
                copy_utf8_bounded(cover_url, tmp, cover_len);
                free(tmp);
            }
        }
    }

    // 时长：time_length（秒）或 timelength（毫秒）
    *is_ms_duration = 0;
    cJSON *tl = cJSON_GetObjectItem(song, "time_length");
    if (tl && tl->valueint > 0)
    {
        int total_sec = tl->valueint;
        snprintf(duration_str, dur_len, "%02d:%02d", total_sec / 60, total_sec % 60);
    }
    else
    {
        cJSON *tms = cJSON_GetObjectItem(song, "timelength");
        if (tms && tms->valueint > 0)
        {
            *is_ms_duration = 1;
            int total_sec = tms->valueint / 1000;
            snprintf(duration_str, dur_len, "%02d:%02d", total_sec / 60, total_sec % 60);
        }
    }
}

// 从指定推荐源获取歌曲列表 JSON 数组
static cJSON *fetch_source_songs(enum recommend_source src, cJSON **root_out)
{
    char url[512] = {0};
    const char *method = "GET";
    const char *post_data = NULL;

    switch (src)
    {
    case SRC_TOP_SONG:
        snprintf(url, sizeof(url), "%s/top/song?page=1&pagesize=30", SERVICE_BASE_URL);
        method = "POST";
        post_data = "{}";
        break;
    case SRC_EVERYDAY_STYLE:
        snprintf(url, sizeof(url), "%s/everyday/style/recommend", SERVICE_BASE_URL);
        break;
    case SRC_EVERYDAY_REC:
        snprintf(url, sizeof(url), "%s/everyday/recommend", SERVICE_BASE_URL);
        break;
    case SRC_AI_REC:
        snprintf(url, sizeof(url), "%s/ai/recommend", SERVICE_BASE_URL);
        break;
    case SRC_PERSONAL_FM:
        snprintf(url, sizeof(url), "%s/personal/fm", SERVICE_BASE_URL);
        break;
    default:
        return NULL;
    }

    struct ResponseData *response = http_request(url, method, post_data, NULL);
    if (!response || !response->data || response->size == 0)
    {
        lwsl_err("auto_recommend: source=%s HTTP请求失败\n", source_names[src]);
        if (response)
        {
            free(response->data);
            free(response);
        }
        return NULL;
    }

    cJSON *root = cJSON_Parse(response->data);
    free(response->data);
    free(response);
    if (!root)
    {
        lwsl_err("auto_recommend: source=%s JSON解析失败\n", source_names[src]);
        return NULL;
    }
    *root_out = root;

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (!data)
        return NULL;

    // data 可能是数组（top/song）或对象含 song_list
    if (cJSON_IsArray(data))
        return data;

    cJSON *song_list = cJSON_GetObjectItem(data, "song_list");
    if (cJSON_IsArray(song_list))
        return song_list;

    return NULL;
}

// 从API获取推荐歌曲并添加到播放列表（多源混用，每次随机选一个源）
int auto_recommend_song(rooms_t *room)
{
    if (!room)
        return -1;

    // 随机选一个源（单次请求，避免阻塞太久）
    enum recommend_source src = rand() % SRC_COUNT;
    cJSON *root = NULL;
    cJSON *songs = fetch_source_songs(src, &root);

    if (!songs || cJSON_GetArraySize(songs) == 0)
    {
        lwsl_notice("auto_recommend: source=%s 无歌曲\n", source_names[src]);
        if (root)
            cJSON_Delete(root);
        return -1;
    }

    int total = cJSON_GetArraySize(songs);
    int candidates[64];
    int candidate_count = 0;

    for (int i = 0; i < total && i < 64; i++)
    {
        cJSON *item = cJSON_GetArrayItem(songs, i);
        cJSON *hash_obj = cJSON_GetObjectItem(item, "hash");
        if (!cJSON_IsString(hash_obj))
            continue;
        if (!is_hash_recommended(room, hash_obj->valuestring) &&
            !is_hash_in_playlist(room, hash_obj->valuestring))
        {
            candidates[candidate_count++] = i;
        }
    }

    if (candidate_count == 0)
    {
        for (int i = 0; i < total && i < 64; i++)
        {
            cJSON *item = cJSON_GetArrayItem(songs, i);
            cJSON *hash_obj = cJSON_GetObjectItem(item, "hash");
            if (!cJSON_IsString(hash_obj))
                continue;
            if (!is_hash_in_playlist(room, hash_obj->valuestring))
                candidates[candidate_count++] = i;
        }
    }

    if (candidate_count == 0)
    {
        cJSON_Delete(root);
        lwsl_notice("auto_recommend: source=%s 无可选歌曲\n", source_names[src]);
        return -1;
    }

    int chosen = candidates[rand() % candidate_count];
    cJSON *song = cJSON_GetArrayItem(songs, chosen);

    const char *songname = cJSON_GetObjectItem(song, "songname") ? cJSON_GetObjectItem(song, "songname")->valuestring : "未知";
    const char *hash = cJSON_GetObjectItem(song, "hash") ? cJSON_GetObjectItem(song, "hash")->valuestring : "";

    char singer_name[128] = "未知歌手";
    char album_name[128] = "";
    char cover_url[512] = "";
    char duration_str[16] = "00:00";
    int is_ms = 0;
    extract_song_fields(song, singer_name, sizeof(singer_name),
                       album_name, sizeof(album_name),
                       cover_url, sizeof(cover_url),
                       duration_str, sizeof(duration_str), &is_ms);

    lwsl_notice("auto_recommend: source=%s 推荐: %s - %s\n", source_names[src], songname, singer_name);

    int result = insert_system_song(room, songname, hash, singer_name, album_name, duration_str, cover_url);
    cJSON_Delete(root);
    return result;
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

    // 先在锁外做HTTP请求（避免持锁阻塞整个服务器）
    char *song_url = get_song_url(curr->song_hash);

    pthread_mutex_lock(&playing_info->lock);

    copy_utf8_bounded(playing_info->song_name, curr->song_name, sizeof(playing_info->song_name));
    copy_utf8_bounded(playing_info->song_hash, curr->song_hash, sizeof(playing_info->song_hash));
    copy_utf8_bounded(playing_info->singer_name, curr->singer_name, sizeof(playing_info->singer_name));
    copy_utf8_bounded(playing_info->album_name, curr->album_name, sizeof(playing_info->album_name));
    copy_utf8_bounded(playing_info->duration, curr->duration, sizeof(playing_info->duration));
    copy_utf8_bounded(playing_info->cover_url, curr->cover_url, sizeof(playing_info->cover_url));
    if (song_url)
    {
        copy_utf8_bounded(playing_info->song_url, song_url, sizeof(playing_info->song_url));
        free(song_url);
    }
    playing_info->played_percent = 0; // 重置播放进度
    playing_info->is_playing = 1;     // 设置为正在播放
    playing_info->last_update_time = time(NULL);
    lws_sul_schedule(context, 0, &playing_info->timer, timer_callback, 1 * LWS_US_PER_SEC);
    pthread_mutex_unlock(&playing_info->lock);

    return 0;
}

// 系统播放下一首
int play_next_song_bysystem(rooms_t *room)
{
    if (!room)
        return -1;
    if (!room->current_song)
    {
        auto_recommend_song(room);
        return 0;
    }

    playlist_t *finished = room->current_song;

    pthread_mutex_lock(&room->lock);

    // 播完后移除
    playlist_t *next_after = finished->next;
    playlist_t *prev = room->playlist_head;
    while (prev && prev->next != finished)
        prev = prev->next;
    if (prev)
    {
        prev->next = next_after;
        if (room->playlist_tail == finished)
            room->playlist_tail = prev;
    }
    free(finished);

    // 有下一首就播，没有就推荐
    if (next_after)
    {
        room->current_song = next_after;
        pthread_mutex_unlock(&room->lock);
        update_playing_info(room);
        return 0;
    }

    room->current_song = NULL;
    pthread_mutex_unlock(&room->lock);
    auto_recommend_song(room);
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

    playlist_t *finished = room->current_song;

    pthread_mutex_lock(&room->lock);

    // 播完后移除
    playlist_t *next_after = finished->next;
    playlist_t *prev = room->playlist_head;
    while (prev && prev->next != finished)
        prev = prev->next;
    if (prev)
    {
        prev->next = next_after;
        if (room->playlist_tail == finished)
            room->playlist_tail = prev;
    }
    free(finished);

    // 有下一首就播，没有就推荐
    if (next_after)
    {
        room->current_song = next_after;
        char message[1024] = {0};
        snprintf(message, sizeof(message), "播放了下一首:%s", room->current_song->song_name);
        init_room_action(room, client->userId, client->nickname, client->avatar_url, PLAY_BY_SONG_HASH, message);
        pthread_mutex_unlock(&room->lock);
        update_playing_info(room);
        return 0;
    }

    room->current_song = NULL;
    pthread_mutex_unlock(&room->lock);
    auto_recommend_song(room);
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
            // 找到歌曲，进行置顶操作（插入到当前播放歌曲后面，使其成为下一首）
            prev->next = curr->next;
            if (curr == room->playlist_tail)
            {
                room->playlist_tail = prev;
            }
            if (room->current_song && room->current_song != curr)
            {
                // 插入到当前播放歌曲之后
                curr->next = room->current_song->next;
                room->current_song->next = curr;
                if (room->current_song == room->playlist_tail)
                    room->playlist_tail = curr;
            }
            else if (!room->current_song)
            {
                // 没有在播放的歌曲，插到最前面
                curr->next = room->playlist_head->next;
                room->playlist_head->next = curr;
            }
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
    if (!room)
        return NULL;
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return NULL;
    // 创建一个json数组对象
    cJSON *playlist = cJSON_CreateArray();
    playlist_t *curr = room->playlist_head->next;
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
    if (!room)
        return NULL;
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return NULL;

    cJSON *playlist = cJSON_CreateArray();
    playlist_t *curr = room->playlist_head->next;
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
        cJSON_AddStringToObject(item, "msg_type", cur->action == SEND_CHAT ? "chat" : "action");
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
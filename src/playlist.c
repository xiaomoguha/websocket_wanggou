#include <stdlib.h>
#include <libwebsockets.h>
#include "cJSON.h"
#include "playlist.h"
#include "websocket_service.h"

#define SERVICE_IP_ADDRESS "47.112.6.94"
#define SERVICE_PORT 3000

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
        lwsl_err("Failed to perform HTTP request: %s--:%s", url,curl_easy_strerror(res));
        free(response->data);
        free(response);
        response = NULL;
    }

    curl_easy_cleanup(curl);
    return response;
}

//获取歌词 url
char *get_lyrics_url(const char *song_hash)
{
    struct ResponseData *response;
    char url[256] = {0};  
    char *lyrics_url = (char *)malloc(256);
    memset(lyrics_url, 0, 256);
    if (!song_hash)
    {
        free(lyrics_url);
        return NULL;
    }

    //拼接url
    snprintf(url, sizeof(url), "http://%s:%d/search/lyric?hash=%s", SERVICE_IP_ADDRESS, SERVICE_PORT, song_hash);
    response = http_request(url, "GET", NULL, NULL);
    if(!response)
    {
        free(lyrics_url);
        return "";
    }
    //开始解析接收到的 json 数据，拼接为最终的歌词 url
    cJSON *root = cJSON_Parse(response->data);
    if (!root)
    {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL)
        {
            char msg[128] = {0};
            sprintf(msg, "JSON 解析错误:%s", error_ptr);
            lwsl_err("JSON 解析错误: %s\n", error_ptr);
            cJSON_Delete(root);
            free(response->data);
            free(response);
            free(lyrics_url);
            return "";
        }
        return "";
    }
    cJSON *candidates = cJSON_GetObjectItem(root, "candidates");
    if(!cJSON_IsArray(candidates))
    {
        lwsl_err("JSON 解析错误");
        cJSON_Delete(root);
        free(response->data);
        free(response);
        free(lyrics_url);
        return "";
    }
    cJSON *candidate = cJSON_GetArrayItem(candidates, 0);
    cJSON *id        = cJSON_GetObjectItem(candidate, "id");
    cJSON *accesskey = cJSON_GetObjectItem(candidate, "accesskey");
    //拼接歌词 url
    snprintf(lyrics_url, 255, "http://%s:%d/lyric?id=%s&accesskey=%s&decode=true&fmt=lrc", SERVICE_IP_ADDRESS, SERVICE_PORT, id->valuestring, accesskey->valuestring);
    free(response->data);
    free(response);
    cJSON_Delete(root);
    return lyrics_url;
}

//获取歌曲 url
char *get_song_url(const char *song_hash)
{
    struct ResponseData *response;
    char url[256] = {0};
    char *song_url = (char *)malloc(256);
    memset(song_url, 0, 256);
    if (!song_hash)
    {
        free(song_url);
        return NULL;
    }
    //拼接url
    snprintf(url, sizeof(url), "http://%s:%d/song/url?hash=%s", SERVICE_IP_ADDRESS, SERVICE_PORT, song_hash);
    response = http_request(url, "GET", NULL, NULL);
    if(!response)
    {
        free(song_url);
        return "";
    }
    cJSON *root = cJSON_Parse(response->data);
    if (!root)
    {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL)
        {
            char msg[128] = {0};
            sprintf(msg, "JSON 解析错误:%s", error_ptr);
            lwsl_err("JSON 解析错误: %s\n", error_ptr);
            cJSON_Delete(root);
            free(response->data);
            free(response);
            free(song_url);
            return "";
        }
        return "";
    }
    cJSON *urls = cJSON_GetObjectItem(root, "url");
    if(!cJSON_IsArray(urls))
    {
        lwsl_err("JSON 解析错误");
        cJSON_Delete(root);
        free(response->data);
        free(response);
        free(song_url);
        return "";
    }
    cJSON *url_obj = cJSON_GetArrayItem(urls, 0);
    strncpy(song_url, url_obj->valuestring, 255);
    free(response->data);
    free(response);
    cJSON_Delete(root);
    return song_url;
}

int insert_song_to_playlist(rooms_t *room, const char *song_name, const char *song_hash,
                            const char *singer_name, const char *album_name,
                            const char *duration,const char *cover_url)
{
    if (!room || !song_name || !song_hash)
    {
        return -1;
    }

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
    new_song->next = NULL;

    char *lyrics_url = get_lyrics_url(song_hash);
    //获取歌词 url 并且填充进去
    strncpy(new_song->lyrics_url, lyrics_url ,sizeof(new_song->lyrics_url));
    free(lyrics_url);

    // 插入到播放列表末尾
    playlist_t *tail = room->playlist_tail;
    tail->next = new_song;
    room->playlist_tail = new_song;

    //如果是第一首歌曲，则更新当前歌曲信息
    if (room->current_song == NULL) 
    {
        room->current_song = new_song;
        update_playing_info(room);
    }

    return 0;
}

int remove_song_from_playlist(rooms_t *room, const char *song_hash)
{
    if (!room || !song_hash)
    {
        return -1;
    }

    playlist_t *prev = room->playlist_head;
    playlist_t *curr = prev->next;

    while (curr)
    {
        if (strcmp(curr->song_hash, song_hash) == 0)
        {
            prev->next = curr->next;
            if (curr == room->playlist_tail)
            {
                room->playlist_tail = prev;
            }
            free(curr);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }

    return -1; // 未找到歌曲
}
//切换正在播放的歌曲信息
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
    strncpy(playing_info->lyrics_url, curr->lyrics_url, sizeof(playing_info->lyrics_url) - 1);
    strncpy(playing_info->cover_url, curr->cover_url, sizeof(playing_info->cover_url) - 1);
    // 获取歌曲 url 填充进去
    char *song_url = get_song_url(curr->song_hash);
    strncpy(playing_info->song_url, song_url, sizeof(playing_info->song_url));
    free(song_url);
    playing_info->played_percent = 0; // 重置播放进度
    playing_info->is_playing = 1;     // 设置为正在播放
    playing_info->start_time = time(NULL);
    playing_info->last_update_time = playing_info->start_time;
    lws_sul_schedule(context, 0, &playing_info->timer, timer_callback, 1 * LWS_US_PER_SEC);
    pthread_mutex_unlock(&playing_info->lock);

    return 0;
}
int play_next_song(rooms_t *room)
{
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
    pthread_mutex_unlock(&room->lock);
    update_playing_info(room);
    return 0;
}
int playbysonghash(rooms_t *room, const char *song_hash)
{
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
            update_playing_info(room);
            return 0;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&room->lock);

    return -1; // 未找到歌曲
}
//将歌曲置顶
int upsongbyhash(rooms_t *room, const char *song_hash)
{
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
            // 找到歌曲，进行置顶操作
            prev->next = curr->next;
            if (curr == room->playlist_tail)
            {
                room->playlist_tail = prev;
            }
            // 插入到头节点后面
            curr->next = room->playlist_head->next;
            room->playlist_head->next = curr;
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }
    pthread_mutex_unlock(&room->lock);

    return -1; // 未找到歌曲
}

const char* get_cur_song_info(rooms_t *room,enum ctrl cmd)
{
    playing_info_t playing = room->playing_info;

    cJSON *root = cJSON_CreateObject();
    if (!root)
        return NULL;

    cJSON *data = cJSON_CreateObject();
    if (!data)
        return NULL;

    pthread_mutex_lock(&playing.lock);

    cJSON_AddNumberToObject(root, "error_code", SUCCESS);
    cJSON_AddStringToObject(root, "status", "success");
    cJSON_AddNumberToObject(root, "action", cmd);
    cJSON_AddStringToObject(data, "songname", playing.song_name);
    cJSON_AddStringToObject(data, "songhash", playing.song_hash);
    cJSON_AddStringToObject(data, "singername", playing.singer_name);
    cJSON_AddStringToObject(data, "album_name", playing.album_name);
    cJSON_AddStringToObject(data, "duration", playing.duration);
    cJSON_AddStringToObject(data, "lyrics_url", playing.lyrics_url);
    cJSON_AddStringToObject(data, "song_url", playing.song_url);
    cJSON_AddStringToObject(data, "cover_url", playing.cover_url);
    cJSON_AddNumberToObject(data, "played_percent", playing.played_percent);
    cJSON_AddNumberToObject(data, "is_playing", playing.is_playing);
    cJSON_AddItemToObject(root, "data", data);

    pthread_mutex_unlock(&playing.lock);

    const char *json_str = cJSON_PrintUnformatted(root);

    cJSON_Delete(root);
    return json_str;
}
int pause_song(rooms_t *room)
{
    if (!room)
    {
        return -1;
    }
    pthread_mutex_lock(&room->playing_info.lock);
    room->playing_info.is_playing = 0;
    room->playing_info.last_update_time = time(NULL);
    pthread_mutex_unlock(&room->playing_info.lock);
    return 0;

}
int resume_song(rooms_t *room)
{
    if (!room)
    {
        return -1;
    }
    pthread_mutex_lock(&room->playing_info.lock);
    room->playing_info.is_playing = 1;
    room->playing_info.last_update_time = time(NULL);
    pthread_mutex_unlock(&room->playing_info.lock);
    lws_sul_schedule(context, 0, &(room->playing_info).timer, timer_callback, 1 * LWS_US_PER_SEC);
    return 0;
}
//获取当前房间播放列表
const char *get_playlist_json(rooms_t *room,enum ctrl cmd)
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
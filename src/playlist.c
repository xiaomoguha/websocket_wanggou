#include <stdlib.h>
#include <libwebsockets.h>
#include "cJSON.h"
#include "playlist.h"
#include "websocket_service.h"
extern struct lws_context *context;
int insert_song_to_playlist(rooms_t *room, const char *song_name, const char *song_hash,
                            const char *singer_name, const char *album_name,
                            const char *duration, const char *lyrics,
                            const char *cover_url)
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
    strncpy(new_song->lyrics_url, lyrics, sizeof(new_song->lyrics_url) - 1);
    strncpy(new_song->cover_url, cover_url, sizeof(new_song->cover_url) - 1);
    new_song->next = NULL;

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

    strncpy(playing_info->song_name, curr->song_name, sizeof(playing_info->song_name) - 1);
    strncpy(playing_info->song_hash, curr->song_hash, sizeof(playing_info->song_hash) - 1);
    strncpy(playing_info->singer_name, curr->singer_name, sizeof(playing_info->singer_name) - 1);
    strncpy(playing_info->album_name, curr->album_name, sizeof(playing_info->album_name) - 1);
    strncpy(playing_info->duration, curr->duration, sizeof(playing_info->duration) - 1);
    strncpy(playing_info->lyrics_url, curr->lyrics_url, sizeof(playing_info->lyrics_url) - 1);
    strncpy(playing_info->cover_url, curr->cover_url, sizeof(playing_info->cover_url) - 1);
    playing_info->played_percent = 0; // 重置播放进度
    playing_info->is_playing = 1;     // 设置为正在播放
    playing_info->start_time = time(NULL);
    playing_info->last_update_time = playing_info->start_time;
    lws_sul_schedule(context, 0, &playing_info->timer, timer_callback, 1 * LWS_US_PER_SEC);

    return 0;
}
int play_next_song(rooms_t *room)
{
    if (!room || !room->current_song)
    {
        return -1;
    }

    room->current_song = room->current_song->next;
    if (!room->current_song)
    {
        // 播放列表结束，重置为头节点
        room->current_song = room->playlist_head->next;
    }
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
    pthread_mutex_lock(&room->lock);
    room->playing_info.is_playing = 0;
    pthread_mutex_unlock(&room->lock);
    return 0;

}
int resume_song(rooms_t *room)
{
    if (!room)
    {
        return -1;
    }
    pthread_mutex_lock(&room->lock);
    room->playing_info.is_playing = 1;
    pthread_mutex_unlock(&room->lock);
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
    // 添加到json对象中
    cJSON_AddItemToObject(root, "playlist", playlist);
    cJSON_AddNumberToObject(root, "error_code", SUCCESS);
    cJSON_AddStringToObject(root, "status", "success");
    cJSON_AddNumberToObject(root, "action", cmd);
    const char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}
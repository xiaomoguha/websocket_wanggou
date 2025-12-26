#ifndef TYPES_H
#define TYPES_H
#include <arpa/inet.h>
#include <libwebsockets.h>
#include <time.h>
// 歌曲信息
typedef struct playlist
{
    char song_name[128];
    char song_hash[128];
    char singer_name[128];
    char album_name[128];
    char duration[16];
    char lyrics_url[256];
    char cover_url[256];
    struct playlist *next;
} playlist_t;
// 正在播放的歌曲信息
typedef struct playing_info
{
    lws_sorted_usec_list_t timer;
    char song_name[128];
    char song_hash[128];
    char song_url[256];
    char singer_name[128];
    char album_name[128];
    char duration[16];
    char lyrics_url[256];
    char cover_url[256];
    double played_percent;
    char is_playing;
    time_t start_time;
    time_t last_update_time;
    struct rooms *room;
    pthread_mutex_t lock;
} playing_info_t;
// 客户端信息
typedef struct client_info
{
    struct lws *wsi;
    char ip[INET_ADDRSTRLEN];
    struct rooms *room; // 对应房间节点
    char userId[64];
    char latest_msg[1024]; // 服务器单独回复信息
    char is_data_to_send;  // 是否有数据需要发送
    struct client_info *next;
    struct client_info *prev;
    pthread_mutex_t lock;
} client_info_t;
// 房间操作信息
typedef struct room_ctrl
{
    char userid[64];
    char action;
    char action_message[512];
    time_t action_time;
    struct room_ctrl *next;
} room_ctrl_t;
// 房间信息
typedef struct rooms
{
    char room_id[64];
    char creater_id[64];
    unsigned int client_counter;
    client_info_t *client_info;
    char latest_msg[1024];
    pthread_mutex_t lock;
    playlist_t *playlist_head;
    playlist_t *playlist_tail;
    playlist_t *current_song;
    room_ctrl_t *room_ctrl_head;
    playing_info_t playing_info;
    struct rooms *next;
} rooms_t;
// 操作枚举
enum ctrl
{
    GET_CUR_SONG_INFO = 200,
    PLAY_NEXT_SONG,
    PLAY_BY_SONG_HASH,
    PAUSE_SONG,
    RESUME_SONG,
    ADD_SONG_TO_PLAYLIST,
    REMOVE_SONG_FROM_PLAYLIST,
    UP_SONGBYHASH,
    GET_PLAYLIST,
    BROADCAST_SONG_INFO,
    BROADCAST_SONG_LIST,
    BROADCAST_CLIENT_LIST,
    GET_CLEIENT_LIST,
};

enum CODE
{
    SUCCESS,
    FAIL
};
#endif // TYPES_H
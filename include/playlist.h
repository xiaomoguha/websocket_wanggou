#ifndef PLAYLIST_H
#define PLAYLIST_H
#include "types.h"
#include "cJSON.h"
#include <curl/curl.h>

int insert_song_to_playlist(client_info_t *client, const char *song_name, const char *song_hash,
                            const char *singer_name, const char *album_name,
                            const char *duration, const char *cover_url);

// 批量加歌：songs_json 为数组，每项 {songname, songhash, singername, albumname, duration, coverurl}。
// 一条消息加一批（客户端「添加到一起听」）：只锁一次、只写一条操作日志、只广播一次。
// 房间"未播放"歌曲上限 MAX_PENDING_SONGS 首，超出部分丢弃。
// 返回值：本次实际加入的数量；出参统计丢弃/重复/失败数量（可为 NULL）
#define MAX_PENDING_SONGS 300
// 一条批量消息最多处理多少首（防单条超大消息）；超出部分按丢弃计
#define MAX_BATCH_ADD_SONGS 500
// 一批里最多允许几次"缺时长回查上游"（每次是一次 HTTP），超出按失败计
#define MAX_BATCH_DURATION_FETCH 20
int insert_songs_to_playlist(client_info_t *client, const cJSON *songs_json,
                             int *out_added, int *out_dropped, int *out_duplicate, int *out_failed);
// 房间未播放歌曲数（current_song 之后的节点数；空房为整条链长度）
int count_pending_songs(rooms_t *room);
int remove_song_from_playlist(client_info_t *client, const char *song_hash);
// 点赞/取消点赞（房间内共享，同一用户对同一首歌只记一次）
int toggle_song_like(client_info_t *client, const char *song_hash);
int update_playing_info(rooms_t *room);
int play_next_song(client_info_t *client);
int playbysonghash(client_info_t *client, const char *song_hash);
const char *get_cur_song_info(rooms_t *room, enum ctrl cmd);
const char *get_cur_song_progress(rooms_t *room);
int pause_song(client_info_t *client);
int resume_song(client_info_t *client);
const char *get_playlist_json(rooms_t *room, enum ctrl cmd);
const char *get_playlist_and_song_info_json(rooms_t *room);
int upsongbyhash(client_info_t *client, const char *song_hash);
const char *get_client_list_json(rooms_t *room, enum ctrl cmd);
int play_next_song_bysystem(rooms_t *room);
const char *get_room_actions_json(rooms_t *room, int max_count);
int auto_recommend_song(rooms_t *room);
int insert_system_song(rooms_t *room, const char *song_name, const char *song_hash,
                       const char *singer_name, const char *album_name,
                       const char *duration, const char *cover_url);
#endif // PLAYLIST_H
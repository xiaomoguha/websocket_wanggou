#ifndef PLAYLIST_H
#define PLAYLIST_H
#include "types.h"
#include <curl/curl.h>

int insert_song_to_playlist(client_info_t *client, const char *song_name, const char *song_hash,
                            const char *singer_name, const char *album_name,
                            const char *duration, const char *cover_url);
int remove_song_from_playlist(client_info_t *client, const char *song_hash);
int update_playing_info(rooms_t *room);
int play_next_song(client_info_t *client);
int playbysonghash(client_info_t *client, const char *song_hash);
const char *get_cur_song_info(rooms_t *room, enum ctrl cmd);
int pause_song(client_info_t *client);
int resume_song(client_info_t *client);
const char *get_playlist_json(rooms_t *room, enum ctrl cmd);
int upsongbyhash(client_info_t *client, const char *song_hash);
const char *get_cur_played_percent(rooms_t *room);
const char *get_client_list_json(rooms_t *room, enum ctrl cmd);
int play_next_song_bysystem(rooms_t *room);
#endif // PLAYLIST_H
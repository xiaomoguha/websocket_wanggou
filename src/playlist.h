#ifndef PLAYLIST_H
#define PLAYLIST_H
#include "types.h"


int insert_song_to_playlist(rooms_t *room, const char *song_name, const char *song_hash,
                            const char *singer_name, const char *album_name,
                            const char *duration, const char *lyrics,
                            const char *cover_url);
int remove_song_from_playlist(rooms_t *room, const char *song_hash);
int update_playing_info(rooms_t *room);
int play_next_song(rooms_t *room);
int playbysonghash(rooms_t *room, const char *song_hash);
const char *get_cur_song_info(rooms_t *room, enum ctrl cmd);
int pause_song(rooms_t *room);
int resume_song(rooms_t *room);
const char *get_playlist_json(rooms_t *room, enum ctrl cmd);
int upsongbyhash(rooms_t *room, const char *song_hash);
#endif // PLAYLIST_H
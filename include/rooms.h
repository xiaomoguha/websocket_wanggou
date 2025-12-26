#ifndef ROOMS_H
#define ROOMS_H
#include "cJSON.h"
#include "types.h"
#include <stdbool.h>
rooms_t *init_rooms();
rooms_t *insert_room_info(const char *room_id, const char *creater_id, rooms_t *head);
void remove_room_node(rooms_t *head, rooms_t *node);
bool init_room_action(rooms_t *room, char *userid, char action, char *action_message);

#endif // ROOMS_H

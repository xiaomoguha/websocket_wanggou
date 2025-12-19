#include "rooms.h"
#include <stdlib.h>
#include <string.h>
#include <libwebsockets.h>


// 带头结点的房间链表初始化
rooms_t *init_rooms()
{
    rooms_t *room = (rooms_t *)malloc(sizeof(rooms_t));
    if (!room)
    {
        lwsl_err("Failed to allocate memory for rooms\n");
        return NULL;
    }
    memset(room, 0, sizeof(room));
    strcpy(room->room_id, "head");
    room->client_counter = 0;
    room->client_info = NULL;
    pthread_mutex_init(&room->lock, NULL);
    room->next = NULL;
    room->playlist_head = NULL;
    return room;
}
// 头插法插入房间节点
bool insert_room_node(rooms_t *head, rooms_t *new_node)
{
    if (head == NULL || new_node == NULL)
    {
        return false;
    }
    new_node->next = head->next;
    head->next = new_node;
    return true;
}
//新建房间节点插入房间链表,返回该房间节点
rooms_t *insert_room_info(const char *room_id, const char *creater_id, rooms_t *head)
{
    rooms_t *new_node = (rooms_t *)malloc(sizeof(rooms_t));
    if (!new_node)
    {
        lwsl_err("Failed to allocate memory for rooms\n");
        return false;
    }
    memset(new_node, 0, sizeof(rooms_t));
    strncpy(new_node->room_id, room_id, 63);
    strncpy(new_node->creater_id, creater_id, 63);
    new_node->client_counter = 0;
    new_node->client_info = (client_info_t *)malloc(sizeof(client_info_t));//初始化客户端链表头节点
    if (!new_node->client_info)
    {
        lwsl_err("Failed to allocate memory for client_info_t\n");
        free(new_node);
        return NULL;
    }
    memset(new_node->client_info, 0, sizeof(client_info_t));
    new_node->client_info->next = NULL;
    new_node->client_info->prev = NULL;
    new_node->playlist_head = (playlist_t *)malloc(sizeof(playlist_t));//初始化播放列表头节点
    if (!new_node->playlist_head)
    {
        lwsl_err("Failed to allocate memory for playlist_t\n");
        free(new_node->client_info);
        free(new_node);
        return NULL;
    }
    memset(new_node->playlist_head, 0, sizeof(playlist_t));
    new_node->playlist_head->next = NULL;
    new_node->playlist_tail = new_node->playlist_head; // 初始化尾节点指向头节点
    new_node->current_song = new_node->playlist_head->next; // 初始化当前播放歌曲指向头节点
    pthread_mutex_init(&new_node->lock, NULL);
    pthread_mutex_init(&new_node->playing_info.lock, NULL);
    new_node->playing_info.room = new_node;
    new_node->next = NULL;
    if(!insert_room_node(head, new_node))
    {
        lwsl_err("Failed to insert room node\n");
        free(new_node);
        return NULL;
    }
    return new_node;
}
//移除对应room节点
void remove_room_node(rooms_t *head, rooms_t *node)
{
    //取消该房间的定时器
    lws_sul_cancel(&node->playing_info.timer);
    // 先释放播放列表链表
    playlist_t *cur = node->playlist_head;
    while (cur != NULL)
    {
        playlist_t *next = cur->next;
        free(cur);
        cur = next;
    }
    //再删除节点
    rooms_t *foreach_cur = head->next;
    rooms_t *prev = head;
    while (foreach_cur != NULL)
    {
        if (foreach_cur == node)
        {
            prev->next = foreach_cur->next;
            free(foreach_cur);
            break;
        }
        else
        {
            prev = foreach_cur;
            foreach_cur = foreach_cur->next;
        }
    }
}
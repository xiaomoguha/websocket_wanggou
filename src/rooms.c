#include "rooms.h"
#include <stdlib.h>
#include <string.h>
#include <libwebsockets.h>
#include "utf8.h"

// 初始化房间操作链表（带头结点）
room_ctrl_t *init_action_list()
{
    room_ctrl_t *head = (room_ctrl_t *)malloc(sizeof(room_ctrl_t));
    if (!head)
    {
        lwsl_err("Failed to allocate memory for room_ctrl_t\n");
        return NULL;
    }
    memset(head, 0, sizeof(room_ctrl_t));
    return head;
}
// 头插法插入房间操作节点
bool insert_room_action(rooms_t *room, room_ctrl_t *new_node)
{
    if (room == NULL || new_node == NULL)
    {
        return false;
    }
    room_ctrl_t *head = room->room_ctrl_head;
    new_node->next = head->next;
    head->next = new_node;
    return true;
}

#define MAX_ROOM_ACTIONS 100

// 新建操作节点并插入链表中
bool init_room_action(rooms_t *room, char *userid, char *nickname, char *avatar_url, int action, char *action_message)
{
    if (room == NULL || userid == NULL || action_message == NULL)
    {
        return false;
    }

    // 统计当前数量，超过上限则移除最旧的
    int count = 0;
    room_ctrl_t *tail_prev = NULL;
    room_ctrl_t *tail = NULL;
    for (room_ctrl_t *cur = room->room_ctrl_head->next, *prev = room->room_ctrl_head;
         cur != NULL; prev = cur, cur = cur->next)
    {
        count++;
        tail_prev = prev;
        tail = cur;
    }
    while (count >= MAX_ROOM_ACTIONS && tail_prev && tail)
    {
        tail_prev->next = NULL;
        free(tail);
        count--;
        // 重新找尾部
        tail = NULL;
        tail_prev = NULL;
        for (room_ctrl_t *cur = room->room_ctrl_head->next, *prev = room->room_ctrl_head;
             cur != NULL; prev = cur, cur = cur->next)
        {
            tail_prev = prev;
            tail = cur;
        }
    }

    room_ctrl_t *new_node = (room_ctrl_t *)malloc(sizeof(room_ctrl_t));
    if (!new_node)
    {
        lwsl_err("Failed to allocate memory for room_ctrl_t\n");
        return false;
    }
    memset(new_node, 0, sizeof(room_ctrl_t));
    strncpy(new_node->userid, userid, 63);
    if (nickname) copy_utf8_bounded(new_node->nickname, nickname, sizeof(new_node->nickname));
    if (avatar_url) copy_utf8_bounded(new_node->avatar_url, avatar_url, sizeof(new_node->avatar_url));
    new_node->action = action;
    copy_utf8_bounded(new_node->action_message, action_message, sizeof(new_node->action_message));
    new_node->action_time = time(NULL);
    return insert_room_action(room, new_node);
}
// 释放房间操作链表
void free_room_action(rooms_t *room)
{
    room_ctrl_t *cur = room->room_ctrl_head->next;
    while (cur != NULL)
    {
        room_ctrl_t *next = cur->next;
        free(cur);
        cur = next;
    }
    free(room->room_ctrl_head);
    room->room_ctrl_head = NULL;
}
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
    room->latest_msg = strdup("");
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
// 新建房间节点插入房间链表,返回该房间节点
rooms_t *insert_room_info(const char *room_id, const char *creater_id, rooms_t *head)
{
    rooms_t *new_node = (rooms_t *)malloc(sizeof(rooms_t));
    if (!new_node)
    {
        lwsl_err("Failed to allocate memory for rooms\n");
        return NULL;
    }
    memset(new_node, 0, sizeof(rooms_t));
    strncpy(new_node->room_id, room_id, 63);
    strncpy(new_node->creater_id, creater_id, 63);
    new_node->client_counter = 0;
    new_node->client_info = (client_info_t *)malloc(sizeof(client_info_t)); // 初始化客户端链表头节点
    if (!new_node->client_info)
    {
        lwsl_err("Failed to allocate memory for client_info_t\n");
        free(new_node);
        return NULL;
    }
    memset(new_node->client_info, 0, sizeof(client_info_t));
    new_node->client_info->next = NULL;
    new_node->client_info->prev = NULL;
    new_node->playlist_head = (playlist_t *)malloc(sizeof(playlist_t)); // 初始化播放列表头节点
    if (!new_node->playlist_head)
    {
        lwsl_err("Failed to allocate memory for playlist_t\n");
        free(new_node->client_info);
        free(new_node);
        return NULL;
    }
    memset(new_node->playlist_head, 0, sizeof(playlist_t));
    new_node->playlist_head->next = NULL;
    new_node->playlist_tail = new_node->playlist_head;      // 初始化尾节点指向头节点
    new_node->current_song = new_node->playlist_head->next; // 初始化当前播放歌曲指向头节点
    new_node->latest_msg = strdup("");
    pthread_mutex_init(&new_node->lock, NULL);
    pthread_mutex_init(&new_node->playing_info.lock, NULL);
    new_node->playing_info.room = new_node;
    new_node->next = NULL;
    new_node->room_ctrl_head = init_action_list();
    if (!new_node->room_ctrl_head)
    {
        lwsl_err("Failed to allocate memory for room_ctrl_head\n");
        pthread_mutex_destroy(&new_node->lock);
        pthread_mutex_destroy(&new_node->playing_info.lock);
        free(new_node->playlist_head);
        free(new_node->client_info);
        free(new_node->latest_msg);
        free(new_node);
        return NULL;
    }
    if (!insert_room_node(head, new_node))
    {
        lwsl_err("Failed to insert room node\n");
        pthread_mutex_destroy(&new_node->lock);
        pthread_mutex_destroy(&new_node->playing_info.lock);
        free(new_node->playlist_head);
        free(new_node->client_info);
        free(new_node->room_ctrl_head);
        free(new_node->latest_msg);
        free(new_node);
        return NULL;
    }
    return new_node;
}
// 移除对应room节点
void remove_room_node(rooms_t *head, rooms_t *node)
{
    // 取消该房间的定时器
    lws_sul_cancel(&node->playing_info.timer);
    // 先释放播放列表链表
    playlist_t *cur = node->playlist_head;
    while (cur != NULL)
    {
        playlist_t *next = cur->next;
        free(cur);
        cur = next;
    }
    node->playlist_head = NULL;
    // 释放房间操作链表
    free_room_action(node);

    // 释放客户端链表（含消息队列）
    client_info_t *ccur = node->client_info;
    while (ccur != NULL)
    {
        client_info_t *cnext = ccur->next;
        for (int i = 0; i < CLIENT_MSG_QUEUE_SIZE; i++)
            free(ccur->msg_queue[i]);
        pthread_mutex_destroy(&ccur->lock);
        free(ccur);
        ccur = cnext;
    }

    // 销毁互斥锁
    pthread_mutex_destroy(&node->lock);
    free(node->latest_msg);
    pthread_mutex_destroy(&node->playing_info.lock);

    // 再删除节点
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
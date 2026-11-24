#ifndef MONITOR_EVENTS_H
#define MONITOR_EVENTS_H

#include "types.h"
#include "cJSON.h"

// 监控事件外发（纯记录，不参与业务逻辑）：
// 把房间/成员/播放/聊天的完整事件以 JSONL 追加写到 MONITOR_EVENTS_FILE
// （默认 /app/monitor-events/ws.jsonl），由外部监控服务采集落库。
// 任何写入失败都静默降级，绝不影响主流程。

// 初始化：创建目录并打开事件文件（进程启动时调用一次）
void monitor_events_init(void);

// 建房 / 进房 / 退房 / 房间销毁
void monitor_emit_room_create(const rooms_t *room, const char *userid, const char *nickname, const char *ip);
void monitor_emit_join(const char *room_id, const client_info_t *client);
void monitor_emit_leave(const rooms_t *room, const client_info_t *client); // 顺带补发一张房间快照
void monitor_emit_room_destroy(const char *room_id);
void monitor_forget_room(const rooms_t *room); // 清理节流状态（房间节点释放前调用）

// 歌曲变化：cause ∈ "system_auto" | "manual_next" | "play_hash"；by 可为 NULL（系统）
void monitor_emit_song_change(const rooms_t *room, const char *cause, const client_info_t *by);

// 加歌 / 移歌 / 置顶（song_op_type ∈ "song_remove" | "song_pin"）
void monitor_emit_song_add(const client_info_t *client, const char *songname, const char *songhash,
                           const char *singername, const char *albumname, const char *duration,
                           const char *coverurl);
void monitor_emit_song_op(const client_info_t *client, const char *song_op_type, const char *songhash);

// 暂停 / 恢复（is_playing: 0=暂停 1=播放）
void monitor_emit_play_state(const client_info_t *client, int is_playing);

// 聊天消息
void monitor_emit_chat(const client_info_t *client, const char *message);

// 播放定时器每拍调用：按节流间隔补发 progress（≥10s）与 snapshot（≥60s）事件
void monitor_tick_room(rooms_t *room);

// ── WebSocket 交互流（人类可读摘要，不记录协议原文）──
// 客户端发来的请求（root 为已解析的消息 JSON，本函数只读不改不释放）
void monitor_emit_ws_in(const client_info_t *client, const cJSON *root);
// 房间全员广播（msg 为广播的 JSON 字符串，仅用于翻译摘要）
void monitor_emit_ws_broadcast(const rooms_t *room, const char *msg);
// 定向发给某个客户端的 JSON 消息
void monitor_emit_ws_send(const client_info_t *client, const char *msg);

#endif // MONITOR_EVENTS_H

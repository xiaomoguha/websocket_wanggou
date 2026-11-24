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
    char cover_url[512];
    char added_by_nickname[128];
    char added_by_avatar[512];
    char is_system; // 0=用户添加, 1=系统推荐
    char mxid[24];  // 评论用 album_audio_id：切歌时懒解析回填，客户端免按歌名搜索
    int like_count; // 点赞数（房间级 song_like_t 链表维护，房间内共享）
    char recommend_source[64]; // 推荐来源标签（热歌榜/每日推荐/AI 推荐/私人FM）
    struct playlist *next;
} playlist_t;
// 正在播放的歌曲信息
typedef struct playing_info
{
    lws_sorted_usec_list_t timer;
    char song_name[128];
    char song_hash[128];
    char song_url[1024];
    char singer_name[128];
    char album_name[128];
    char duration[16];
    char cover_url[512];
    char mxid[24]; // 当前歌的 album_audio_id（随歌曲信息广播）
    char added_by_nickname[128]; // 点歌/推荐人昵称（一起听状态文案）
    char recommend_source[64];   // 推荐来源（系统/私人FM）
    double played_percent;
    char is_playing;
    time_t last_update_time;
    struct rooms *room;
    pthread_mutex_t lock;
} playing_info_t;
// 客户端信息
#define CLIENT_MSG_QUEUE_SIZE 8
// 一次推荐最多询问几个不同客户端；全部超时/失败后回退旧算法
#define FM_RECOMMEND_MAX_TRIES 3
// 单条消息（把所有分片算在一起）的上限：批量加歌 500 首约 75KB，留足余量
#define MAX_RX_MESSAGE (512 * 1024)
typedef struct client_info
{
    struct lws *wsi;
    char ip[INET_ADDRSTRLEN];
    struct rooms *room;
    char userId[64];
    char nickname[128];
    char avatar_url[512];
    char fm_ready; // 1 = 客户端已登录且支持代拉个人FM，可接收 216 请求
    char *msg_queue[CLIENT_MSG_QUEUE_SIZE]; // 动态分配的消息队列
    int msg_queue_head;
    int msg_queue_count;
    unsigned int last_broadcast_version;
    struct client_info *next;
    struct client_info *prev;
    // 接收分片缓冲：libwebsockets 按接收缓冲把大消息切成多次回调，
    // 累到最后一个分片再解析（见 client_callback_receive）
    char *rx_buf;
    size_t rx_len;
    pthread_mutex_t lock;
} client_info_t;
// 房间操作信息
typedef struct room_ctrl
{
    char userid[64];
    char nickname[128];
    char avatar_url[512];
    int action;
    char action_message[512];
    time_t action_time;
    struct room_ctrl *next;
} room_ctrl_t;
// 歌曲点赞（房间级）：同一 song_hash + userid 只保留一条，取消点赞即删除
typedef struct song_like
{
    char song_hash[128];
    char userid[64];
    struct song_like *next;
} song_like_t;
// 房间信息
typedef struct rooms
{
    char room_id[64];
    char creater_id[64];
    unsigned int client_counter;
    client_info_t *client_info;
    char *latest_msg; // 动态分配的广播消息
    unsigned int broadcast_version;
    pthread_mutex_t lock;
    playlist_t *playlist_head;
    playlist_t *playlist_tail;
    playlist_t *current_song;
    room_ctrl_t *room_ctrl_head;
    song_like_t *likes; // 房间内所有歌曲点赞，按 song_hash + userid 去重
    playlist_t *pending_recommend; // 已预取、等待队列空时插入的推荐歌曲（不广播、不计入队列）
    struct
    {
        int active;                 // 1 = 已发 216，正在等客户端 217（不阻塞事件循环）
        char request_id[64];
        char target_userid[64];
        char tried_userids[FM_RECOMMEND_MAX_TRIES][64];
        int tried_count;
        int attempt;
        time_t retry_after;         // 本轮失败后的冷却时间，避免每个 tick 重试
    } fm_recommend;
    lws_sorted_usec_list_t fm_timer; // 216 回复超时定时器（lws 事件循环，非阻塞）
    playing_info_t playing_info;
    // 系统推荐去重
    char recommended_hashes[50][128];
    int recommended_count;
    int recommend_page;
    time_t last_user_add_time; // 最近一次用户加歌时间（系统推荐不计）：空闲回收依据
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
    GET_CLIENT_LIST,
    BROADCAST_SONG_PROGRESS,
    // 批量加歌（客户端「添加到一起听」）：一条消息加一批，服务端只锁一次、只广播一次。
    // 追加在枚举末尾以保持既有动作编号不变（老客户端不受影响）
    ADD_SONGS_TO_PLAYLIST,
    LIKE_SONG,
    // 一起听 FM 推荐：服务端 216 请求客户端代拉个人FM，客户端 217 回传歌曲
    REQUEST_FM_RECOMMEND,
    SUBMIT_FM_RECOMMEND,
    SEND_CHAT = 300,
    BROADCAST_CHAT,
    BROADCAST_ROOM_ACTION,
};

enum CODE
{
    SUCCESS,
    FAIL
};
#endif // TYPES_H
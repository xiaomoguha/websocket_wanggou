#include <libwebsockets.h>
#include <string.h>
#include <signal.h>

static volatile int interrupted;
struct lws_context *context;

struct client_ctx
{
    struct lws_context *context;
    struct lws *wsi;
    int connected;
    struct lws_sorted_usec_list sul;
};

// 定时器回调
static void send_timer_cb(struct lws_sorted_usec_list *sul)
{
    struct client_ctx *ctx = lws_container_of(sul, struct client_ctx, sul);
    if (ctx->connected && ctx->wsi)
    {
        lws_callback_on_writable(ctx->wsi);
    }
    // 重新调度：3秒后再次触发
    lws_sul_schedule(ctx->context, 0, &ctx->sul, send_timer_cb, LWS_US_PER_SEC * 3);
}

// LWS 回调
static int callback_client(struct lws *wsi, enum lws_callback_reasons reason,
                           void *user, void *in, size_t len)
{
    struct client_ctx *ctx = (struct client_ctx *)user;

    switch (reason)
    {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        struct client_ctx *connectx = (struct client_ctx *)malloc(sizeof(struct client_ctx));
        if (!connectx)
        {
            lwsl_err("Failed to allocate memory for client_ctx\n");
            return -1;
        }
        memset(connectx, 0, sizeof(struct client_ctx));
        connectx->context = lws_get_context(wsi);
        connectx->wsi = wsi;
        connectx->connected = 1;
        lws_set_opaque_user_data(wsi, connectx);
        lwsl_notice("连接成功，启动定时器\n");
        lws_sul_schedule(connectx->context, 0, &connectx->sul, send_timer_cb, LWS_US_PER_SEC * 3);
        break;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
    {
        const char *msg = "hello from sul timer";
        unsigned char buf[LWS_PRE + 64];
        size_t n = strlen(msg);
        memcpy(buf + LWS_PRE, msg, n);
        lws_write(wsi, buf + LWS_PRE, n, LWS_WRITE_TEXT);
        lwsl_notice("✅ 发送: %s\n", msg);
        break;
    }

    case LWS_CALLBACK_CLIENT_CLOSED:
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        ctx->connected = 0;
        lws_sul_cancel(&ctx->sul); // 取消定时器
        break;

    default:
        break;
    }
    return 0;
}

// 信号处理
void sigint_handler(int sig)
{
    interrupted = 1;
}

int main(char argc, char **argv)
{
    char *userid = argv[1] ? argv[1] : "default_user";
    char url[128] = {0};
    sprintf(url, "/?roomid=test123&userId=%s", userid);
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = (struct lws_protocols[]){
        {"ctrl-protocol", callback_client, sizeof(struct client_ctx), 0, 0, NULL, 0},
        {NULL, NULL, 0, 0, 0, NULL, 0}};
    info.options = 0;

    struct lws_context *context = lws_create_context(&info);
    if (!context)
    {
        lwsl_err("创建 context 失败\n");
        return -1;
    }

    struct lws_client_connect_info i = {0};
    i.context = context;
    i.address = "localhost";
    i.port = 8080;
    i.path = url;
    i.host = i.address;
    i.origin = i.address;
    i.protocol = "ctrl-protocol";

    if (!lws_client_connect_via_info(&i))
    {
        lwsl_err("连接失败\n");
        goto cleanup;
    }

    signal(SIGINT, sigint_handler);

    while (!interrupted)
    {
        lws_service(context, 10); // 必须调用！sul 依赖它
    }

cleanup:
    lws_context_destroy(context);
    return 0;
}
#ifndef WEBSOCKET_SERVICE_H
#define WEBSOCKET_SERVICE_H
#include <netinet/in.h>
#include <arpa/inet.h>
#include <libwebsockets.h>

void timer_callback(lws_sorted_usec_list_t *sul);

#endif // WEBSOCKET_SERVICE_H

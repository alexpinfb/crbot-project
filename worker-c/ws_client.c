#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libwebsockets.h>

#include "ws_readonly.h"

static int interrupted = 0;

static int cb(struct lws *wsi,
              enum lws_callback_reasons reason,
              void *user,
              void *in,
              size_t len)
{
    (void)user;

    switch (reason) {
        case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
            unsigned char **hp = (unsigned char **)in;
            unsigned char *end = (*hp) + len;

            const char *cookie = getenv("CRBOT_COOKIE");
            const char *ua = getenv("CRBOT_UA");

            if (cookie) {
                if (lws_add_http_header_by_name(
                        wsi,
                        (const unsigned char *)"cookie:",
                        (const unsigned char *)cookie,
                        strlen(cookie),
                        hp,
                        end)) {
                    return -1;
                }
            }

            if (ua) {
                if (lws_add_http_header_by_name(
                        wsi,
                        (const unsigned char *)"user-agent:",
                        (const unsigned char *)ua,
                        strlen(ua),
                        hp,
                        end)) {
                    return -1;
                }
            }

            break;
        }

        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            printf("C_WS_READY\n");
            fflush(stdout);
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
            printf("C_WS_RECV len=%zu data=%.*s\n", len, (int)len, (const char *)in);

            if (len == 1 && ((const char *)in)[0] == '2') {
                unsigned char buf[LWS_PRE + 1];
                buf[LWS_PRE] = '3';
                lws_write(wsi, &buf[LWS_PRE], 1, LWS_WRITE_TEXT);
                printf("C_WS_SEND 3\n");
            } else if (len > 0 && ((const char *)in)[0] == '0') {
                const char *msg = "40";
                unsigned char buf[LWS_PRE + 2];
                memcpy(&buf[LWS_PRE], msg, 2);
                lws_write(wsi, &buf[LWS_PRE], 2, LWS_WRITE_TEXT);
                printf("C_WS_SEND 40\n");
            } else if (len >= 2 && ((const char *)in)[0] == '4' && ((const char *)in)[1] == '0') {
                const char *msg = "42[\"list:initialize\"]";
                size_t mlen = strlen(msg);
                unsigned char buf[LWS_PRE + 64];
                memcpy(&buf[LWS_PRE], msg, mlen);
                lws_write(wsi, &buf[LWS_PRE], mlen, LWS_WRITE_TEXT);
                printf("C_WS_SEND list:initialize\n");
            } else {
                parse_event((const char *)in);
            }

            fflush(stdout);
            break;

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            printf("C_WS_ERROR %s\n", in ? (const char *)in : "unknown");
            fflush(stdout);
            interrupted = 1;
            break;

        case LWS_CALLBACK_CLIENT_CLOSED:
            printf("C_WS_CLOSED\n");
            fflush(stdout);
            interrupted = 1;
            break;

        default:
            break;
    }

    return 0;
}

static const struct lws_protocols protocols[] = {
    { "crbot-protocol", cb, 0, 65536 },
    { NULL, NULL, 0, 0 }
};

int ws_run_forever(void)
{
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));

    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;

    struct lws_context *context = lws_create_context(&info);

    if (!context) {
        printf("C_WS_CONTEXT_FAIL\n");
        return 1;
    }

    struct lws_client_connect_info ccinfo;
    memset(&ccinfo, 0, sizeof(ccinfo));

    ccinfo.context = context;
    ccinfo.address = "app.send.tg";
    ccinfo.port = 443;
    ccinfo.path = "/internal/v1/p2c-socket/?EIO=4&transport=websocket";
    ccinfo.host = ccinfo.address;
    ccinfo.origin = "https://app.send.tg";
    ccinfo.ssl_connection = LCCSCF_USE_SSL;

    if (!lws_client_connect_via_info(&ccinfo)) {
        printf("C_WS_CONNECT_FAIL\n");
        lws_context_destroy(context);
        return 1;
    }

    while (!interrupted) {
        lws_service(context, 500);
    }

    lws_context_destroy(context);
    return 0;
}

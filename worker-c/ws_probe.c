#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libwebsockets.h>

static int interrupted = 0;

static int cb(struct lws *wsi, enum lws_callback_reasons reason,
              void *user, void *in, size_t len) {
    (void)user;

    switch (reason) {
        case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
            unsigned char **p = (unsigned char **)in;
            unsigned char *end = (*p) + len;

            const char *cookie = getenv("CRBOT_COOKIE");
            const char *ua = getenv("CRBOT_UA");

            if (cookie) {
                char h[4096];
                snprintf(h, sizeof(h), "Cookie: %s\r\n", cookie);
                if (lws_add_http_header_by_name(wsi, (const unsigned char *)"cookie:", (const unsigned char *)cookie, strlen(cookie), p, end))
                    return -1;
            }

            if (ua) {
                if (lws_add_http_header_by_name(wsi, (const unsigned char *)"user-agent:", (const unsigned char *)ua, strlen(ua), p, end))
                    return -1;
            }

            break;
        }

        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            printf("C_WS_READY\n");
            fflush(stdout);
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
            printf("C_WS_RECV len=%zu data=%.*s\n", len, (int)len, (const char *)in);
            fflush(stdout);

            if (len == 1 && ((const char *)in)[0] == '2') {
                unsigned char buf[LWS_PRE + 1];
                buf[LWS_PRE] = '3';
                lws_write(wsi, &buf[LWS_PRE], 1, LWS_WRITE_TEXT);
                printf("C_WS_SEND 3\n");
                fflush(stdout);
            } else if (len > 0 && ((const char *)in)[0] == '0') {
                const char *msg = "40";
                unsigned char buf[LWS_PRE + 2];
                memcpy(&buf[LWS_PRE], msg, 2);
                lws_write(wsi, &buf[LWS_PRE], 2, LWS_WRITE_TEXT);
                printf("C_WS_SEND 40\n");
                fflush(stdout);
            } else if (len >= 2 && ((const char *)in)[0] == '4' && ((const char *)in)[1] == '0') {
                const char *msg = "42[\"list:initialize\"]";
                size_t mlen = strlen(msg);
                unsigned char buf[LWS_PRE + 64];
                memcpy(&buf[LWS_PRE], msg, mlen);
                lws_write(wsi, &buf[LWS_PRE], mlen, LWS_WRITE_TEXT);
                printf("C_WS_SEND list:initialize\n");
                fflush(stdout);
            }

            break;

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            printf("C_WS_ERROR %s\n", in ? (const char *)in : "(null)");
            fflush(stdout);
            interrupted = 1;
            break;

        case LWS_CALLBACK_CLOSED:
        case LWS_CALLBACK_CLIENT_CLOSED:
            printf("C_WS_CLOSE\n");
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

int main(void) {
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));

    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    struct lws_context *context = lws_create_context(&info);

    if (!context) {
        fprintf(stderr, "lws_create_context failed\n");
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

    struct lws *wsi = lws_client_connect_via_info(&ccinfo);

    if (!wsi) {
        fprintf(stderr, "lws_client_connect_via_info failed\n");
        lws_context_destroy(context);
        return 1;
    }

    int n = 0;
    while (!interrupted && n < 300) {
        lws_service(context, 100);
        n++;
    }

    lws_context_destroy(context);
    return 0;
}

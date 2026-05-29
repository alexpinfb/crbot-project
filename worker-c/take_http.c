#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "take_http.h"

static size_t capture(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t n = size * nmemb;
    char *buf = (char *)userdata;
    size_t cur = strlen(buf);

    if (cur + n > 511)
        n = 511 - cur;

    if (n > 0) {
        memcpy(buf + cur, ptr, n);
        buf[cur + n] = 0;
    }

    return size * nmemb;
}

void take_http_stub(const char *id, const char *amount, const char *brand) {
    const char *cookie = getenv("CRBOT_COOKIE");
    const char *ua = getenv("CRBOT_UA");

    if (!cookie || !ua) {
        printf("TAKE_HTTP_SKIP_AUTH id=%s\n", id);
        fflush(stdout);
        return;
    }

    CURL *c = curl_easy_init();
    if (!c) {
        printf("TAKE_HTTP_INIT_FAIL id=%s\n", id);
        fflush(stdout);
        return;
    }

    char url[512];
    snprintf(url, sizeof(url),
             "https://app.send.tg/internal/v1/p2c/payments/take/%s",
             id);

    struct curl_slist *h = NULL;
    char cookie_h[2048];
    char ua_h[1024];

    snprintf(cookie_h, sizeof(cookie_h), "Cookie: %s", cookie);
    snprintf(ua_h, sizeof(ua_h), "User-Agent: %s", ua);

    h = curl_slist_append(h, cookie_h);
    h = curl_slist_append(h, ua_h);
    h = curl_slist_append(h, "Origin: https://app.send.tg");
    h = curl_slist_append(h, "Referer: https://app.send.tg/");
    h = curl_slist_append(h, "Connection: keep-alive");

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, 0L);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    char body[512] = {0};
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, capture);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 3000L);

    CURLcode res = curl_easy_perform(c);

    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);

    printf("TAKE_HTTP_SEND domain=app.send.tg id=%s amount=%s brand=%s code=%ld curl=%d body=%s\n",
           id,
           amount,
           brand,
           code,
           (int)res,
           body);
    fflush(stdout);

    if (h) curl_slist_free_all(h);
    curl_easy_cleanup(c);
}

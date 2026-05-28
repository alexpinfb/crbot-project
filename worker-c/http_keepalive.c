#include <stdio.h>
#include <stdlib.h>
#include <curl/curl.h>
#include <time.h>

static size_t discard(char *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)ptr;
    (void)userdata;
    return size * nmemb;
}

static void warm_domain(const char *domain) {
    CURL *c = curl_easy_init();

    if (!c) {
        printf("KEEPALIVE_INIT_FAIL domain=%s\n", domain);
        fflush(stdout);
        return;
    }

    char url[256];
    snprintf(url, sizeof(url), "https://%s/internal/v1/p2c/payments/take/warmup_%ld", domain, (long)time(NULL));

    const char *cookie = getenv("CRBOT_COOKIE");
    const char *ua = getenv("CRBOT_UA");

    struct curl_slist *h = NULL;
    char cookie_h[2048];
    char ua_h[1024];

    if (cookie) {
        snprintf(cookie_h, sizeof(cookie_h), "Cookie: %s", cookie);
        h = curl_slist_append(h, cookie_h);
    }

    if (ua) {
        snprintf(ua_h, sizeof(ua_h), "User-Agent: %s", ua);
        h = curl_slist_append(h, ua_h);
    }

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, 0L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 3000L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, discard);

    CURLcode res = curl_easy_perform(c);

    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);

    printf("KEEPALIVE_WARMUP_HTTP domain=%s code=%ld curl=%d\n", domain, code, (int)res);
    fflush(stdout);

    if (h)
        curl_slist_free_all(h);

    curl_easy_cleanup(c);
}

void keepalive_warmup_once(void) {
    warm_domain("app.send.tg");
    warm_domain("app.cr.bot");
}

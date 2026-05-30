#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <time.h>
#include <hiredis/hiredis.h>
#include <pthread.h>
#include "take_http.h"


struct take_job {
    char id[128];
    char amount[64];
    char brand[256];
    char cookie[2048];
    char ua[1024];
};

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


static void save_success(const char *source_id, const char *amount, const char *brand, const char *body) {
    const char *addr = getenv("REDIS_ADDR");
    const char *pass = getenv("REDIS_PASSWORD");
    const char *worker = getenv("WORKER_ID");

    if (!addr || !pass || !worker) return;

    redisContext *rc = redisConnect(addr, 6379);
    if (!rc || rc->err) return;

    redisReply *auth = redisCommand(rc, "AUTH %s", pass);
    if (auth) freeReplyObject(auth);

    redisReply *r1 = redisCommand(rc, "SETEX crbot:cppTake:%s 600 %s", source_id, body);
    if (r1) freeReplyObject(r1);

    redisReply *r2 = redisCommand(rc, "SETEX crbot:activeOrder 600 %s", body);
    if (r2) freeReplyObject(r2);

    printf("TAKE_SUCCESS_SAVE id=%s amount=%s brand=%s worker=%s\n", source_id, amount, brand, worker);
    fflush(stdout);

    redisFree(rc);
}

static void *take_http_worker(void *arg) {
    struct take_job *job = (struct take_job *)arg;
    const char *id = job->id;
    const char *amount = job->amount;
    const char *brand = job->brand;
    const char *cookie = job->cookie;
    const char *ua = job->ua;
    CURL *c = curl_easy_init();
    if (!c) {
        printf("TAKE_HTTP_INIT_FAIL id=%s\n", id);
        fflush(stdout);
        free(job);
        return NULL;
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
    h = curl_slist_append(h, "Accept: application/json");
    h = curl_slist_append(h, "Content-Length: 0");

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, 0L);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    char body[512] = {0};
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, capture);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 1500L);
    struct curl_slist *resolve = NULL;
    resolve = curl_slist_append(resolve, "app.send.tg:443:138.249.21.1");
    curl_easy_setopt(c, CURLOPT_RESOLVE, resolve);

    struct timespec t1, t2;
    clock_gettime(CLOCK_MONOTONIC, &t1);

    CURLcode res = curl_easy_perform(c);

    clock_gettime(CLOCK_MONOTONIC, &t2);
    long elapsed_ms = (long)((t2.tv_sec - t1.tv_sec) * 1000 + (t2.tv_nsec - t1.tv_nsec) / 1000000);

    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);

    printf("TAKE_HTTP_SEND domain=app.send.tg id=%s amount=%s brand=%s code=%ld curl=%d elapsed_ms=%ld body=%s\n",
           id,
           amount,
           brand,
           code,
           (int)res,
           elapsed_ms,
           body);
    fflush(stdout);

    if (code == 200 && body[0]) {
        save_success(id, amount, brand, body);
    }

    if (h) curl_slist_free_all(h);
    if (resolve) curl_slist_free_all(resolve);
    curl_easy_cleanup(c);
    free(job);
    return NULL;
}

void take_http_stub(const char *id, const char *amount, const char *brand) {
    const char *cookie = getenv("CRBOT_COOKIE");
    const char *ua = getenv("CRBOT_UA");

    if (!cookie || !ua) {
        printf("TAKE_HTTP_SKIP_AUTH id=%s\n", id);
        fflush(stdout);
        return;
    }

    struct take_job *job = calloc(1, sizeof(*job));
    if (!job) {
        printf("TAKE_HTTP_JOB_ALLOC_FAIL id=%s\n", id);
        fflush(stdout);
        return;
    }

    snprintf(job->id, sizeof(job->id), "%s", id);
    snprintf(job->amount, sizeof(job->amount), "%s", amount);
    snprintf(job->brand, sizeof(job->brand), "%s", brand);
    snprintf(job->cookie, sizeof(job->cookie), "%s", cookie);
    snprintf(job->ua, sizeof(job->ua), "%s", ua);

    pthread_t th;
    if (pthread_create(&th, NULL, take_http_worker, job) != 0) {
        printf("TAKE_HTTP_THREAD_FAIL id=%s\n", id);
        fflush(stdout);
        free(job);
        return;
    }

    pthread_detach(th);
}

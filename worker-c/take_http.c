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
    char domain[128];
    char origin[128];
    char resolve[256];
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


static void save_success(const char *source_id, const char *amount, const char *brand, const char *domain, long elapsed_ms, const char *body) {
    const char *addr = getenv("REDIS_ADDR");
    const char *pass = getenv("REDIS_PASSWORD");
    const char *worker = getenv("WORKER_ID");

    if (!addr || !pass || !worker) return;

    redisContext *rc = redisConnect(addr, 6379);
    if (!rc || rc->err) return;

    redisReply *auth = redisCommand(rc, "AUTH %s", pass);
    if (auth) freeReplyObject(auth);

    const char *data = body;
    const char *p = strstr(body, "{\"data\":");
    if (p == body) {
        data = body + 8;
        size_t len = strlen(data);
        if (len > 0 && data[len - 1] == '}') {
            char *tmp = strdup(data);
            if (tmp) {
                tmp[len - 1] = 0;

                char json[4096];
                snprintf(json, sizeof(json),
                         "{\"source_id\":\"%s\",\"source_worker\":\"%s\",\"workerId\":\"%s\",\"accountName\":\"a7\",\"source_domain\":\"%s\",\"elapsed_ms\":%ld,\"brand\":\"%s\",\"amount\":\"%s\",\"data\":%s}",
                         source_id, worker, worker, domain, elapsed_ms, brand, amount, tmp);

                redisReply *r1 = redisCommand(rc, "SETEX crbot:cppTake:%s 600 %s", source_id, json);
                if (r1) freeReplyObject(r1);

                redisReply *r2 = redisCommand(rc, "SETEX crbot:activeOrder 600 %s", json);
                if (r2) freeReplyObject(r2);

                free(tmp);
            }
        }
    }

    printf("TAKE_SUCCESS_SAVE id=%s amount=%s brand=%s worker=%s domain=%s elapsed_ms=%ld\n", source_id, amount, brand, worker, domain, elapsed_ms);
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
    const char *domain = job->domain;
    const char *origin = job->origin;
    const char *resolve_rule = job->resolve;
    CURL *c = curl_easy_init();
    if (!c) {
        printf("TAKE_HTTP_INIT_FAIL id=%s\n", id);
        fflush(stdout);
        free(job);
        return NULL;
    }

    char url[512];
    snprintf(url, sizeof(url),
             "https://%s/internal/v1/p2c/payments/take/%s",
             domain,
             id);

    struct curl_slist *h = NULL;
    char cookie_h[2048];
    char ua_h[1024];

    snprintf(cookie_h, sizeof(cookie_h), "Cookie: %s", cookie);
    snprintf(ua_h, sizeof(ua_h), "User-Agent: %s", ua);

    h = curl_slist_append(h, cookie_h);
    h = curl_slist_append(h, ua_h);
        char origin_h[256];
    char referer_h[256];
    snprintf(origin_h, sizeof(origin_h), "Origin: %s", origin);
    snprintf(referer_h, sizeof(referer_h), "Referer: %s/", origin);
    h = curl_slist_append(h, origin_h);
    h = curl_slist_append(h, referer_h);
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
    resolve = curl_slist_append(resolve, resolve_rule);
    curl_easy_setopt(c, CURLOPT_RESOLVE, resolve);

    struct timespec t1, t2;
    clock_gettime(CLOCK_MONOTONIC, &t1);

    CURLcode res = curl_easy_perform(c);

    clock_gettime(CLOCK_MONOTONIC, &t2);
    long elapsed_ms = (long)((t2.tv_sec - t1.tv_sec) * 1000 + (t2.tv_nsec - t1.tv_nsec) / 1000000);

    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);

    printf("TAKE_HTTP_SEND domain=%s id=%s amount=%s brand=%s code=%ld curl=%d elapsed_ms=%ld body=%s\n",
           domain,
           id,
           amount,
           brand,
           code,
           (int)res,
           elapsed_ms,
           body);
    fflush(stdout);

    if (code == 200 && body[0]) {
        save_success(id, amount, brand, domain, elapsed_ms, body);
    }

    if (h) curl_slist_free_all(h);
    if (resolve) curl_slist_free_all(resolve);
    curl_easy_cleanup(c);
    free(job);
    return NULL;
}


static void spawn_take_job(const char *id,
                           const char *amount,
                           const char *brand,
                           const char *cookie,
                           const char *ua,
                           const char *domain,
                           const char *origin,
                           const char *resolve_rule) {
    struct take_job *job = calloc(1, sizeof(*job));
    if (!job) {
        printf("TAKE_HTTP_JOB_ALLOC_FAIL domain=%s id=%s\n", domain, id);
        fflush(stdout);
        return;
    }

    snprintf(job->id, sizeof(job->id), "%s", id);
    snprintf(job->amount, sizeof(job->amount), "%s", amount);
    snprintf(job->brand, sizeof(job->brand), "%s", brand);
    snprintf(job->cookie, sizeof(job->cookie), "%s", cookie);
    snprintf(job->ua, sizeof(job->ua), "%s", ua);
    snprintf(job->domain, sizeof(job->domain), "%s", domain);
    snprintf(job->origin, sizeof(job->origin), "%s", origin);
    snprintf(job->resolve, sizeof(job->resolve), "%s", resolve_rule);

    pthread_t th;
    if (pthread_create(&th, NULL, take_http_worker, job) != 0) {
        printf("TAKE_HTTP_THREAD_FAIL domain=%s id=%s\n", domain, id);
        fflush(stdout);
        free(job);
        return;
    }

    pthread_detach(th);
}

void take_http_stub(const char *id, const char *amount, const char *brand) {
    const char *cookie = getenv("CRBOT_COOKIE");
    const char *ua = getenv("CRBOT_UA");

    if (!cookie || !ua) {
        printf("TAKE_HTTP_SKIP_AUTH id=%s\n", id);
        fflush(stdout);
        return;
    }

    spawn_take_job(id, amount, brand, cookie, ua,
                   "app.send.tg",
                   "https://app.send.tg",
                   "app.send.tg:443:138.249.21.1");

    spawn_take_job(id, amount, brand, cookie, ua,
                   "app.cr.bot",
                   "https://app.cr.bot",
                   "app.cr.bot:443:138.249.21.1");
}

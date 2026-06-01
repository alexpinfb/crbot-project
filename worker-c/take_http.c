#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <time.h>
#include <hiredis/hiredis.h>
#include <pthread.h>
#include <unistd.h>
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
    char source_url[512];
    long spawn_ms;
};

struct complete_job {
    char id[128];
    char cookie[2048];
    char ua[1024];
    char domain[128];
    char origin[128];
    char resolve[256];
};

struct curl_pool {
    CURL *curl;
    pthread_mutex_t mu;
    char resolve[256];
};

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}


static struct curl_pool g_pools[4] = {
    {0, PTHREAD_MUTEX_INITIALIZER, "app.send.tg:443:138.249.21.1"},
    {0, PTHREAD_MUTEX_INITIALIZER, "app.send.tg:443:138.249.21.3"},
    {0, PTHREAD_MUTEX_INITIALIZER, "app.cr.bot:443:138.249.21.1"},
    {0, PTHREAD_MUTEX_INITIALIZER, "app.cr.bot:443:138.249.21.3"},
};

static struct curl_pool *pool_for_resolve(const char *resolve_rule) {
    for (int i = 0; i < 4; i++) {
        if (strcmp(g_pools[i].resolve, resolve_rule) == 0) return &g_pools[i];
    }
    return NULL;
}

static CURL *pool_get_locked(const char *resolve_rule) {
    struct curl_pool *pool = pool_for_resolve(resolve_rule);
    if (!pool) return NULL;

    pthread_mutex_lock(&pool->mu);

    if (!pool->curl) {
        pool->curl = curl_easy_init();
        if (pool->curl) {
            curl_easy_setopt(pool->curl, CURLOPT_TCP_KEEPALIVE, 1L);
            curl_easy_setopt(pool->curl, CURLOPT_TCP_KEEPIDLE, 30L);
            curl_easy_setopt(pool->curl, CURLOPT_TCP_KEEPINTVL, 10L);
            curl_easy_setopt(pool->curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(pool->curl, CURLOPT_SSL_SESSIONID_CACHE, 1L);
            curl_easy_setopt(pool->curl, CURLOPT_FRESH_CONNECT, 0L);
            curl_easy_setopt(pool->curl, CURLOPT_FORBID_REUSE, 0L);
            curl_easy_setopt(pool->curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(pool->curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(pool->curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        }
    }

    return pool->curl;
}

static void pool_unlock(const char *resolve_rule) {
    struct curl_pool *pool = pool_for_resolve(resolve_rule);
    if (pool) pthread_mutex_unlock(&pool->mu);
}

static size_t capture(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t n = size * nmemb;
    char *buf = (char *)userdata;
    size_t cur = strlen(buf);

    if (cur + n > 511) n = 511 - cur;

    if (n > 0) {
        memcpy(buf + cur, ptr, n);
        buf[cur + n] = 0;
    }

    return size * nmemb;
}


static char *json_find_string(const char *json, const char *key, char *out, size_t outsz) {
    if (!json || !key || !out || outsz == 0) return NULL;
    out[0] = 0;

    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);

    const char *p = strstr(json, pat);
    if (!p) return NULL;

    p = strchr(p + strlen(pat), ':');
    if (!p) return NULL;
    p++;

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return NULL;
    p++;

    size_t n = 0;
    while (*p && *p != '"' && n + 1 < outsz) {
        if (*p == '\\' && p[1]) p++;
        out[n++] = *p++;
    }
    out[n] = 0;
    return out[0] ? out : NULL;
}


static char *json_find_number(const char *json, const char *key, char *out, size_t outsz) {
    if (!json || !key || !out || outsz == 0) return NULL;
    out[0] = 0;

    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);

    const char *p = strstr(json, pat);
    if (!p) return NULL;

    p = strchr(p + strlen(pat), ':');
    if (!p) return NULL;
    p++;

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;

    size_t n = 0;
    while (*p >= '0' && *p <= '9' && n + 1 < outsz) {
        out[n++] = *p++;
    }

    out[n] = 0;
    return out[0] ? out : NULL;
}


static int http_json_request(
    const char *domain,
    const char *origin,
    const char *resolve_rule,
    const char *cookie,
    const char *ua,
    const char *url,
    const char *method,
    const char *post_body,
    long timeout_ms,
    long *code_out,
    char *body_out,
    size_t body_sz
) {
    CURL *c = curl_easy_init();
    if (!c) return -1;

    if (body_out && body_sz) body_out[0] = 0;

    struct curl_slist *h = NULL;
    struct curl_slist *resolve = NULL;

    char cookie_h[2300];
    char ua_h[1200];
    char origin_h[256];
    char referer_h[256];

    snprintf(cookie_h, sizeof(cookie_h), "Cookie: %s", cookie);
    snprintf(ua_h, sizeof(ua_h), "User-Agent: %s", ua);
    snprintf(origin_h, sizeof(origin_h), "Origin: %s", origin);
    snprintf(referer_h, sizeof(referer_h), "Referer: %s/p2c/payments?tab=active", origin);

    h = curl_slist_append(h, cookie_h);
    h = curl_slist_append(h, ua_h);
    h = curl_slist_append(h, origin_h);
    h = curl_slist_append(h, referer_h);
    h = curl_slist_append(h, "Connection: keep-alive");
    h = curl_slist_append(h, "Accept: application/json");
    h = curl_slist_append(h, "Content-Type: application/json");

    resolve = curl_slist_append(resolve, resolve_rule);

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, capture);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, body_out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(c, CURLOPT_RESOLVE, resolve);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, post_body ? post_body : "");
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, post_body ? (long)strlen(post_body) : 0L);
    }

    CURLcode res = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);

    if (code_out) *code_out = code;

    if (h) curl_slist_free_all(h);
    if (resolve) curl_slist_free_all(resolve);
    curl_easy_cleanup(c);

    return (int)res;
}


static void save_auto_complete_signal(const char *id, const char *domain, const char *reason) {
    const char *addr = getenv("REDIS_ADDR");
    const char *pass = getenv("REDIS_PASSWORD");
    const char *worker = getenv("WORKER_ID");
    if (!addr || !pass || !id) return;

    redisContext *rc = redisConnect(addr, 6379);
    if (!rc || rc->err) {
        if (rc) redisFree(rc);
        return;
    }

    redisReply *auth = redisCommand(rc, "AUTH %s", pass);
    if (auth) freeReplyObject(auth);

    redisReply *r = redisCommand(
        rc,
        "SETEX crbot:autoComplete:%s 60 {\"id\":\"%s\",\"domain\":\"%s\",\"worker\":\"%s\",\"reason\":\"%s\",\"ts\":%ld}",
        id,
        id,
        domain ? domain : "",
        worker ? worker : "",
        reason ? reason : "",
        (long)(time(NULL) * 1000)
    );
    if (r) freeReplyObject(r);

    redisFree(rc);
}

static int payment_still_processing(struct complete_job *job) {
    char url[512];
    char body[8192] = {0};
    long code = 0;

    snprintf(url, sizeof(url), "https://%s/internal/v1/p2c/payments", job->domain);

    int curl = http_json_request(
        job->domain, job->origin, job->resolve,
        job->cookie, job->ua,
        url, "GET", NULL, 5000L,
        &code, body, sizeof(body)
    );

    if (curl != 0 || code < 200 || code >= 300) {
        printf("AUTO_COMPLETE_VERIFY_FAIL id=%s domain=%s code=%ld curl=%d body=%s\n",
               job->id, job->domain, code, curl, body);
        fflush(stdout);
        return 1; // не уверены — считаем что ещё active, чтобы не очищать
    }

    char needle[256];
    snprintf(needle, sizeof(needle), "\"id\":%s", job->id);

    char *p = strstr(body, needle);
    if (!p) {
        printf("AUTO_COMPLETE_VERIFY_OK id=%s domain=%s reason=not_in_active\n",
               job->id, job->domain);
        fflush(stdout);
        save_auto_complete_signal(job->id, job->domain, "not_in_active");
        return 0;
    }

    char *win_end = p + 900;
    char saved = 0;
    if ((size_t)(win_end - body) < sizeof(body)) {
        saved = *win_end;
        *win_end = 0;
    }

    int processing = strstr(p, "\"status\":\"processing\"") != NULL;

    if (saved) *win_end = saved;

    if (processing) {
        printf("AUTO_COMPLETE_VERIFY_ACTIVE id=%s domain=%s\n", job->id, job->domain);
        fflush(stdout);
        return 1;
    }

    printf("AUTO_COMPLETE_VERIFY_OK id=%s domain=%s reason=not_processing\n",
           job->id, job->domain);
    fflush(stdout);
    save_auto_complete_signal(job->id, job->domain, "not_processing");
    return 0;
}

static int complete_once(struct complete_job *job, int elapsed_sec) {
    char accounts_url[512];
    char complete_url[512];
    char acc_body[512] = {0};
    char complete_body_resp[512] = {0};
    char method[128] = {0};
    long acc_code = 0;
    long complete_code = 0;

    snprintf(accounts_url, sizeof(accounts_url), "https://%s/internal/v1/p2c/accounts", job->domain);

    int acc_curl = http_json_request(
        job->domain, job->origin, job->resolve,
        job->cookie, job->ua,
        accounts_url, "GET", NULL, 5000L,
        &acc_code, acc_body, sizeof(acc_body)
    );

    if (acc_curl != 0 || acc_code < 200 || acc_code >= 300) {
        printf("AUTO_COMPLETE_ACCOUNTS_FAIL id=%s domain=%s elapsed_sec=%d code=%ld curl=%d body=%s\n",
               job->id, job->domain, elapsed_sec, acc_code, acc_curl, acc_body);
        fflush(stdout);
        return 0;
    }

    if (!json_find_string(acc_body, "id", method, sizeof(method))) {
        printf("AUTO_COMPLETE_NO_METHOD id=%s domain=%s elapsed_sec=%d accounts=%s\n",
               job->id, job->domain, elapsed_sec, acc_body);
        fflush(stdout);
        return 0;
    }

    char post_body[256];
    snprintf(post_body, sizeof(post_body), "{\"method\":\"%s\"}", method);

    snprintf(complete_url, sizeof(complete_url), "https://%s/internal/v1/p2c/payments/%s/complete", job->domain, job->id);

    printf("AUTO_COMPLETE_ATTEMPT id=%s domain=%s elapsed_sec=%d method=%s\n",
           job->id, job->domain, elapsed_sec, method);
    fflush(stdout);

    int complete_curl = http_json_request(
        job->domain, job->origin, job->resolve,
        job->cookie, job->ua,
        complete_url, "POST", post_body, 5000L,
        &complete_code, complete_body_resp, sizeof(complete_body_resp)
    );

    printf("AUTO_COMPLETE_RESULT id=%s domain=%s elapsed_sec=%d code=%ld curl=%d body=%s\n",
           job->id, job->domain, elapsed_sec, complete_code, complete_curl, complete_body_resp);
    fflush(stdout);

    if (complete_code >= 200 && complete_code < 300) {
        if (!payment_still_processing(job)) return 1;
        return 0;
    }

    if (strstr(complete_body_resp, "InvalidStatus") || strstr(complete_body_resp, "NotFound")) {
        if (!payment_still_processing(job)) return 1;
        return 0;
    }

    return 0;
}

static void clear_active_order(void) {
    const char *addr = getenv("REDIS_ADDR");
    const char *pass = getenv("REDIS_PASSWORD");
    if (!addr || !pass) return;

    redisContext *rc = redisConnect(addr, 6379);
    if (!rc || rc->err) {
        if (rc) redisFree(rc);
        return;
    }

    redisReply *auth = redisCommand(rc, "AUTH %s", pass);
    if (auth) freeReplyObject(auth);

    redisReply *r = redisCommand(rc, "DEL crbot:activeOrder crbot:takeLock crbot:takeLock:a1 crbot:takeLock:a2 crbot:takeLock:a3 crbot:takeLock:a4 crbot:takeLock:a5 crbot:takeLock:a6 crbot:takeLock:a7");
    if (r) freeReplyObject(r);

    redisFree(rc);
}

static void *complete_worker(void *arg) {
    struct complete_job *job = (struct complete_job *)arg;

    sleep(7);

    for (int elapsed = 7; elapsed <= 60; elapsed += 3) {
        if (complete_once(job, elapsed)) {
            clear_active_order();
            break;
        }
        sleep(3);
    }

    free(job);
    return NULL;
}

static void spawn_complete_job(const char *id, const char *cookie, const char *ua, const char *domain, const char *origin, const char *resolve_rule) {
    struct complete_job *job = calloc(1, sizeof(*job));
    if (!job) {
        printf("AUTO_COMPLETE_JOB_ALLOC_FAIL id=%s domain=%s\n", id, domain);
        fflush(stdout);
        return;
    }

    snprintf(job->id, sizeof(job->id), "%s", id);
    snprintf(job->cookie, sizeof(job->cookie), "%s", cookie);
    snprintf(job->ua, sizeof(job->ua), "%s", ua);
    snprintf(job->domain, sizeof(job->domain), "%s", domain);
    snprintf(job->origin, sizeof(job->origin), "%s", origin);
    snprintf(job->resolve, sizeof(job->resolve), "%s", resolve_rule);
    pthread_t th;
    if (pthread_create(&th, NULL, complete_worker, job) != 0) {
        printf("AUTO_COMPLETE_THREAD_FAIL id=%s domain=%s\n", id, domain);
        fflush(stdout);
        free(job);
        return;
    }

    pthread_detach(th);
}


static void disable_worker_penalty(const char *domain, const char *id, const char *amount, const char *body) {
    const char *addr = getenv("REDIS_ADDR");
    const char *pass = getenv("REDIS_PASSWORD");
    const char *worker = getenv("WORKER_ID");
    const char *minv = getenv("MIN_AMOUNT");
    const char *maxv = getenv("MAX_AMOUNT");

    if (!addr || !pass || !worker) {
        printf("PENALTY_STOP_NO_ENV worker=%s domain=%s id=%s amount=%s body=%s\n",
               worker ? worker : "unknown", domain, id, amount, body);
        fflush(stdout);
        return;
    }

    if (!minv) minv = "0";
    if (!maxv) maxv = "0";

    redisContext *rc = redisConnect(addr, 6379);
    if (!rc || rc->err) {
        if (rc) redisFree(rc);
        printf("PENALTY_STOP_REDIS_FAIL worker=%s domain=%s id=%s amount=%s body=%s\n",
               worker, domain, id, amount, body);
        fflush(stdout);
        return;
    }

    redisReply *auth = redisCommand(rc, "AUTH %s", pass);
    if (auth) freeReplyObject(auth);

    char key[256];
    snprintf(key, sizeof(key), "crbot:worker:%s", worker);

    char val[512];
    snprintf(val, sizeof(val), "{\"min\":%s,\"max\":%s,\"enabled\":false}", minv, maxv);

    redisReply *r = redisCommand(rc, "SET %s %s", key, val);
    if (r) freeReplyObject(r);

    printf("PENALTY_STOP worker=%s domain=%s id=%s amount=%s range=%s-%s body=%s\n",
           worker, domain, id, amount, minv, maxv, body);
    fflush(stdout);

    redisFree(rc);
}


static void set_take_cooldown_success(void) {
    const char *addr = getenv("REDIS_ADDR");
    const char *pass = getenv("REDIS_PASSWORD");
    if (!addr || !pass) return;

    redisContext *rc = redisConnect(addr, 6379);
    if (!rc || rc->err) {
        if (rc) redisFree(rc);
        return;
    }

    redisReply *auth = redisCommand(rc, "AUTH %s", pass);
    if (auth) freeReplyObject(auth);

    redisReply *r = redisCommand(rc, "SETEX crbot:takeCooldown 65 1");
    if (r) freeReplyObject(r);

    redisFree(rc);
}

static void set_take_cooldown(void) {
    const char *addr = getenv("REDIS_ADDR");
    const char *pass = getenv("REDIS_PASSWORD");
    if (!addr || !pass) return;

    redisContext *rc = redisConnect(addr, 6379);
    if (!rc || rc->err) {
        if (rc) redisFree(rc);
        return;
    }

    redisReply *auth = redisCommand(rc, "AUTH %s", pass);
    if (auth) freeReplyObject(auth);

    redisReply *r = redisCommand(rc, "SETEX crbot:takeCooldown 3 1");
    if (r) freeReplyObject(r);

    redisFree(rc);
}

static void save_success(const char *source_id, const char *amount, const char *brand, const char *source_url, const char *domain, long elapsed_ms, const char *body) {
    const char *addr = getenv("REDIS_ADDR");
    const char *pass = getenv("REDIS_PASSWORD");
    const char *worker = getenv("WORKER_ID");
    if (!addr || !pass || !worker) return;

    redisContext *rc = redisConnect(addr, 6379);
    if (!rc || rc->err) {
        if (rc) redisFree(rc);
        return;
    }

    redisReply *auth = redisCommand(rc, "AUTH %s", pass);
    if (auth) freeReplyObject(auth);

    const char *data = body;
    char *tmp = NULL;

    if (strncmp(body, "{\"data\":", 8) == 0) {
        data = body + 8;
        size_t len = strlen(data);
        tmp = strdup(data);
        if (tmp && len > 0 && tmp[len - 1] == '}') {
            tmp[len - 1] = 0;
            data = tmp;
        }
    }

    char json[4096];
    snprintf(json, sizeof(json),
        "{\"source_id\":\"%s\",\"source_worker\":\"%s\",\"workerId\":\"%s\",\"accountName\":\"%s\",\"source_domain\":\"%s\",\"elapsed_ms\":%ld,\"brand\":\"%s\",\"amount\":\"%s\",\"url\":\"%s\",\"data\":%s}",
        source_id, worker, worker, worker, domain, elapsed_ms, brand, amount, source_url ? source_url : "", data);

    redisReply *r1 = redisCommand(rc, "SETEX crbot:cppTake:%s 30 %s", source_id, json);
    if (r1) freeReplyObject(r1);

    redisReply *r2 = redisCommand(rc, "SETEX crbot:activeOrder 30 %s", json);
    if (r2) freeReplyObject(r2);

    printf("TAKE_SUCCESS_SAVE id=%s amount=%s brand=%s worker=%s domain=%s elapsed_ms=%ld\n", source_id, amount, brand, worker, domain, elapsed_ms);
    fflush(stdout);

    if (tmp) free(tmp);
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
    const char *source_url = job->source_url;

    CURL *c = pool_get_locked(resolve_rule);
    if (!c) {
        printf("TAKE_HTTP_INIT_FAIL domain=%s id=%s\n", domain, id);
        fflush(stdout);
        pool_unlock(resolve_rule);
        free(job);
        return NULL;
    }

    char url[512];
    snprintf(url, sizeof(url), "https://%s/internal/v1/p2c/payments/take/%s", domain, id);

    struct curl_slist *h = NULL;
    struct curl_slist *resolve = NULL;
    char cookie_h[2300];
    char ua_h[1200];
    char origin_h[256];
    char referer_h[256];
    char body[512] = {0};

    snprintf(cookie_h, sizeof(cookie_h), "Cookie: %s", cookie);
    snprintf(ua_h, sizeof(ua_h), "User-Agent: %s", ua);
    snprintf(origin_h, sizeof(origin_h), "Origin: %s", origin);
    snprintf(referer_h, sizeof(referer_h), "Referer: %s/", origin);

    h = curl_slist_append(h, cookie_h);
    h = curl_slist_append(h, ua_h);
    h = curl_slist_append(h, origin_h);
    h = curl_slist_append(h, referer_h);
    h = curl_slist_append(h, "Connection: keep-alive");
    h = curl_slist_append(h, "Accept: application/json");
    h = curl_slist_append(h, "Content-Length: 0");

    resolve = curl_slist_append(resolve, resolve_rule);

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, 0L);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, capture);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 1500L);
    curl_easy_setopt(c, CURLOPT_RESOLVE, resolve);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

    long pre_perform_ms = now_ms();
    printf("TAKE_HTTP_BEFORE_SEND domain=%s resolve=%s id=%s queue_delay_ms=%ld\n",
           domain, resolve_rule, id, pre_perform_ms - job->spawn_ms);
    fflush(stdout);

    struct timespec t1, t2;
    clock_gettime(CLOCK_MONOTONIC, &t1);

    CURLcode res = curl_easy_perform(c);

    clock_gettime(CLOCK_MONOTONIC, &t2);
    long elapsed_ms = (long)((t2.tv_sec - t1.tv_sec) * 1000 + (t2.tv_nsec - t1.tv_nsec) / 1000000);

    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);

    printf("TAKE_HTTP_SEND domain=%s resolve=%s id=%s amount=%s brand=%s code=%ld curl=%d elapsed_ms=%ld body=%s\n",
           domain, resolve_rule, id, amount, brand, code, (int)res, elapsed_ms, body);
    fflush(stdout);

    if (code == 403 && strstr(body, "MerchantPenalized")) {
        disable_worker_penalty(domain, id, amount, body);
    }

    if (code == 429 && res == CURLE_OK) {
        set_take_cooldown();
        printf("TAKE_COOLDOWN_SET id=%s domain=%s\n", id, domain);
        fflush(stdout);
    }

    if (code == 200 && body[0]) {
        set_take_cooldown_success();
        printf("TAKE_SUCCESS_COOLDOWN_SET id=%s seconds=65\n", id);
        fflush(stdout);
        save_success(id, amount, brand, source_url, domain, elapsed_ms, body);

        char complete_id[128] = {0};
        const char *cid = json_find_number(body, "id", complete_id, sizeof(complete_id));
        printf("AUTO_COMPLETE_ID source_id=%s complete_id=%s\n", id, cid ? cid : "NONE");
        fflush(stdout);
        spawn_complete_job(cid ? cid : id, cookie, ua, domain, origin, resolve_rule);
    }

    if (h) curl_slist_free_all(h);
    if (resolve) curl_slist_free_all(resolve);

    pool_unlock(resolve_rule);
    free(job);
    return NULL;
}

static void spawn_take_job(const char *id, const char *amount, const char *brand, const char *source_url, const char *cookie, const char *ua, const char *domain, const char *origin, const char *resolve_rule) {
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
    snprintf(job->source_url, sizeof(job->source_url), "%s", source_url ? source_url : "");
    job->spawn_ms = now_ms();

    pthread_t th;
    if (pthread_create(&th, NULL, take_http_worker, job) != 0) {
        printf("TAKE_HTTP_THREAD_FAIL domain=%s id=%s\n", domain, id);
        fflush(stdout);
        free(job);
        return;
    }

    pthread_detach(th);
}

void take_http_stub(const char *id, const char *amount, const char *brand, const char *source_url) {
    const char *cookie = getenv("CRBOT_COOKIE");
    const char *ua = getenv("CRBOT_UA");

    if (!cookie || !ua) {
        printf("TAKE_HTTP_SKIP_AUTH id=%s\n", id);
        fflush(stdout);
        return;
    }

    const char *take_domain = getenv("CRBOT_TAKE_DOMAIN");

    if (!take_domain || strcmp(take_domain, "both") == 0) {
        // GO эталон: 2 домена x 2 IP = максимум 4 запроса на worker/id
        spawn_take_job(id, amount, brand, source_url, cookie, ua, "app.send.tg", "https://app.send.tg", "app.send.tg:443:138.249.21.1");
        spawn_take_job(id, amount, brand, source_url, cookie, ua, "app.send.tg", "https://app.send.tg", "app.send.tg:443:138.249.21.3");
        spawn_take_job(id, amount, brand, source_url, cookie, ua, "app.cr.bot",   "https://app.cr.bot",   "app.cr.bot:443:138.249.21.1");
        spawn_take_job(id, amount, brand, source_url, cookie, ua, "app.cr.bot",   "https://app.cr.bot",   "app.cr.bot:443:138.249.21.3");
        return;
    }

    if (strcmp(take_domain, "send") == 0 || strcmp(take_domain, "app.send.tg") == 0) {
        spawn_take_job(id, amount, brand, source_url, cookie, ua, "app.send.tg", "https://app.send.tg", "app.send.tg:443:138.249.21.1");
        spawn_take_job(id, amount, brand, source_url, cookie, ua, "app.send.tg", "https://app.send.tg", "app.send.tg:443:138.249.21.3");
        return;
    }

    if (strcmp(take_domain, "cr") == 0 || strcmp(take_domain, "app.cr.bot") == 0) {
        spawn_take_job(id, amount, brand, source_url, cookie, ua, "app.cr.bot", "https://app.cr.bot", "app.cr.bot:443:138.249.21.1");
        spawn_take_job(id, amount, brand, source_url, cookie, ua, "app.cr.bot", "https://app.cr.bot", "app.cr.bot:443:138.249.21.3");
        return;
    }

    spawn_take_job(id, amount, brand, source_url, cookie, ua, "app.send.tg", "https://app.send.tg", "app.send.tg:443:138.249.21.3");
}

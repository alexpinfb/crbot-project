#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <hiredis/hiredis.h>
#include <curl/curl.h>
#include "ws_readonly.h"
#include "ws_client.h"

static void load_env(const char *path) {
    FILE *f = fopen(path, "r");

    if (!f) {
        perror("fopen");
        return;
    }

    char line[4096];

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n')
            continue;

        char *eq = strchr(line, '=');

        if (!eq)
            continue;

        *eq = '\0';

        char *key = line;
        char *val = eq + 1;

        val[strcspn(val, "\r\n")] = '\0';

        setenv(key, val, 1);
    }

    fclose(f);
}

static int redis_ping(void) {
    const char *addr = getenv("REDIS_ADDR");
    const char *port_s = getenv("REDIS_PORT");
    const char *pass = getenv("REDIS_PASSWORD");

    int port = port_s ? atoi(port_s) : 6379;

    if (!addr || !pass) {
        fprintf(stderr, "missing REDIS_ADDR or REDIS_PASSWORD\n");
        return 1;
    }

    struct timeval timeout = { 3, 0 };
    redisContext *c = redisConnectWithTimeout(addr, port, timeout);

    if (!c || c->err) {
        fprintf(stderr, "redis connect error: %s\n", c ? c->errstr : "null context");
        if (c) redisFree(c);
        return 1;
    }

    redisReply *r = redisCommand(c, "AUTH %s", pass);

    if (!r || r->type == REDIS_REPLY_ERROR) {
        fprintf(stderr, "redis auth error: %s\n", r ? r->str : "null reply");
        if (r) freeReplyObject(r);
        redisFree(c);
        return 1;
    }

    freeReplyObject(r);

    r = redisCommand(c, "PING");

    if (!r) {
        fprintf(stderr, "redis ping null reply\n");
        redisFree(c);
        return 1;
    }

    printf("REDIS_PING=%s\n", r->str ? r->str : "(null)");

    freeReplyObject(r);
    redisFree(c);

    return 0;
}


static int redis_set_worker_info(void) {
    const char *addr = getenv("REDIS_ADDR");
    const char *port_s = getenv("REDIS_PORT");
    const char *pass = getenv("REDIS_PASSWORD");
    const char *worker = getenv("WORKER_ID");

    int port = port_s ? atoi(port_s) : 6379;

    if (!addr || !pass || !worker) {
        fprintf(stderr, "missing redis env or WORKER_ID\n");
        return 1;
    }

    redisContext *c = redisConnect(addr, port);

    if (!c || c->err) {
        fprintf(stderr, "redis connect error: %s\n", c ? c->errstr : "null context");
        if (c) redisFree(c);
        return 1;
    }

    redisReply *r = redisCommand(c, "AUTH %s", pass);

    if (!r || r->type == REDIS_REPLY_ERROR) {
        fprintf(stderr, "redis auth error: %s\n", r ? r->str : "null reply");
        if (r) freeReplyObject(r);
        redisFree(c);
        return 1;
    }

    freeReplyObject(r);

    char key[256];
    snprintf(key, sizeof(key), "crbot:workerInfo:%s", worker);

    char body[1024];
    snprintf(body, sizeof(body),
        "{\"workerId\":\"%s\",\"worker_id\":\"%s\",\"accountId\":\"c-test\",\"account_id\":\"c-test\",\"instance\":\"c-worker-test\",\"online\":true,\"provider\":\"c\",\"updated\":0}",
        worker, worker
    );

    r = redisCommand(c, "SET %s %s EX 15", key, body);

    if (!r || r->type == REDIS_REPLY_ERROR) {
        fprintf(stderr, "redis SET workerInfo error: %s\n", r ? r->str : "null reply");
        if (r) freeReplyObject(r);
        redisFree(c);
        return 1;
    }

    printf("WORKER_INFO_SET key=%s reply=%s\n", key, r->str ? r->str : "(null)");

    freeReplyObject(r);
    redisFree(c);

    return 0;
}


static int redis_get_auth(void) {
    const char *addr = getenv("REDIS_ADDR");
    const char *port_s = getenv("REDIS_PORT");
    const char *pass = getenv("REDIS_PASSWORD");
    const char *account = getenv("ACCOUNT_ID");

    int port = port_s ? atoi(port_s) : 6379;

    if (!addr || !pass || !account) {
        fprintf(stderr, "missing redis env or ACCOUNT_ID\n");
        return 1;
    }

    redisContext *c = redisConnect(addr, port);

    if (!c || c->err) {
        fprintf(stderr, "redis connect error: %s\n", c ? c->errstr : "null context");
        if (c) redisFree(c);
        return 1;
    }

    redisReply *r = redisCommand(c, "AUTH %s", pass);

    if (!r || r->type == REDIS_REPLY_ERROR) {
        fprintf(stderr, "redis auth error: %s\n", r ? r->str : "null reply");
        if (r) freeReplyObject(r);
        redisFree(c);
        return 1;
    }

    freeReplyObject(r);

    char cookie_key[256];
    char ua_key[256];

    snprintf(cookie_key, sizeof(cookie_key), "crbot:account:%s:cookie", account);
    snprintf(ua_key, sizeof(ua_key), "crbot:account:%s:userAgent", account);

    r = redisCommand(c, "GET %s", cookie_key);

    if (!r || r->type == REDIS_REPLY_NIL) {
        fprintf(stderr, "cookie not found key=%s\n", cookie_key);
        if (r) freeReplyObject(r);
        redisFree(c);
        return 1;
    }

    printf("AUTH_COOKIE key=%s len=%zu start=%.32s\n", cookie_key, strlen(r->str), r->str);
    setenv("CRBOT_COOKIE", r->str, 1);
    freeReplyObject(r);

    r = redisCommand(c, "GET %s", ua_key);

    if (!r || r->type == REDIS_REPLY_NIL) {
        fprintf(stderr, "userAgent not found key=%s\n", ua_key);
        if (r) freeReplyObject(r);
        redisFree(c);
        return 1;
    }

    printf("AUTH_UA key=%s len=%zu value=%s\n", ua_key, strlen(r->str), r->str);
    setenv("CRBOT_UA", r->str, 1);

    freeReplyObject(r);
    redisFree(c);

    return 0;
}


static int http_accounts_check(void) {
    const char *cookie = getenv("CRBOT_COOKIE");
    const char *ua = getenv("CRBOT_UA");

    if (!cookie || !ua) {
        fprintf(stderr, "missing CRBOT_COOKIE or CRBOT_UA\n");
        return 1;
    }

    CURL *curl = curl_easy_init();

    if (!curl) {
        fprintf(stderr, "curl_easy_init failed\n");
        return 1;
    }

    struct curl_slist *headers = NULL;
    char cookie_header[4096];
    char ua_header[1024];

    snprintf(cookie_header, sizeof(cookie_header), "Cookie: %s", cookie);
    snprintf(ua_header, sizeof(ua_header), "User-Agent: %s", ua);

    headers = curl_slist_append(headers, cookie_header);
    headers = curl_slist_append(headers, ua_header);
    headers = curl_slist_append(headers, "Origin: https://app.cr.bot");
    headers = curl_slist_append(headers, "Referer: https://app.cr.bot/p2c/payments?tab=active");
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, "https://app.cr.bot/internal/v1/p2c/accounts");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);

    CURLcode res = curl_easy_perform(curl);

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    printf("\nHTTP_ACCOUNTS code=%ld curl=%d\n", code, (int)res);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK && code >= 200 && code < 500) ? 0 : 1;
}

int main(void) {
    load_env(".env");

    const char *worker = getenv("WORKER_ID");
    const char *redis = getenv("REDIS_ADDR");

    printf("C worker boot\n");
    printf("WORKER_ID=%s\n", worker ? worker : "(null)");
    printf("REDIS_ADDR=%s\n", redis ? redis : "(null)");

    if (redis_ping() != 0) {
        return 1;
    }

    if (redis_get_auth() != 0) {
        return 1;
    }

    if (http_accounts_check() != 0) {
        fprintf(stderr, "http accounts check failed\n");
    }

    redis_set_worker_info();

    printf("C_WS_START\n");
    fflush(stdout);

    return ws_run_forever();
}

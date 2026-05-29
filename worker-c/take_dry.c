#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

#include <hiredis/hiredis.h>

static time_t last_take_ts = 0;
static char last_id[256] = {0};

static int amount_allowed(const char *amount_s) {

    const char *addr = getenv("REDIS_ADDR");
    const char *pass = getenv("REDIS_PASSWORD");
    const char *worker = getenv("WORKER_ID");

    if (!addr || !pass || !worker)
        return 0;

    double amount = atof(amount_s);

    redisContext *c = redisConnect(addr, 6379);

    if (!c || c->err)
        return 0;

    redisReply *auth = redisCommand(c, "AUTH %s", pass);

    if (auth)
        freeReplyObject(auth);

    char key[256];
    snprintf(key, sizeof(key), "crbot:worker:%s", worker);

    redisReply *r = redisCommand(c, "GET %s", key);

    if (!r || r->type != REDIS_REPLY_STRING) {
        if (r) freeReplyObject(r);
        redisFree(c);
        return 0;
    }

    double min = 0;
    double max = 0;
    int enabled = 0;

    sscanf(
        r->str,
        "{\"min\":%lf,\"max\":%lf,\"enabled\":%5s",
        &min,
        &max,
        (char[6]){0}
    );

    if (strstr(r->str, "\"enabled\":true"))
        enabled = 1;

    freeReplyObject(r);
    redisFree(c);

    if (!enabled)
        return 0;

    if (amount < min || amount > max)
        return 0;

    return 1;
}


static void save_candidate(const char *id, const char *amount, const char *brand) {
    const char *addr = getenv("REDIS_ADDR");
    const char *pass = getenv("REDIS_PASSWORD");
    const char *worker = getenv("WORKER_ID");

    if (!addr || !pass || !worker)
        return;

    redisContext *c = redisConnect(addr, 6379);
    if (!c || c->err)
        return;

    redisReply *auth = redisCommand(c, "AUTH %s", pass);
    if (auth) freeReplyObject(auth);

    char json[1024];
    snprintf(
        json,
        sizeof(json),
        "{\"id\":\"%s\",\"amount\":\"%s\",\"brand\":\"%s\",\"worker\":\"%s\",\"ts\":%ld}",
        id,
        amount,
        brand,
        worker,
        (long)time(NULL)
    );

    redisReply *r = redisCommand(c, "SETEX crbot:candidateTake:%s 300 %s", id, json);
    if (r) freeReplyObject(r);

    printf("TAKE_CANDIDATE_SAVE id=%s amount=%s worker=%s\n", id, amount, worker);
    fflush(stdout);

    redisFree(c);
}


int take_dry(const char *id, const char *amount, const char *brand) {

    if (!amount_allowed(amount)) {
        printf("TAKE_SKIP_FILTER id=%s amount=%s brand=%s\n", id, amount, brand);
        fflush(stdout);
        return 0;
    }

    time_t now = time(NULL);

    if (strcmp(last_id, id) == 0) {
        printf("TAKE_SKIP_DUP id=%s\n", id);
        fflush(stdout);
        return 0;
    }

    /* rate limit disabled for live C take */
    snprintf(last_id, sizeof(last_id), "%s", id);
    last_take_ts = now;

    printf(
        "TAKE_DRY id=%s amount=%s brand=%s url1=https://app.send.tg/internal/v1/p2c/payments/take/%s url2=https://app.cr.bot/internal/v1/p2c/payments/take/%s\n",
        id,
        amount,
        brand,
        id,
        id
    );

    fflush(stdout);

    save_candidate(id, amount, brand);

    return 1;
}

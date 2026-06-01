#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <hiredis/hiredis.h>

static char *account_prefix(const char *worker, char *out, size_t outsz) {
    if (!worker || !out || outsz < 3) return NULL;
    snprintf(out, outsz, "%.2s", worker);
    return out;
}

static int redis_auth(redisContext *c) {
    const char *pass = getenv("REDIS_PASSWORD");
    if (!pass || !c) return 0;
    redisReply *auth = redisCommand(c, "AUTH %s", pass);
    if (auth) freeReplyObject(auth);
    return 1;
}

static redisContext *redis_open(void) {
    const char *addr = getenv("REDIS_ADDR");
    if (!addr) return NULL;

    redisContext *c = redisConnect(addr, 6379);
    if (!c || c->err) {
        if (c) redisFree(c);
        return NULL;
    }

    redis_auth(c);
    return c;
}

static int amount_allowed(redisContext *c, const char *amount_s) {
    const char *worker = getenv("WORKER_ID");
    if (!c || !worker) return 0;

    double amount = atof(amount_s);

    char key[256];
    snprintf(key, sizeof(key), "crbot:worker:%s", worker);

    redisReply *r = redisCommand(c, "GET %s", key);
    if (!r || r->type != REDIS_REPLY_STRING) {
        if (r) freeReplyObject(r);
        return 0;
    }

    double min = 0;
    double max = 0;
    int enabled = strstr(r->str, "\"enabled\":true") != NULL;

    sscanf(r->str, "{\"min\":%lf,\"max\":%lf", &min, &max);

    freeReplyObject(r);

    if (!enabled) return 0;
    if (amount < min || amount > max) return 0;

    return 1;
}

static int active_order_exists(redisContext *c) {
    redisReply *r = redisCommand(c, "EXISTS crbot:activeOrder");
    int exists = 0;

    if (r && r->type == REDIS_REPLY_INTEGER && r->integer > 0)
        exists = 1;

    if (r) freeReplyObject(r);
    return exists;
}

static int take_cooldown_exists(redisContext *c) {
    redisReply *r = redisCommand(c, "EXISTS crbot:takeCooldown");
    int exists = 0;

    if (r && r->type == REDIS_REPLY_INTEGER && r->integer > 0)
        exists = 1;

    if (r) freeReplyObject(r);
    return exists;
}

static int shared_catching_enabled(redisContext *c) {
    redisReply *r = redisCommand(c, "GET crbot:catching");
    int enabled = 0;

    if (r && r->type == REDIS_REPLY_STRING && strcmp(r->str, "1") == 0)
        enabled = 1;

    if (r) freeReplyObject(r);
    return enabled;
}

static int mark_seen_take(redisContext *c, const char *id) {
    if (!c || !id || !id[0]) return 0;

    char key[256];
    snprintf(key, sizeof(key), "crbot:seenTake:%s", id);

    redisReply *r = redisCommand(c, "SET %s 1 NX EX 30", key);
    int ok = 0;

    if (r && r->type == REDIS_REPLY_STATUS && strcmp(r->str, "OK") == 0)
        ok = 1;

    if (r) freeReplyObject(r);
    return ok;
}

static int acquire_take_lock(redisContext *c, const char *id, const char *amount, const char *brand) {
    const char *worker = getenv("WORKER_ID");
    if (!c || !worker) return 0;

    char acc[16] = {0};
    account_prefix(worker, acc, sizeof(acc));

    char key[128];
    snprintf(key, sizeof(key), "crbot:takeLock");

    char body[1024];
    snprintf(
        body,
        sizeof(body),
        "{\"id\":\"%s\",\"amount\":\"%s\",\"brand\":\"%s\",\"worker\":\"%s\",\"account\":\"%s\",\"ts\":%ld}",
        id, amount, brand, worker, acc, (long)(time(NULL) * 1000)
    );

    redisReply *r = redisCommand(c, "SET %s %s NX PX 1500", key, body);
    int ok = 0;

    if (r && r->type == REDIS_REPLY_STATUS && strcmp(r->str, "OK") == 0)
        ok = 1;

    if (r) freeReplyObject(r);
    return ok;
}

int take_dry(const char *id, const char *amount, const char *brand) {
    redisContext *c = redis_open();
    if (!c) return 0;

    if (!amount_allowed(c, amount)) {
        redisFree(c);
        return 0;
    }

    if (!shared_catching_enabled(c)) {
        redisFree(c);
        return 0;
    }

    if (active_order_exists(c)) {
        redisFree(c);
        return 0;
    }

    if (take_cooldown_exists(c)) {
        redisFree(c);
        return 0;
    }

    if (!mark_seen_take(c, id)) {
        redisFree(c);
        return 0;
    }

    if (!acquire_take_lock(c, id, amount, brand)) {
        redisFree(c);
        return 0;
    }

    redisFree(c);

    printf("TAKE_LOCK_OK id=%s amount=%s brand=%s\n", id, amount, brand);
    fflush(stdout);

    return 1;
}

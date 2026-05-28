#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hiredis/hiredis.h>

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

    return redis_set_worker_info();
}

#include <stdio.h>
#include <string.h>
#include <time.h>

static time_t last_take_ts = 0;
static char last_id[256] = {0};

void take_dry(const char *id, const char *amount, const char *brand) {
    time_t now = time(NULL);

    if (strcmp(last_id, id) == 0) {
        printf("TAKE_SKIP_DUP id=%s\n", id);
        fflush(stdout);
        return;
    }

    if ((now - last_take_ts) < 5) {
        printf("TAKE_SKIP_RATE id=%s wait=%ld\n", id, (long)(5 - (now - last_take_ts)));
        fflush(stdout);
        return;
    }

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
}

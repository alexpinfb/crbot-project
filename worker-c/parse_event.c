
#include <stdio.h>

#include <string.h>

#include <stdlib.h>

static int extract_str(const char *s, const char *key, char *out, size_t out_sz) {

    char pat[128];

    snprintf(pat, sizeof(pat), "\"%s\":\"", key);

    const char *p = strstr(s, pat);

    if (!p) return 0;

    p += strlen(pat);

    const char *e = strchr(p, '"');

    if (!e) return 0;

    size_t n = (size_t)(e - p);

    if (n >= out_sz) n = out_sz - 1;

    memcpy(out, p, n);

    out[n] = 0;

    return 1;

}

int main(void) {

    char buf[65536];

    while (fgets(buf, sizeof(buf), stdin)) {

        if (!strstr(buf, "42[\"list:update\"")) continue;

        if (!strstr(buf, "\"op\":\"add\"")) continue;

        char id[256] = {0};

        char brand[512] = {0};

        char amount[128] = {0};

        extract_str(buf, "id", id, sizeof(id));

        extract_str(buf, "brand_name", brand, sizeof(brand));

        extract_str(buf, "in_amount", amount, sizeof(amount));

        if (id[0] && amount[0]) {

            printf("C_WS_EVENT id=%s amount=%s brand=%s\n", id, amount, brand);

        }

    }

    return 0;

}


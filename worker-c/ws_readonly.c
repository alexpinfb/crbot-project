#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int extract_str(const char *s, const char *key, char *out, size_t out_sz) {
    char pat[128];

    snprintf(pat, sizeof(pat), "\"%s\":\"", key);

    const char *p = strstr(s, pat);

    if (!p)
        return 0;

    p += strlen(pat);

    const char *e = strchr(p, '"');

    if (!e)
        return 0;

    size_t n = (size_t)(e - p);

    if (n >= out_sz)
        n = out_sz - 1;

    memcpy(out, p, n);

    out[n] = 0;

    return 1;
}

void parse_event(const char *msg) {
    if (!strstr(msg, "list:update"))
        return;

    char id[128] = {0};
    char amount[128] = {0};
    char brand[512] = {0};

    extract_str(msg, "id", id, sizeof(id));
    extract_str(msg, "in_amount", amount, sizeof(amount));
    extract_str(msg, "brand_name", brand, sizeof(brand));

    if (id[0]) {
        printf(
            "C_WS_EVENT id=%s amount=%s brand=%s\n",
            id,
            amount,
            brand
        );

        fflush(stdout);
    }
}

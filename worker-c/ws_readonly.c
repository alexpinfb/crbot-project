#include <stdio.h>
#include <stdlib.h>
#include "take_dry.h"
#include "take_http.h"
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

    if (!strstr(msg, "\"data\""))
        return;

    if (!strstr(msg, "\"data\""))
        return;

    char id[128] = {0};
    char amount[128] = {0};
    char brand[512] = {0};
    char provider[64] = {0};
    char url[512] = {0};

    extract_str(msg, "id", id, sizeof(id));
    extract_str(msg, "in_amount", amount, sizeof(amount));
    extract_str(msg, "brand_name", brand, sizeof(brand));
    extract_str(msg, "provider", provider, sizeof(provider));
    extract_str(msg, "url", url, sizeof(url));

    if (!id[0] || !amount[0] || !provider[0] || !url[0])
        return;

    if (!id[0])
        return;

    if (strcmp(provider, "nspk") != 0 || strncmp(url, "https://qr.nspk.ru/", 19) != 0) {
        return;
    }

    if (take_dry(id, amount, brand)) {
        printf("C_WS_EVENT id=%s amount=%s brand=%s provider=%s url=%s\n",
               id, amount, brand, provider, url);
        fflush(stdout);
        take_http_stub(id, amount, brand, url);
    }
}



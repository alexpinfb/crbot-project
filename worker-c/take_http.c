#include <stdio.h>
#include "take_http.h"

void take_http_stub(const char *id, const char *amount, const char *brand) {
    printf("TAKE_HTTP_STUB id=%s amount=%s brand=%s\n",
           id,
           amount,
           brand);
    fflush(stdout);
}

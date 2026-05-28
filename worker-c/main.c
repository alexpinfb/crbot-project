#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void) {
    load_env(".env");

    const char *worker = getenv("WORKER_ID");
    const char *redis = getenv("REDIS_ADDR");

    printf("C worker boot\n");
    printf("WORKER_ID=%s\n", worker ? worker : "(null)");
    printf("REDIS_ADDR=%s\n", redis ? redis : "(null)");

    return 0;
}

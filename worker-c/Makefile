.RECIPEPREFIX := >
CC=gcc
CFLAGS=-O2 -Wall -Wextra
LDFLAGS=-lhiredis -lcurl -lwebsockets

all:
>$(CC) $(CFLAGS) main.c ws_readonly.c ws_client.c -o worker-c $(LDFLAGS)

clean:
>rm -f worker-c parse_event ws_probe

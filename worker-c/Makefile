.RECIPEPREFIX := >
CC=gcc
CFLAGS=-O2 -Wall -Wextra
LDFLAGS=-lhiredis -lcurl

all:
>$(CC) $(CFLAGS) main.c ws_readonly.c -o worker-c $(LDFLAGS)

clean:
>rm -f worker-c parse_event ws_probe

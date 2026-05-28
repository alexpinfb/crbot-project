.RECIPEPREFIX := >
CC=gcc
CFLAGS=-O2 -Wall -Wextra
LDFLAGS=-lhiredis

all:
>$(CC) $(CFLAGS) main.c -o worker-c $(LDFLAGS)

clean:
>rm -f worker-c

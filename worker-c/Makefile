.RECIPEPREFIX := >

CC=gcc
CXX=g++

CFLAGS=-O2 -Wall -Wextra
CXXFLAGS=-O2 -Wall -Wextra

LD_C=-lhiredis -lcurl -lwebsockets
LD_CPP=-lboost_system -lssl -lcrypto -lpthread

all:
>$(CC) $(CFLAGS) main.c ws_readonly.c ws_client.c -o worker-c $(LD_C)

ws-cpp:
>$(CC) $(CFLAGS) -c ws_readonly.c -o ws_readonly.o
>$(CXX) $(CXXFLAGS) ws_cpp.cpp ws_readonly.o -o ws_cpp $(LD_CPP)

clean:
>rm -f worker-c ws_cpp ws_readonly.o parse_event ws_probe

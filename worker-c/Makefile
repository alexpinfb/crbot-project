.RECIPEPREFIX := >

CC=gcc
CXX=g++

CFLAGS=-O2 -Wall -Wextra
CXXFLAGS=-O0 -g0 -Wall -Wextra

LD_C=-lhiredis -lcurl -lwebsockets
LD_CPP=-lboost_system -lssl -lcrypto -lpthread -lhiredis -lcurl

all:
>$(CC) $(CFLAGS) main.c ws_readonly.c ws_client.c -o worker-c $(LD_C)

ws-cpp:
>$(CC) $(CFLAGS) -c ws_readonly.c -o ws_readonly.o
>$(CC) $(CFLAGS) -c take_dry.c -o take_dry.o
>$(CC) $(CFLAGS) -c take_http.c -o take_http.o
>$(CC) $(CFLAGS) -c http_keepalive.c -o http_keepalive.o
>$(CXX) $(CXXFLAGS) ws_cpp.cpp ws_readonly.o take_dry.o take_http.o http_keepalive.o -o ws_cpp $(LD_CPP)

clean:
>rm -f worker-c ws_cpp ws_readonly.o take_dry.o take_http.o http_keepalive.o parse_event ws_probe

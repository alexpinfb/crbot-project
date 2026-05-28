# worker-c

Experimental C worker.

Status:
- read-only first
- no TAKE
- no complete
- no production use yet

Milestones:
1. load .env
2. connect Redis
3. publish workerInfo heartbeat
4. connect WS
5. log WS_EVENT

Current WS status:

- wscat works with Cookie/Origin/User-Agent

- libwebsockets draft still gets HTTP 400 during upgrade

- next step: replace libwebsockets with ixwebsocket or raw TLS websocket handshake


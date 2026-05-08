# Workers setup

## Что такое worker

Worker — это отдельный сервер, который ловит заявки.

Главный сервер хранит Redis и Telegram/admin панель.
Worker подключается к главному серверу через Redis.

## Схема

MAIN SERVER:
- node-admin
- redis
- go-worker v1

WORKER SERVER:
- go-worker v2 / v3 / v4

## Как подключить worker к главному серверу

На worker сервере открыть:

nano /opt/crbot-project/go-worker/.env

Пример для второго worker:

COOKIE=...
WORKER_ID=v2
REDIS_URL=redis://MAIN_SERVER_IP:6379
PROVIDER=nspk

## Запуск worker

systemctl restart crbot-go-worker

## Проверка worker

systemctl status crbot-go-worker --no-pager -l

## Проверка в Redis

redis-cli -u redis://MAIN_SERVER_IP:6379 GET crbot:worker_status:v2

## Диапазоны

Диапазоны меняются через Telegram кнопку:

⚙️ Диапазоны

Или через Redis:

redis-cli SET crbot:worker:v2 '{"min":300,"max":50000,"enabled":true}'

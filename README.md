# CRBOT Project

Проект состоит из двух частей:

- node-admin — Telegram/admin панель
- go-worker — быстрый worker для ловли и take

## Установка

cd /opt/crbot-project
cp node-admin/.env.example node-admin/.env
cp go-worker/.env.example go-worker/.env

Заполнить:
- BOT_TOKEN
- CHAT_ID
- COOKIE
- REDIS_URL
- WORKER_ID

## Обновление

cd /opt/crbot-project
bash update.sh

## Логи

bash scripts/logs.sh

## Проверка статусов

redis-cli MGET crbot:worker_status:v1 crbot:worker_status:v2 crbot:worker_status:v3 crbot:worker_status:v4

## Диапазоны

redis-cli MGET crbot:worker:v1 crbot:worker:v2 crbot:worker:v3 crbot:worker:v4

## Важно

Не коммитить реальные .env, COOKIE, BOT_TOKEN.

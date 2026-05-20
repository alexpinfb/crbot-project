# CRBOT RELEASE

## Архитектура

- admin/   -> Telegram admin bot + Redis
- worker/  -> Go worker для аккаунта

Можно запускать:
- 1 admin server
- много worker servers

---

# 1. Установка ADMIN

## Ubuntu/Debian

cd admin
bash install-admin.sh

После установки:

nano .env

Заполнить:

BOT_TOKEN=
CHAT_ID=
COOKIE=
USER_AGENT=

Запуск:

systemctl restart crbot-admin
systemctl status crbot-admin

Логи:

journalctl -u crbot-admin -f

---

# 2. Получение CHAT_ID

Написать боту:

/start

Потом смотреть лог:

journalctl -u crbot-admin -f

Там будет:

CHAT_DEBUG unauthorized
chat_id: XXXXX

Этот ID вставить в .env:

CHAT_ID=XXXXX

---

# 3. Установка WORKER

На отдельном сервере:

cd worker
bash install-worker.sh

Потом:

nano .env

Заполнить:

REDIS_URL=
WORKER_ID=
COOKIE=
USER_AGENT=

Пример:

WORKER_ID=a1w1

Запуск:

systemctl restart crbot-go-worker
systemctl status crbot-go-worker

Логи:

tail -f live-go.log

---

# 4. COOKIE

В текущей версии cookie и user-agent задаются в worker/.env.

После обновления cookie:

nano .env
systemctl restart crbot-go-worker

В следующей версии cookie можно будет менять через Telegram bot и хранить в Redis.

---

# 5. Несколько аккаунтов

Пример:

a1w1
a1w2
a2w1
a2w2

Каждый worker:
- отдельный cookie
- отдельный account
- отдельные лимиты

---

# 6. Проверка

Redis:

redis-cli KEYS 'crbot:*'

Worker:

tail -f live-go.log

Admin:

journalctl -u crbot-admin -f


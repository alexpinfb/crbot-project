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

Cookie можно обновлять через Telegram bot:

1. Открыть меню
2. Нажать "🍪 Куки"
3. Выбрать "🍪 Куки A1" или "🍪 Куки A2"
4. Вставить полный COOKIE

Admin сохранит cookie в Redis:

- crbot:account:a1:cookie
- crbot:account:a1:userAgent
- crbot:account:a2:cookie
- crbot:account:a2:userAgent

Worker автоматически подхватывает новые cookie примерно за 3 секунды.
Restart worker не нужен.

Fallback: если в Redis cookie нет, worker использует COOKIE из worker/.env.

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


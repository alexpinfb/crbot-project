# CRBOT INSTALL GUIDE

## Архитектура

Проект состоит из двух частей:

- node-admin — Telegram/admin панель
- go-worker — быстрый worker

Система работает через Redis.

---

# MAIN SERVER

Главный сервер:

- Telegram bot
- Redis
- настройки
- stop/start
- диапазоны
- blacklist

---

# WORKER SERVER

Worker сервер:

- websocket
- take
- ловля заявок

---

# Установка

## 1. Клонирование

```bash
git clone https://github.com/alexpinfb/crbot-project.git /opt/crbot-project
cd /opt/crbot-project
bash install.sh

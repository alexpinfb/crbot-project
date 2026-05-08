# Telegram setup

## Как получить BOT_TOKEN

1. Открыть Telegram
2. Найти @BotFather
3. Написать /newbot
4. Придумать имя бота
5. Скопировать token

BOT_TOKEN вставить сюда:

node-admin/.env

Пример:

BOT_TOKEN=123456:ABCDEF

## Как получить CHAT_ID

1. Написать своему боту /start
2. Открыть в браузере:

https://api.telegram.org/botBOT_TOKEN/getUpdates

3. Найти строку:

"chat":{"id":123456789

4. Вставить число в:

node-admin/.env

Пример:

CHAT_ID=123456789

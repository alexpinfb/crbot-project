# Access and updates

## Как выдавать доступ другу

Не выдавать свой GitHub token.

Правильный способ:

GitHub -> Repository -> Settings -> Collaborators -> Add people

Добавить GitHub username друга.

## Как отозвать доступ

GitHub -> Repository -> Settings -> Collaborators -> Remove

## Как друг устанавливает проект

git clone https://github.com/alexpinfb/crbot-project.git /opt/crbot-project
cd /opt/crbot-project
bash install.sh

## Как обновлять проект

Владелец:

git add .
git commit -m "update"
git push

Пользователь:

cd /opt/crbot-project
bash update.sh

## Важно

Никогда не передавать:
- GitHub Personal Access Token
- COOKIE
- BOT_TOKEN
- чужой REDIS_URL

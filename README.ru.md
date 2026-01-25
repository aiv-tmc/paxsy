# Paxsy

Проект **paxsy** свободно доступен по лицензии MIT. Вы можете прочитать её по [этой ссылке](https://github.com/aiv-tmc/Paxsi/blob/main/LICENSE).

<!--Статус версий-->
## Статус версий
Новейшая: [Beta version 4 BlackBerry - v0.4.1](https://github.com/aiv-tmc/Paxsy/tree/beta-4.1_2A-Rowan)

Текущая: [Beta version 4 BlackBerry - v0.4.1](https://github.com/aiv-tmc/Paxsy/tree/beta-4.1_2A-Rowan)

Стабильная: [Beta version 4 BlackBerry - v0.4.1](https://github.com/aiv-tmc/Paxsy/tree/beta-4.1_2A-Rowan)

<!--Установка-->
## Установка (Linux)
У вас должны быть установлены [зависимости проекта](https://github.com/aiv-tmc/Paxsy#dependencies).

1.  Клонируйте репозиторий:
    `$ git clone https://github.com/aiv-tmc/Paxsy.git`

2.  Перейдите в директорию с кодом:
    `$ cd ~/Download/Paxsy/src/`

3.  Начните сборку программы:
    `$ sudo make all`

<!--Подсветка синтаксиса-->
## Подсветка синтаксиса (vim)
Чтобы добавить подсветку синтаксиса в **vim**:

1.  Переместите файл **paxsy.vim** в: `~/.vim/syntax/`

2.  Переместите содержимое файла из **vim_highlight/ftdetect/** в: `~/.vim/ftdetect/`

3.  Убедитесь, что в файле `~/.vimrc` **установлена цветовая схема industry**

Примечание: Чтобы активировать пользовательскую подсветку, в файле `~/.vim/syntax/paxsy.vim` закомментируйте строки **67-80** (опционально также строки **27**, **33** и **18-20**).

<!--Документация-->
## Документация
<!--Документацию можно получить по [этой ссылке](./docs/doc-en.md).-->
На данный момент документация ещё не готова.

<!--Зависимости-->
## Зависимости
Эта программа зависит от версии интерпретатора **gcc** **3.2** или выше.

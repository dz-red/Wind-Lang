# Wind

Собственный язык программирования. Транспилируется в C, собирается системным `gcc` — на выходе нативный бинарник со стартом около 2 мс.

Компилятор написан с нуля на чистом Си, около 3500 строк. Никаких генераторов парсеров, никаких сторонних зависимостей кроме `libcurl` для HTTP.

## Как выглядит

Веб-сервер с маршрутизацией — целиком:

```wnd
dict[str,str].info
info["lang"] = "Wind"

http.serve 8080
    on "/"     -> "<h1>Wind web server</h1>"
    on "/json" -> json.encode(info)
    on "/time" -> str(time.now())
end
```

Функции, исключения и строковая интерполяция:

```wnd
wnd.func validate_port(int.p) -> str
    if int.p < 1 || int.p > 65535
        throw "порт вне диапазона"
    end if
    return "обычный"
end wnd.func

try
    str.kind = validate_port(8080)
    terminal.paste -> "порт 8080 — {str.kind}"
catch err
    terminal.paste -> "ошибка: {str.err}"
end try
```

## Как устроено

```
файл.wnd
   ↓  lexer.c        разбор на токены, переводы строк значимы
   ↓  astparse.c     построение дерева
   ↓  astcodegen.c   генерация C-кода
output.c
   ↓  gcc -O3 -lm -lcurl
./app
```

Типы объявляются перед именем: `int.x`, `str.name`, `dict[str,int].cfg`. Блоки закрываются явно — `end if`, `end wnd.func`, `end try`.

## Сборка

```sh
git clone https://github.com/dz-red/Wind-Lang.git
cd Wind-Lang
make                    # получаем ./wind

./wind file.wnd         # собрать в ./app
./wind -s file.wnd      # собрать и сразу запустить
```

Зависимости: `gcc`, `make`, `libcurl-dev`.

## Что в репозитории

```
src/            компилятор: лексер, AST-парсер, генератор C, обработка ошибок
LANGUAGE.md     полная шпаргалка по языку на 500+ строк
demo.wnd        HTTP-сервер
backend.wnd     пример побольше
jregress.wnd    регрессионные проверки JSON
wind-vscode/    расширение для VS Code: подсветка, иконки файлов
templates/      HTML-шаблоны для веб-примеров
```

Полное описание синтаксиса, стандартной библиотеки и ограничений — в **[LANGUAGE.md](LANGUAGE.md)**.

## Что язык не умеет

Честно, чтобы не тратить чужое время: нет ООП, нет лямбд, нет многопоточности и async, коллекции только однотипные, `int` 32-битный, сборка только под Linux.

## Статус

Проект закрыт и в разработке не находится — лежит как есть. Написан в одиночку в 16 лет.

## Лицензия

MIT

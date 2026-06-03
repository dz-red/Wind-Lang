# Wind Language — Полная Шпаргалка

**Wind** — собственный язык программирования, транспилируется в C, компилируется системным `gcc`. Цель: убийца Python, но с нативной скоростью и строгой типизацией.

- Расширение файлов: `.wnd`
- Компилятор: `./wind` (написан на чистом Си, ~4700 строк)
- Сборка вашего кода: `./wind file.wnd` → создаёт бинарник `./app`
- Сборка + запуск: `./wind -s file.wnd`

---

## 📑 Содержание

1. [Базовый синтаксис](#базовый-синтаксис)
2. [Ввод и вывод](#ввод-и-вывод)
3. [Управление потоком](#управление-потоком)
4. [Функции](#функции)
5. [Массивы (static)](#статические-массивы)
6. [Списки (list)](#динамические-списки-list)
7. [Словари (dict)](#словари-dict)
8. [Строковые операции](#строковые-операции)
9. [Математика](#математика)
10. [Работа с файлами](#работа-с-файлами)
11. [JSON](#json)
12. [HTTP клиент](#http-клиент)
13. [Обработка ошибок](#обработка-ошибок)
14. [Модули и импорт](#модули-и-импорт)
15. [Random](#random)
16. [Комментарии](#комментарии)
17. [Полный пример](#полный-пример)
18. [Известные ограничения](#известные-ограничения)

---

## Базовый синтаксис

### Три типа данных:

```wnd
int.x = 42
frac.y = 3,14
str.name = "Wind"
```

- `int` — целое число (C `int`)
- `frac` — дробное (C `double`), **разделитель — запятая** (`3,14`, не точка!)
- `str` — динамическая строка (C `char*` с автоматическим malloc/free)

### Особенности

- **Скобки группировки в выражениях** — квадратные `[...]`, не круглые:
  ```wnd
  int.r = [2 + 3] * 4    // даст 20
  int.q = 2 + 3 * 4      // даст 14
  ```
- **Остаток деления** — `/%`, не `%`:
  ```wnd
  int.r = 7 /% 3         // 1
  ```
- **Касты типов** — `int(x)`, `frac(x)`:
  ```wnd
  int.n = int("42")          // парсит число из строки
  frac.f = frac("3,14")      // парсит дробь со запятой
  int.i = int(frac.y)        // дробь → целое
  ```

---

## Ввод и вывод

### Вывод

```wnd
terminal.paste -> "Hello, World!"
terminal.paste -> "x = {int.x}, name = {str.name}"   // интерполяция
terminal.clear                                         // очистить терминал
```

В `{...}` внутри строки можно вставить:
- `{int.x}` / `{frac.x}` / `{str.x}` — переменные
- `{int.nums[0]}` — элементы массива/list

### Ввод

```wnd
int.age = str.write {"Сколько тебе лет? "}     // с приглашением
str.name = str.write {"Имя: "}
int.x = write()                                 // тихий ввод без приглашения
```

Runtime сам валидирует тип. Введёшь букву где ждётся `int` → программа упадёт с понятной ошибкой.

---

## Управление потоком

### Условия

```wnd
if int.x > 0
    terminal.paste -> "положительное"
else if int.x < 0
    terminal.paste -> "отрицательное"
else
    terminal.paste -> "ноль"
end if
```

**Логические операторы:** `&&`, `||`, **скобки группировки** `[...]`:
```wnd
if [int.a > 0 && int.b < 10] || int.c == 99
    terminal.paste -> "ок"
end if
```

**Сравнения:** `>`, `<`, `>=`, `<=`, `==`, `!=`. Для строк работают только `==` и `!=`.

**Truthy-условия** (без сравнения):
```wnd
if cnt.has("key")          // если возвращает не 0
    terminal.paste -> "есть"
end if
```

### Циклы

**`repeat N`** — повторить N раз (фиксированное число):
```wnd
repeat 5
    terminal.paste -> "hi"
end repeat
```

**`while`** — пока условие истинно:
```wnd
int.i = 0
while int.i < 10
    int.i = int.i + 1
end while
```

**`loop V in COLL`** — итерация по list/dict:
```wnd
int.list.nums = [10, 20, 30]
loop n in nums
    terminal.paste -> "n = {int.n}"
end loop
```

**`break`** и **`continue`** работают во всех трёх циклах.

---

## Функции

```wnd
wnd.func square(int.x) -> int
    return int.x * int.x
end wnd.func

wnd.func greet(str.name) -> str
    str.name = "Привет, " + str.name + "!"
    return str.name
end wnd.func

wnd.func nothing() -> void
    terminal.paste -> "ничего не возвращает"
end wnd.func

// Использование:
int.r = square(5)
str.g = greet("Wind")
wnd.run nothing                      // void-функцию через wnd.run
```

- Возвращаемый тип обязателен (`-> int/frac/str/void`)
- До 8 параметров
- Рекурсия поддерживается
- `str`-параметры можно безопасно переприсваивать внутри функции

---

## Статические массивы

Только `int[]` и `frac[]`, фиксированный размер:

```wnd
int.nums[5]              // массив на 5 элементов, заполнен нулями
int.nums[0] = 10
int.nums[1] = 20
int.x = int.nums[0]      // чтение
int.len = len(nums)      // 5
```

Для динамических — используй **list** (см. ниже).

---

## Динамические списки (list)

**Гомогенные** — все элементы одного типа.

### Создание и базовые операции

```wnd
int.list.nums = [10, 20, 30]
str.list.words = ["один", "два", "три"]
frac.list.fracs = [1,5, 2,5, 3,5]

int.list.empty = []                  // пустой

// Чтение элемента — два способа:
int.x = int.list.nums[0]             // с префиксом
int.y = nums[0]                      // bare access (короче)

// Запись:
int.list.nums[1] = 99                // только с префиксом

// Методы:
nums.add(40)                         // в конец
nums.pop()                           // удалить последний
nums.remove(0)                       // удалить по индексу

// Длина:
int.n = len(nums)

// Итерация:
loop v in nums
    terminal.paste -> "{int.v}"
end loop
```

---

## Словари (dict)

**Гомогенные** — все ключи одного типа, все значения одного типа. Поддерживаются **все 9 комбинаций** (int/frac/str × int/frac/str).

```wnd
dict[str, int].cfg = ["port": 8080, "max": 100, "timeout": 30]
dict[int, str].byId = [1: "alice", 2: "bob"]
dict[str, str].env = ["HOME": "/home/user", "SHELL": "bash"]
dict[str, frac].prices = ["apple": 0,5, "milk": 2,75]

dict[str, int].empty = []            // пустой

// Чтение:
int.p = cfg["port"]
str.a = byId[1]

// Запись:
cfg["new_key"] = 42                  // bare
dict[str, int].cfg["other"] = 1      // с префиксом

// Методы:
int.ok = cfg.has("port")             // 0/1
cfg.delete("max")                    // удалить ключ (no-op если нет)

// Длина:
int.n = len(cfg)

// Итерация по ключам:
loop k in cfg
    int.v = cfg[str.k]
    terminal.paste -> "{str.k} = {int.v}"
end loop
```

**Доступ к несуществующему ключу = runtime error** (как Python KeyError). Проверяй через `.has()` или ловы через `try/catch`.

**Порядок итерации** — не гарантирован (это hashmap).

---

## Строковые операции

```wnd
str.t = "Hello, Wind!"

int.b = len(t)                       // 12 — длина в БАЙТАХ
int.c = chars(t)                     // 12 — длина в СИМВОЛАХ (UTF-8 aware)

// Срезы:
str.first5 = slice(t, 0, 5)          // "Hello" (по байтам)
str.chars3 = slice_chars(t, 0, 3)    // "Hel" (по символам, UTF-8)
str.last5 = slice(t, -5, len(t))     // "Wind!" (отрицательные индексы)

// Поиск (возвращает позицию или -1):
int.pos = find(t, "Wind")            // 7
int.miss = find(t, "Python")         // -1

// Замена (все вхождения):
str.r = replace(t, "Wind", "Mir")    // "Hello, Mir!"

// Split → str.list:
str.csv = "a,b,c,d"
str.list.parts = split(csv, ",")     // ["a","b","c","d"]

// Join → str:
str.joined = join(parts, "-")        // "a-b-c-d"

// Конкатенация:
str.full = "пре" + "фикс"
```

**Для русских/UTF-8 строк используй `chars()` и `slice_chars()`** — `len()` и `slice()` работают по байтам и могут разрезать середину символа.

---

## Математика

```wnd
frac.r = sqrt(16)                    // 4
frac.p = pow(2, 10)                  // 1024
frac.s = sin(PI)                     // ~0
frac.c = cos(0)                      // 1
frac.t = tan(PI / 4)                 // ~1

frac.l = log(E)                      // 1 (натуральный)
frac.e = exp(1)                      // 2.718...

frac.f = floor(3,7)                  // 3
frac.cl = ceil(3,2)                  // 4
frac.rd = round(3,5)                 // 4

frac.a = abs(-5,5)                   // 5.5
frac.mn = min(3, 7)                  // 3
frac.mx = max(3, 7)                  // 7
```

Константы: `PI`, `E`.

---

## Работа с файлами

```wnd
// Чтение всего файла:
str.text = file.read("config.txt")

// Чтение построчно (вернёт str.list):
str.list.lines = file.lines("log.txt")

// Проверка существования:
int.exists = file.exists("data.json")  // 0/1

// Запись (statement, перезапись):
file.write "out.txt" -> "содержимое"
file.write "out.txt" -> str.some_var

// Добавить в конец:
file.append "log.txt" -> "новая запись\n"
```

Если файл не открылся → runtime error (можно поймать через try/catch).

---

## JSON

**Только гомогенные** dict/list. Wind строго типизирован — гетерогенный JSON (с разными типами значений) не поддерживается.

```wnd
// Encode:
dict[str, int].cfg = ["port": 8080, "max": 100]
str.json = json.encode(cfg)
// "{"port":8080,"max":100}"

// Decode (нужно знать схему заранее):
dict[str, int].cfg2 = json.decode_str_int(json_text)
dict[str, str].env2 = json.decode_str_str(env_text)
```

---

## HTTP клиент

```wnd
// GET:
str.resp = http.get("https://api.github.com/users/torvalds")
int.code = http.status()                       // HTTP-код последнего ответа

// POST с form-data:
str.r = http.post("https://example.com/login", "user=foo&pass=bar")

// POST с JSON:
dict[str, str].payload = ["name": "Wind", "year": "2026"]
str.body = json.encode(payload)
str.r = http.post_json("https://api.example.com/users", body)

// PUT / DELETE:
str.r = http.put("https://api.example.com/users/1", body)
str.r = http.delete("https://api.example.com/users/1")
```

Все ошибки сети (timeout, DNS, SSL) бросают исключения — лови через `try/catch`.

---

## Обработка ошибок

```wnd
try
    str.text = file.read("missing.txt")
    terminal.paste -> "не дойдём сюда"
catch err
    terminal.paste -> "поймали: {str.err}"
end try

// Custom throw:
wnd.func divide(int.a, int.b) -> int
    if int.b == 0
        throw "деление на ноль"
    end if
    return int.a / int.b
end wnd.func

try
    int.r = divide(10, 0)
catch err
    terminal.paste -> "ошибка: {str.err}"
end try
```

**Что бросает исключения автоматически:**
- Чтение элемента list/dict за пределами
- Несуществующий ключ dict
- `file.read` / `file.write` если файл не открылся
- HTTP сетевые ошибки
- JSON parse errors
- `throw "..."` — вручную

**Что НЕ ловится:**
- Деление на ноль (SIGFPE) — программа умирает (TODO)
- Malloc failures

---

## Модули и импорт

```wnd
// в файле math.wnd:
wnd.func square(int.x) -> int
    return int.x * int.x
end wnd.func

// в основном файле:
st.import math                       // импорт всего namespace
int.r = math.square(5)               // вызов через namespace

st.import math.square                // импорт одной функции
int.r = square(5)                    // напрямую

st.import math.{square, cube}        // несколько функций
```

---

## Random

```wnd
int.r = random(1, 100)               // случайное число от 1 до 100
int.r2 = random[1, 100]              // то же, со скобками Wind-стиля
```

---

## Комментарии

**Парные** — открывают и закрывают:

```wnd
// это комментарий до конца строки //
int.x = 5

// многострочный
   тоже бывает //

terminal.paste -> "код"
```

---

## Полный пример

```wnd
// Скрипт: парсит конфиг из JSON и проверяет порт //

wnd.func validate_port(int.p) -> str
    if int.p < 1 || int.p > 65535
        throw "порт вне диапазона"
    end if
    if int.p < 1024
        return "привилегированный"
    end if
    return "обычный"
end wnd.func

try
    str.json = file.read("/tmp/config.json")
    dict[str, int].cfg = json.decode_str_int(json)
    
    if cfg.has("port")
        int.port = cfg["port"]
        str.kind = validate_port(int.port)
        terminal.paste -> "порт {int.port} — {str.kind}"
    else
        terminal.paste -> "в конфиге нет port"
    end if
catch err
    terminal.paste -> "ошибка: {str.err}"
end try

// Скачать список пользователей и сохранить //
try
    str.resp = http.get("https://api.example.com/users")
    file.write "/tmp/users.json" -> str.resp
    terminal.paste -> "сохранили"
catch err
    terminal.paste -> "API недоступно: {str.err}"
end try
```

---

## Известные ограничения

- **Гомогенные коллекции.** `[1, "hello"]` нельзя (mixed types). list и dict требуют один тип.
- **Гомогенный JSON только.** `{"name": "X", "year": 2026}` (string + int) не парсится — Wind строго типизирован.
- **Один поток.** Нет async/await, нет threads. Все операции блокирующие. Для скриптов норм, для нагруженных серверов — нет.
- **`int` 32-bit.** Большие числа не поддерживаются (нет `long`/`BigInt`).
- **Нет ООП.** Классов, наследования, методов на пользовательских типах — нет.
- **Нет лямбд.** Функции — first-citizen только через имя, не как значение.
- **Только Linux.** Windows .exe — в TODO.

---

## Сборка из исходников

```bash
git clone <repo>
cd Wind_Language_2026-05-26
make                                 # бинарник ./wind
./wind -s demo.wnd                   # тест
```

**Зависимости (Fedora):** `gcc`, `make`, `libcurl-devel`, `glibc-devel`.
**Зависимости (Ubuntu/Debian):** `gcc`, `make`, `libcurl4-openssl-dev`.

---

## Размер и архитектура

- **Компилятор:** ~4700 строк чистого Си (parser.c — главный диспатчер на ~2500 строк)
- **Транспиляция:** `.wnd` → `output.c` → `gcc -O3 -lm -lcurl` → `./app`
- **Стартовое время:** ~2мс (нативный бинарник)
- **Парсер:** построчный, без лексера и AST — каждая ветка в `translate_line` обрабатывает одну форму

---

*Сделано в одиночку в 16 лет за несколько недель. Версия 0.x — pre-launch. На pull-request'ы пока не открыто.*

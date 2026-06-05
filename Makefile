# Makefile для компилятора Wind.
#
# Команды:
#   make        — собрать wind (инкрементально: только изменённое перекомпилируется)
#   make clean  — снести бинарник, .o, .d файлы и output.c
#
# ВАЖНО: отступы в командах — ТОЛЬКО ТАБЫ. Не пробелы.

CC = gcc
# -MMD -MP заставляет gcc генерить рядом с .o файл .d со списком зависимостей
# (включая все подключённые .h). Эти .d мы подсасываем ниже через -include.
CFLAGS = -O2 -Wall -Isrc -MMD -MP
TARGET = wind

SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Подключаем сгенерированные правила зависимостей.
-include $(DEPS)

clean:
	rm -f $(TARGET) src/*.o src/*.d output_ast.c app

.PHONY: clean

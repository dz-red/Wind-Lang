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
WIND_DLOPEN_LIB := $(if $(findstring linux,$(shell $(CC) -dumpmachine)),-ldl)
WIND_CLANG_INCLUDE ?= $(patsubst %/clang-c/Index.h,%,$(lastword $(wildcard /usr/lib/llvm-*/include/clang-c/Index.h)))

SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET) $(WIND_DLOPEN_LIB)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

src/main.o: CPPFLAGS += -DWIND_RUNTIME_DIR='"$(abspath runtime)"'
ifneq ($(strip $(WIND_CLANG_INCLUDE)),)
src/cheader.o: CPPFLAGS += -I"$(WIND_CLANG_INCLUDE)"
endif

# Подключаем сгенерированные правила зависимостей.
-include $(DEPS)

clean:
	rm -f $(TARGET) src/*.o src/*.d output_ast.c app

.PHONY: clean

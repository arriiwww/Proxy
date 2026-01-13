# Makefile для HTTP-прокси
# Соответствует ручной команде: gcc -o proxy main.c proxy.c logger.c -lpthread -O2 -Wall

CC = gcc
CFLAGS = -O2 -Wall
LDFLAGS = -lpthread
TARGET = proxy
SRCS = main.c proxy.c logger.c
OBJS = $(SRCS:.c=.o)

# Основная цель
all: $(TARGET)

# Сборка исполняемого файла
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

# Автоматическое правило для компиляции .c -> .o
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Очистка
clean:
	rm -f $(OBJS) $(TARGET) proxy.log

# Пересборка
rebuild: clean all

# Запуск
run: $(TARGET)
	@echo "Запуск прокси на порту 80 (требует sudo)..."
	sudo ./$(TARGET)

# Факультативно: запуск на порту 8080 (без sudo)
run8080: $(TARGET)
	@echo "Запуск прокси на порту 8080..."
	sed -i.bak 's/listen_port = 80/listen_port = 8080/' main.c 2>/dev/null || true
	./$(TARGET)
	@mv main.c.bak main.c 2>/dev/null || true

.PHONY: all clean rebuild run run8080
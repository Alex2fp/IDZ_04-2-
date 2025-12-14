CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -pthread
SOURCES = main.c src/common.c src/semaphore_mode.c src/condition_mode.c
TARGET = talkers

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCES)

clean:
	rm -f $(TARGET)

run:
	./$(TARGET)


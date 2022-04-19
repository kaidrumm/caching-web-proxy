CC = gcc -g
SOURCES = webproxy.c
OBJECTS = $(SOURCES:.c=.o)
TARGET = all

$(TARGET): webproxy

.PHONY: clean fclean

clean_files:
	- @rm -f logs/*
	- @rm -rf cache/*

clean: clean_files
	- @rm -f webproxy $(OBJECTS)

fclean: clean

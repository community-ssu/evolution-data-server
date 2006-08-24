%: %.c
	$(CC) -g -Wall -O2 `pkg-config --cflags --libs libebook-1.2` -o $@ $^  -lcheck 

all: test-vcard-runner

test-vcard-runner: craphash.c test-vcard-parse.c test-vcard-photo.c test-vcard-runner.c
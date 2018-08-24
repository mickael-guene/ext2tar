LDFLAGS=-lext2fs -larchive
CFLAGS=-O2 -g

all: ext2tar

clean:
	rm -f *.o ext2tar

ext2tar: main.o
	$(CC) $^ -o $@ $(LDFLAGS)

install: ext2tar
	install -s $^ /usr/local/bin/$^
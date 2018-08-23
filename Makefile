LDFLAGS=-lext2fs -larchive

all: ext2tar

clean:
	rm -f *.o ext2tar

ext2tar: main.o
	$(CC) $^ -o $@ $(LDFLAGS)
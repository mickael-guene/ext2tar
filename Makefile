LDFLAGS=-lext2fs -larchive
CFLAGS=-O2 -g
INSTALL_DIR?=/usr/local/bin

all: ext2tar

clean:
	rm -f *.o ext2tar

ext2tar: main.o
	$(CC) $^ -o $@ $(LDFLAGS)

install: ext2tar
	install -s $< ${INSTALL_DIR}/$<
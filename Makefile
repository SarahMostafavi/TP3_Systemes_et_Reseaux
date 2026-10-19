CC = gcc
SRC = futosfs.c
BIN = futosfs
RES_DIR = Ressoures FS-20261019
IMAGE = "$(RES_DIR)/test_tosfs_files"
MOUNT_DIR = /tmp/futosfs

CFLAGS += -Wall -Wextra -g -I"$(RES_DIR)" $(shell pkg-config fuse --cflags)
LDLIBS += $(shell pkg-config fuse --libs)

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(BIN) $(LDLIBS)

run: $(BIN)
	mkdir -p $(MOUNT_DIR)
	./$(BIN) $(IMAGE) $(MOUNT_DIR) -d

umount:
	fusermount -u $(MOUNT_DIR)

clean:
	rm -f $(BIN)
	rm -f *.o
	rm -f *~

indent:
	indent -linux -i4 -nut -ts2 *.c

.PHONY: all run umount clean indent

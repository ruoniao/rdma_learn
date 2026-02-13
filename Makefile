CC := gcc
CFLAGS := -Wall -O2 -Iinclude
LDFLAGS := -lrdmacm -libverbs

SRC_DIR := src
BIN_DIR := bin

COMMON_SRC := $(SRC_DIR)/rdma_sim.c
SENDER_SRC := $(SRC_DIR)/sender.c
RECEIVER_SRC := $(SRC_DIR)/receiver.c

SENDER_BIN := $(BIN_DIR)/sender
RECEIVER_BIN := $(BIN_DIR)/receiver

.PHONY: all clean

all: $(SENDER_BIN) $(RECEIVER_BIN)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(SENDER_BIN): $(SENDER_SRC) $(COMMON_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(RECEIVER_BIN): $(RECEIVER_SRC) $(COMMON_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(SENDER_BIN) $(RECEIVER_BIN)

CC      ?= gcc
CFLAGS  := -std=c99 -Wall -Wextra -Werror -pedantic -O1
PROTO   := ../../firmware/components/fw_protocol
BUILD   := build

.PHONY: test clean

test: $(BUILD)/test_fw_protocol
	./$(BUILD)/test_fw_protocol

$(BUILD)/test_fw_protocol: test_fw_protocol.c $(PROTO)/fw_protocol.c $(PROTO)/include/fw_protocol.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(PROTO)/include test_fw_protocol.c $(PROTO)/fw_protocol.c -o $@

clean:
	rm -rf $(BUILD)

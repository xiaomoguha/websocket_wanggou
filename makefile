CC := gcc
CFLAGS := -lpthread -lwebsockets
SRC_DIR := src
OBJ_DIR := obj
BUILD_DIR := bin
SOURCES := $(wildcard $(SRC_DIR)/*.c)
OBJECTS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SOURCES))
all: $(BUILD_DIR)/websocket_server

$(BUILD_DIR)/websocket_server: $(OBJECTS)
	$(CC) -o $@ $^ $(CFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -rf $(OBJ_DIR)/*.o $(BUILD_DIR)/*
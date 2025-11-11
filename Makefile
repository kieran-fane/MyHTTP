# ---- Compiler & Flags ----
CC      := gcc
CFLAGS  := -std=c11 -O2 -Wall -Wextra -Werror=pedantic
CFLAGS  += -Iinclude
LDFLAGS := -pthread

# ---- Directories ----
SRC_DIR := src
OBJ_DIR := build
BIN     := MyHTTP

# ---- Sources & Objects ----
SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

# ---- Default Target ----
.PHONY: all
all: $(BIN)

# ---- Link Executable ----
$(BIN): $(OBJS)
	@echo "Linking $@"
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

# ---- Compile Objects ----
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	@echo "Compiling $<"
	$(CC) $(CFLAGS) -c $< -o $@

# ---- Ensure Build Directory ----
$(OBJ_DIR):
	@mkdir -p $(OBJ_DIR)

# ---- Run Target ----
.PHONY: run
run: $(BIN)
	@echo "Starting server..."
	./$(BIN)

# ---- Clean ----
.PHONY: clean
clean:
	@echo "Cleaning build artifacts..."
	rm -rf $(OBJ_DIR) $(BIN)

# ---- TEST ----
.PHONY: test
test: all
	@echo "Running Python tests..."
	python3 -m unittest discover -s test -t . -v


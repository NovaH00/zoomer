CXX := g++-14
FLAGS := -std=c++23 -Wall -Wextra $(shell pkg-config --cflags gio-2.0)
LIBS := -lSDL2 -lX11 -lXrandr $(shell pkg-config --libs gio-2.0)

BUILD_DIR := build
TARGET := zoomer
SRCS := src/main.cpp

$(BUILD_DIR)/$(TARGET): $(SRCS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(FLAGS) $(SRCS) $(LIBS) -o $(BUILD_DIR)/$(TARGET)

run: $(BUILD_DIR)/$(TARGET)
	./$(BUILD_DIR)/$(TARGET)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: run clean

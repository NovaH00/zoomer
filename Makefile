CXX := g++-14
FLAGS := -std=c++23 -Wall -Wextra
LIBS := -lSDL2 -lX11 -lXrandr

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

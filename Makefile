# Skrongle — simple Makefile (Linux/X11). For Windows/macOS use CMakeLists.txt.
CXX      ?= g++
CXXFLAGS ?= -O2 -fPIC -std=c++17 -Wall -Wextra -Wno-unused-parameter -pthread
INCLUDES  = -Iclap/include
LDFLAGS   = -shared -lX11 -lm -pthread

BUILD = build
SRCS  = src/plugin.cpp src/gui/gui_core.cpp src/gui/platform_x11.cpp
OBJS  = $(SRCS:%.cpp=$(BUILD)/%.o)

TARGET = $(BUILD)/skrongle.clap

all: $(TARGET) smoketest

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) $(LDFLAGS) -o $@

smoketest: tools/smoketest.cpp
	$(CXX) -O2 -std=c++17 $(INCLUDES) tools/smoketest.cpp -ldl -lm -o $(BUILD)/smoketest

test: all
	$(BUILD)/smoketest $(TARGET)

install: $(TARGET)
	mkdir -p $(HOME)/.clap
	cp $(TARGET) $(HOME)/.clap/

clean:
	rm -rf $(BUILD)

.PHONY: all clean test install smoketest

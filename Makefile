CXX ?= g++
SPINNAKER_ROOT ?= /opt/spinnaker

CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic
CPPFLAGS += -isystem $(SPINNAKER_ROOT)/include
LDFLAGS += -L$(SPINNAKER_ROOT)/lib -Wl,-rpath,$(SPINNAKER_ROOT)/lib
LDLIBS += -lSpinnaker -pthread

BUILD_DIR := build
TARGET := $(BUILD_DIR)/cam_website
SOURCES := src/main.cpp

.PHONY: all clean run rtcap

all: $(TARGET)

$(TARGET): $(SOURCES)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) -o $(TARGET) $(LDFLAGS) $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

rtcap: $(TARGET)
	sudo /sbin/setcap CAP_SYS_NICE,CAP_SYS_RESOURCE,CAP_NET_RAW+eip $(TARGET)

clean:
	rm -rf $(BUILD_DIR)

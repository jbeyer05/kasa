# C++ build with vendored nlohmann/json (probe + pipe mode).
CXX ?= c++
CXXFLAGS ?= -O3 -Wall -Wextra -I third_party
TARGET = kasa

$(TARGET): kasa.cpp
	$(CXX) $(CXXFLAGS) -o $(TARGET) kasa.cpp

.PHONY: clean
clean:
	rm -f $(TARGET)

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
TARGET    = tex2epub
SRC       = tex2epub.cpp

.PHONY: all debug clean

all: $(TARGET)

$(TARGET): $(SRC)
	    $(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET)

debug: CXXFLAGS = -std=c++17 -O0 -g3 -Wall -Wextra -Wpedantic -fsanitize=address,undefined
debug: $(TARGET)

clean:
	rm -f $(TARGET)

# Compiler and flags
CXX      = g++
CXXFLAGS = -std=c++11 -O2 -Wall -Wextra

# Target binary name and source file
TARGET   = yuv_cli
SRC      = yuv_inspector_cli-v2.cpp

# Default target
all: $(TARGET)

# Compile target binary
$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET)

# Clean up build artifacts and generated PPM frame exports
clean:
	rm -f $(TARGET) *.o *.ppm

# Show usage help for the built binary
help: $(TARGET)
	./$(TARGET) --help

.PHONY: all clean help
CXX = g++
CXXFLAGFS = -Wall -std=c++17 -O2

all: minlsp

minlsp: minlsp.cpp
	$(CXX) $(CXXFLAGS) minlsp.cpp -ominlsp

clean:
	rm -f minlsp

.PHONY: clean all

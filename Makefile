CXX      = g++
CXXFLAGS = -O2 -std=c++17 -Wno-stringop-overread
LIBS     = -lpthread -lhiredis -lsqlite3

all: server

server: server.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LIBS)

# 无 Redis（内存模式）编译
server-noredis: server.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< -lpthread -lsqlite3

clean:
	rm -f server server-noredis *.o

.PHONY: all clean

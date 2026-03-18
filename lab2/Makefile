CXX = g++
CXXFLAGS = -Wall -Wextra

all: tcp_server tcp_client

tcp_server: tcp_server.cpp protocol.hpp
	$(CXX) $(CXXFLAGS) -o $@ tcp_server.cpp

tcp_client: tcp_client.cpp protocol.hpp
	$(CXX) $(CXXFLAGS) -o $@ tcp_client.cpp

clean:
	rm -f tcp_server tcp_client

.PHONY: all clean


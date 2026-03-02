CXX = g++
CXXFLAGS = -Wall -Wextra

all: udp_echo_server udp_echo_client

udp_echo_server: udp_echo_server.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

udp_echo_client: udp_echo_client.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f udp_echo_server udp_echo_client

.PHONY: all clean

CXXFLAGS := -Wall -Wextra
LDFLAGS  := -lpthread -lm

TARGETS  := server client

.PHONY: all clean

all: $(TARGETS)

server: server.cpp protocol.h
	g++ $(CXXFLAGS) -o $@ $< $(LDFLAGS)

client: client.cpp protocol.h
	g++ $(CXXFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS) chat_history.json net_diag_*.json

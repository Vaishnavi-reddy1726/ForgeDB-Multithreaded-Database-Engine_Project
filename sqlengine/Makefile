CXX := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -pthread -Iinclude
LDFLAGS := -pthread

SERVER_SRC := src/main_server.cpp
CLIENT_SRC := client/main_client.cpp

BIN_DIR := bin
SERVER_BIN := $(BIN_DIR)/sqlengine_server
CLIENT_BIN := $(BIN_DIR)/sqlengine_client

.PHONY: all clean run-server run-client dirs test

all: dirs $(SERVER_BIN) $(CLIENT_BIN)

dirs:
	@mkdir -p $(BIN_DIR) data logs

$(SERVER_BIN): $(SERVER_SRC) include/*.hpp
	$(CXX) $(CXXFLAGS) $(SERVER_SRC) -o $(SERVER_BIN) $(LDFLAGS)

$(CLIENT_BIN): $(CLIENT_SRC)
	$(CXX) $(CXXFLAGS) $(CLIENT_SRC) -o $(CLIENT_BIN) $(LDFLAGS)

clean:
	rm -rf $(BIN_DIR) data/*.catalog logs/*.log

run-server: all
	./$(SERVER_BIN) --port 5432

run-client: all
	./$(CLIENT_BIN) --port 5432

test: all
	@bash tests/integration_test.sh

#include <iostream>
#include <csignal>
#include "database.hpp"
#include "server.hpp"

using namespace sqlengine;

static Server* g_server = nullptr;

void handleSignal(int) {
    if (g_server) {
        std::cout << "\n[server] shutting down...\n";
        g_server->stop();
    }
    std::exit(0);
}

int main(int argc, char** argv) {
    int port = 5432;
    std::string walPath = "logs/wal.log";
    std::string schemaPath = "data/schema.catalog";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) port = std::stoi(argv[++i]);
        else if (arg == "--wal" && i + 1 < argc) walPath = argv[++i];
        else if (arg == "--schema" && i + 1 < argc) schemaPath = argv[++i];
        else if (arg == "--help") {
            std::cout << "Usage: sqlengine_server [--port N] [--wal path] [--schema path]\n";
            return 0;
        }
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    try {
        std::cout << "[server] recovering from WAL (" << walPath << ") and schema catalog ("
                  << schemaPath << ")...\n";
        Database db(walPath, schemaPath);
        std::cout << "[server] recovery complete. tables=" << db.listTables().size() << "\n";

        Server server(db, port);
        g_server = &server;
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "[server] fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

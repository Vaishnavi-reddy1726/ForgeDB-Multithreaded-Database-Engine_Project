#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include "database.hpp"
#include "sql_engine.hpp"

namespace sqlengine {

// ------------------------------------------------------------------
// Multithreaded TCP server: accepts connections and spawns one
// detached worker thread per client. Each client gets its own Session
// (transaction context) while sharing the single Database instance,
// whose internal locking (row locks, index shared_mutex, WAL mutexes,
// cache mutex) makes it safe for concurrent access.
//
// Wire protocol: newline-terminated SQL statements in, newline-
// terminated text responses out (a "\n" delimited line protocol,
// simple enough for `nc`/telnet testing).
// ------------------------------------------------------------------
class Server {
public:
    Server(Database& db, int port) : db_(db), port_(port) {}

    void run() {
        int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd < 0) throw std::runtime_error("socket() failed");

        int opt = 1;
        setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            throw std::runtime_error("bind() failed on port " + std::to_string(port_));
        if (listen(listenFd, 128) < 0)
            throw std::runtime_error("listen() failed");

        std::cout << "[server] listening on 0.0.0.0:" << port_ << " (pid " << getpid() << ")\n";
        running_ = true;
        listenFd_ = listenFd;

        while (running_) {
            sockaddr_in clientAddr{};
            socklen_t clientLen = sizeof(clientAddr);
            int clientFd = accept(listenFd, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
            if (clientFd < 0) {
                if (!running_) break;
                continue;
            }
            connCount_.fetch_add(1);
            std::thread worker(&Server::handleClient, this, clientFd);
            worker.detach();
        }
        close(listenFd);
    }

    void stop() {
        running_ = false;
        if (listenFd_ >= 0) shutdown(listenFd_, SHUT_RDWR);
    }

    uint64_t connectionCount() const { return connCount_.load(); }

private:
    void handleClient(int fd) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        Session session;
        SqlEngine engine(db_);
        sendLine(fd, "OK connected. multithreaded-sql-engine ready.");

        std::string buffer;
        char chunk[4096];
        while (true) {
            ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0) break; // client closed or error
            buffer.append(chunk, n);

            size_t pos;
            while ((pos = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, pos);
                buffer.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                if (upper(line) == "QUIT" || upper(line) == "EXIT") {
                    sendLine(fd, "OK BYE");
                    close(fd);
                    return;
                }
                std::string response = engine.execute(session, line);
                sendLine(fd, response);
            }
        }
        // Client disconnected mid-transaction: roll back to release locks.
        if (session.inTxn) {
            try { db_.rollback(session.txnId); } catch (...) {}
        }
        close(fd);
    }

    static void sendLine(int fd, const std::string& msg) {
        std::string out = msg;
        out += "\n";
        // Encode embedded newlines (multi-row results) so the client's
        // line-based reader receives them as one logical message: we
        // use a sentinel terminator instead of relying on single '\n'.
        out += "--END--\n";
        size_t sent = 0;
        while (sent < out.size()) {
            ssize_t n = send(fd, out.data() + sent, out.size() - sent, 0);
            if (n <= 0) return;
            sent += static_cast<size_t>(n);
        }
    }

    Database& db_;
    int port_;
    int listenFd_ = -1;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> connCount_{0};
};

} // namespace sqlengine

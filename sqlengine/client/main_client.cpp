#include <iostream>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

// Simple line-protocol client: sends one SQL statement per line,
// reads back the response up to the "--END--" sentinel line.

static std::string readResponse(int fd) {
    std::string buffer;
    char chunk[4096];
    std::string out;
    while (true) {
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line == "--END--") return out;
            if (!out.empty()) out += "\n";
            out += line;
        }
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) return out;
        buffer.append(chunk, n);
    }
}

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = 5432;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = std::stoi(argv[++i]);
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { std::cerr << "socket() failed\n"; return 1; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        std::cerr << "Invalid host: " << host << "\n";
        return 1;
    }
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "connect() failed to " << host << ":" << port << "\n";
        return 1;
    }

    std::cout << readResponse(fd) << "\n";
    std::cout << "Connected to " << host << ":" << port << ". Type SQL statements, 'QUIT' to exit.\n";

    std::string line;
    std::cout << "sql> ";
    while (std::getline(std::cin, line)) {
        if (line.empty()) { std::cout << "sql> "; continue; }
        std::string toSend = line + "\n";
        send(fd, toSend.data(), toSend.size(), 0);
        std::string resp = readResponse(fd);
        std::cout << resp << "\n";
        if (line == "QUIT" || line == "quit" || line == "EXIT" || line == "exit") break;
        std::cout << "sql> ";
    }
    close(fd);
    return 0;
}

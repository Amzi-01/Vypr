#include "control.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace vypr {

namespace { bool g_wsa = false; }

Control::~Control() { close(); }

bool Control::connect(const char* host, std::uint16_t port) {
    if (!g_wsa) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        g_wsa = true;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        closesocket(s);
        return false;
    }

    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::fprintf(stderr, "vypr: connect to %s:%u failed: %d\n", host, port, WSAGetLastError());
        closesocket(s);
        return false;
    }

    // Input is a stream of small messages whose value is entirely in arriving
    // now. Nagle would coalesce them into batches and add up to 40ms.
    BOOL one = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));

    sock_ = static_cast<std::uint64_t>(s);
    return true;
}

void Control::close() {
    if (sock_ != ~0ull) {
        closesocket(static_cast<SOCKET>(sock_));
        sock_ = ~0ull;
    }
}

bool Control::send(std::uint16_t type, const void* payload, std::uint32_t bytes) {
    if (sock_ == ~0ull) return false;
    if (bytes > VYPR_MAX_MSG_BYTES) return false;

    vypr_msg_head head{};
    head.bytes = bytes;
    head.type  = type;
    head.flags = 0;

    // One send for header and payload: two sends with TCP_NODELAY put two
    // packets on the wire for every message.
    std::vector<std::uint8_t> buf(sizeof(head) + bytes);
    std::memcpy(buf.data(), &head, sizeof(head));
    if (bytes) std::memcpy(buf.data() + sizeof(head), payload, bytes);

    const char* p = reinterpret_cast<const char*>(buf.data());
    int left = static_cast<int>(buf.size());
    while (left > 0) {
        int n = ::send(static_cast<SOCKET>(sock_), p, left, 0);
        if (n <= 0) { close(); return false; }
        p += n;
        left -= n;
    }
    return true;
}

bool Control::send_window(std::uint16_t type, const vypr_msg_window& w,
                          const std::string& title) {
    vypr_msg_window msg = w;
    msg.title_bytes = static_cast<std::uint32_t>(title.size());

    std::vector<std::uint8_t> buf(sizeof(msg) + title.size());
    std::memcpy(buf.data(), &msg, sizeof(msg));
    std::memcpy(buf.data() + sizeof(msg), title.data(), title.size());
    return send(type, buf.data(), static_cast<std::uint32_t>(buf.size()));
}

bool Control::recv_exact(void* dst, std::uint32_t bytes) {
    char* p = static_cast<char*>(dst);
    while (bytes > 0) {
        int n = ::recv(static_cast<SOCKET>(sock_), p, static_cast<int>(bytes), 0);
        if (n <= 0) return false;
        p += n;
        bytes -= static_cast<std::uint32_t>(n);
    }
    return true;
}

void Control::run(const Handler& on_message) {
    while (sock_ != ~0ull) {
        vypr_msg_head head{};
        if (!recv_exact(&head, sizeof(head))) break;

        if (head.bytes > VYPR_MAX_MSG_BYTES) {
            std::fprintf(stderr, "vypr: control message of %u bytes; dropping link\n",
                         head.bytes);
            break;
        }

        rx_.resize(head.bytes);
        if (head.bytes && !recv_exact(rx_.data(), head.bytes)) break;

        on_message(head.type, rx_.data(), head.bytes);
    }
    close();
}

}  // namespace vypr

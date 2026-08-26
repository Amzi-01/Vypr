// sash-testagent - runs the guest agent's real publish path on Linux.
//
// This links the same publisher.cpp the Windows agent will, against the same
// host client, so the seqlock ordering and ring arithmetic are exercised for
// real before any of it depends on a Windows toolchain existing. What stays
// unproven after this is only capture, mapping and input - not the frame
// handoff, which is the part that fails subtly rather than loudly.
//
// It plays both roles: the host's allocator carves the slot, then the guest's
// Publisher fills it.
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "publisher.hpp"
extern "C" {
#include "msg.h"
#include "shm.h"
}

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop = true; }

static std::uint64_t now_ns() {
    return (std::uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Draws the same pattern the standalone mode does.
static void draw(std::uint8_t* dst, std::uint32_t w, std::uint32_t h,
                 std::uint32_t stride, std::uint32_t f) {
    for (std::uint32_t y = 0; y < h; y++) {
        std::uint8_t* row = dst + (std::size_t)y * stride;
        for (std::uint32_t x = 0; x < w; x++) {
            row[x * 4 + 0] = (std::uint8_t)((x * 2 + f * 4) & 0xff);
            row[x * 4 + 1] = (std::uint8_t)((y + f) & 0xff);
            row[x * 4 + 2] = (std::uint8_t)(((x ^ y) - f * 3) & 0xff);
            row[x * 4 + 3] = 0xff;
        }
    }
}

// Control mode: behave like the Windows agent. Announce a window, wait to be
// attached, publish into whatever slot the daemon assigns, and report the input
// that comes back. This exercises sashd, slot allocation, client spawning and
// the input return path without a VM.
static int run_connected(const std::string& host, std::uint16_t port,
                         const std::string& shm_path, const std::string& title,
                         std::uint32_t w, std::uint32_t h, std::uint32_t fps) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { std::perror("socket"); return 1; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "sash-testagent: bad address %s\n", host.c_str());
        return 1;
    }
    if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::perror("connect"); return 1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // Open the mapping before announcing, so HELLO can report its real size -
    // the daemon formatted the region before it started listening.
    struct sash_shm shm;
    if (sash_shm_open(&shm, shm_path.c_str(), 0) < 0) return 1;

    sash_msg_hello hello{};
    hello.version   = SASH_PROTO_VERSION;
    hello.qpc_freq  = 1000000000ull;
    hello.shm_bytes = shm.bytes;
    hello.agent_pid = (std::uint32_t)getpid();
    hello.capabilities = SASH_CAP_RESIZE;
    msg_send(fd, SASH_MSG_HELLO, &hello, sizeof(hello));

    // A stable fake HWND, so a reconnect looks like the same window.
    const std::uint64_t window_id = 0x5a5480000001ull;

    sash_msg_window desc{};
    desc.window_id = window_id;
    desc.width = w; desc.height = h;
    desc.dpi = 96; desc.pid = (std::uint32_t)getpid();
    desc.flags = SASH_WIN_RESIZABLE;
    desc.title_bytes = (std::uint32_t)title.size();

    std::vector<std::uint8_t> buf(sizeof(desc) + title.size());
    std::memcpy(buf.data(), &desc, sizeof(desc));
    std::memcpy(buf.data() + sizeof(desc), title.data(), title.size());
    msg_send(fd, SASH_MSG_WINDOW_ADDED, buf.data(), (std::uint32_t)buf.size());

    std::fprintf(stderr, "sash-testagent: announced '%s' %ux%u, waiting for attach\n",
                 title.c_str(), w, h);

    sash::Publisher pub;
    bool streaming = false;

    struct msg_reader rx{};
    const std::uint64_t period = 1000000000ull / (fps ? fps : 60);
    std::uint64_t next = now_ns();
    std::uint64_t input_events = 0;

    while (!g_stop) {
        std::uint64_t t = now_ns();
        int timeout = streaming ? (int)((next > t ? next - t : 0) / 1000000) : 200;

        pollfd p{ fd, POLLIN, 0 };
        if (poll(&p, 1, timeout) < 0) break;

        if (p.revents & (POLLIN | POLLHUP)) {
            if (msg_reader_fill(&rx, fd) < 0) {
                std::fprintf(stderr, "sash-testagent: daemon closed the link\n");
                break;
            }
            sash_msg_head head;
            const std::uint8_t* payload;
            while (msg_reader_next(&rx, &head, &payload) == 1) {
                switch (head.type) {
                case SASH_MSG_ATTACH: {
                    if (head.bytes < sizeof(sash_msg_attach)) break;
                    auto* at = (const sash_msg_attach*)payload;
                    sash_msg_attach_result res{};
                    res.window_id = at->window_id;
                    res.slot      = at->slot;
                    if (pub.bind(shm.base, shm.bytes, at->slot)) {
                        res.status = 0;
                        streaming = true;
                        next = now_ns();
                        std::fprintf(stderr,
                            "sash-testagent: attached to slot %u (%ux%u max)\n",
                            at->slot, pub.max_width(), pub.max_height());
                    } else {
                        res.status = -2;
                        std::fprintf(stderr, "sash-testagent: bind failed\n");
                    }
                    msg_send(fd, SASH_MSG_ATTACH_RESULT, &res, sizeof(res));
                    break;
                }
                case SASH_MSG_POINTER: {
                    if (head.bytes < sizeof(sash_msg_pointer)) break;
                    auto* m = (const sash_msg_pointer*)payload;
                    if (++input_events <= 5 || input_events % 100 == 0)
                        std::fprintf(stderr, "sash-testagent: pointer %d,%d buttons %u"
                                             " wheel %d\n", m->x, m->y, m->buttons, m->wheel);
                    break;
                }
                case SASH_MSG_KEY: {
                    if (head.bytes < sizeof(sash_msg_key)) break;
                    auto* m = (const sash_msg_key*)payload;
                    input_events++;
                    std::fprintf(stderr, "sash-testagent: key scancode 0x%02X %s\n",
                                 m->scancode, m->down ? "down" : "up");
                    break;
                }
                case SASH_MSG_FOCUS:
                    std::fprintf(stderr, "sash-testagent: focus\n");
                    break;
                case SASH_MSG_RESIZE: {
                    if (head.bytes < sizeof(sash_msg_resize)) break;
                    auto* m = (const sash_msg_resize*)payload;
                    std::fprintf(stderr, "sash-testagent: resize request %ux%u\n",
                                 m->width, m->height);
                    break;
                }
                case SASH_MSG_CLOSE:
                    std::fprintf(stderr, "sash-testagent: host closed the window\n");
                    g_stop = true;
                    break;
                case SASH_MSG_DETACH:
                    std::fprintf(stderr, "sash-testagent: detached\n");
                    streaming = false;
                    pub.close();
                    break;
                default:
                    break;
                }
            }
        }

        if (streaming && now_ns() >= next) {
            std::uint32_t stride = 0;
            if (std::uint8_t* dst = pub.begin_frame(&stride)) {
                draw(dst, w, h, stride, pub.serial());
                pub.publish(w, h, stride, now_ns(), 1000000000ull, SASH_PUB_DAMAGE_FULL);
            }
            next += period;
            if (next < now_ns()) next = now_ns();
        }
    }

    std::fprintf(stderr, "\nsash-testagent: %u frames, %llu input events\n",
                 pub.serial(), (unsigned long long)input_events);
    pub.close();
    msg_reader_free(&rx);
    sash_shm_close(&shm);
    close(fd);
    return 0;
}

int main(int argc, char** argv) {
    std::string path = "/dev/shm/sash-test";
    std::string connect_host, title = "sash test window";
    std::uint16_t port = SASH_CONTROL_PORT;
    std::uint32_t w = 1280, h = 720, fps = 60;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--shm" && i + 1 < argc)       path = argv[++i];
        else if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%ux%u", &w, &h);
        else if (a == "--fps" && i + 1 < argc)  fps = (std::uint32_t)std::atoi(argv[++i]);
        else if (a == "--connect" && i + 1 < argc) connect_host = argv[++i];
        else if (a == "--port" && i + 1 < argc) port = (std::uint16_t)std::atoi(argv[++i]);
        else if (a == "--title" && i + 1 < argc) title = argv[++i];
        else {
            std::fputs("usage: sash-testagent [--shm PATH] [--size WxH] [--fps N]\n"
                       "                      [--connect HOST [--port N] [--title T]]\n"
                       "\nWithout --connect it formats the region and publishes standalone.\n"
                       "With --connect it behaves like the guest agent against sashd.\n", stderr);
            return 2;
        }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    if (!connect_host.empty())
        return run_connected(connect_host, port, path, title, w, h, fps);

    int fd = open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) { std::perror("open"); return 1; }
    struct stat st{};
    if (fstat(fd, &st) == 0 && st.st_size < 256ll * 1024 * 1024)
        if (ftruncate(fd, 256ll * 1024 * 1024) < 0) { std::perror("ftruncate"); return 1; }
    close(fd);

    // Host half: format and carve.
    struct sash_shm shm;
    if (sash_shm_open(&shm, path.c_str(), 1) < 0) return 1;

    struct sash_msg_attach at;
    if (sash_shm_alloc(&shm, 0x5a5480000001ull, w, h, &at) < 0) return 1;

    // Guest half: bind and publish, exactly as the Windows agent will.
    sash::Publisher pub;
    if (!pub.bind(shm.base, shm.bytes, at.slot)) {
        std::fprintf(stderr, "sash-testagent: bind failed for slot %u\n", at.slot);
        return 1;
    }

    std::printf("sash-testagent: slot %u, %ux%u, stride %u\n", at.slot, w, h, pub.stride());
    std::printf("present it with:  ./build/sash-host --shm %s --slot %u --stats\n",
                path.c_str(), at.slot);

    const std::uint64_t period = 1000000000ull / (fps ? fps : 60);
    std::uint64_t next = now_ns();

    while (!g_stop) {
        std::uint32_t stride = 0;
        std::uint8_t* dst = pub.begin_frame(&stride);
        if (!dst) break;

        const std::uint32_t f = pub.serial();
        for (std::uint32_t y = 0; y < h; y++) {
            std::uint8_t* row = dst + (std::size_t)y * stride;
            for (std::uint32_t x = 0; x < w; x++) {
                row[x * 4 + 0] = (std::uint8_t)((x * 2 + f * 4) & 0xff);
                row[x * 4 + 1] = (std::uint8_t)((y + f) & 0xff);
                row[x * 4 + 2] = (std::uint8_t)(((x ^ y) - f * 3) & 0xff);
                row[x * 4 + 3] = 0xff;
            }
        }

        if (!pub.publish(w, h, stride, now_ns(), 1000000000ull, SASH_PUB_DAMAGE_FULL)) {
            std::fprintf(stderr, "sash-testagent: publish rejected\n");
            break;
        }

        next += period;
        std::uint64_t t = now_ns();
        if (next > t) usleep((useconds_t)((next - t) / 1000));
        else next = t;
    }

    std::printf("\nsash-testagent: published %u frames\n", pub.serial());
    pub.close();
    sash_shm_close(&shm);
    return 0;
}

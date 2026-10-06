// vypr-testagent - runs the guest agent's real publish path on Linux.
//
// This links the same publisher.cpp the Windows agent will, against the same
// host client, so the seqlock ordering and ring arithmetic are exercised for
// real before any of it depends on a Windows toolchain existing. What stays
// unproven after this is only capture, mapping and input - not the frame
// handoff, which is the part that fails subtly rather than loudly.
//
// Guest RAM is a sparse file laid out like a q35 guest's (see fake_ram.h), so
// with --connect the whole v3 handshake runs for real: the ring's pages are
// allocated scattered, stamped, sent, checked by vyprd, and only then written.
#include <algorithm>
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

#include <vector>

#include "fake_ram.h"
#include "publisher.hpp"
extern "C" {
#include "guest_ram.h"
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

// A static background with a white block, painted into one rectangle, matching
// draw() at f==0 everywhere outside the block. Lets the damage path be exercised
// end to end: if the host accumulates partial frames correctly, the block moves
// over an unchanging background with no trail.
static void draw_block_rect(std::uint8_t* dst, std::uint32_t stride,
                            std::uint32_t rx, std::uint32_t ry, std::uint32_t rw,
                            std::uint32_t rh, std::uint32_t bx, std::uint32_t by,
                            std::uint32_t bs) {
    for (std::uint32_t y = ry; y < ry + rh; y++) {
        std::uint8_t* row = dst + (std::size_t)y * stride;
        for (std::uint32_t x = rx; x < rx + rw; x++) {
            const bool in = (x >= bx && x < bx + bs && y >= by && y < by + bs);
            row[x * 4 + 0] = in ? 0xff : (std::uint8_t)((x * 2) & 0xff);
            row[x * 4 + 1] = in ? 0xff : (std::uint8_t)(y & 0xff);
            row[x * 4 + 2] = in ? 0xff : (std::uint8_t)((x ^ y) & 0xff);
            row[x * 4 + 3] = 0xff;
        }
    }
}

// Control mode: behave like the Windows agent. Announce a window, wait to be
// attached, publish into whatever slot the daemon assigns, and report the input
// that comes back. This exercises vyprd, slot allocation, client spawning and
// the input return path without a VM.
// One ring, owned the way the agent owns its own: allocated on ATTACH, written
// only after RING_READY, freed on DETACH.
struct TestRing {
    fake_ram*                  ram = nullptr;
    void*                      base = nullptr;
    std::uint32_t              pages = 0;
    std::vector<std::uint64_t> pfns;
    std::uint64_t              nonce = 0;
    vypr_msg_attach            at{};

    void release() {
        if (base) fake_ram_free(ram, base, pages, pfns.data());
        base = nullptr;
        pages = 0;
    }
};

// --bad-ring: spoil one page's stamp before sending, to prove the host refuses
// a ring it cannot verify and that both ends recover from it.
static bool g_bad_ring = false;

static int run_connected(const std::string& host, std::uint16_t port,
                         const std::string& ram_path, const std::string& title,
                         std::uint32_t w, std::uint32_t h, std::uint32_t fps) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { std::perror("socket"); return 1; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "vypr-testagent: bad address %s\n", host.c_str());
        return 1;
    }
    if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::perror("connect"); return 1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // Point vyprd at the same file with --guest-ram.
    fake_ram ram{};
    if (fake_ram_open(&ram, ram_path.c_str(), 3ull << 30, 2ull << 30) < 0) return 1;
    TestRing ring;
    ring.ram = &ram;

    vypr_msg_hello hello{};
    hello.version   = VYPR_PROTO_VERSION;
    hello.qpc_freq  = 1000000000ull;
    hello.agent_pid = (std::uint32_t)getpid();
    hello.capabilities = VYPR_CAP_RESIZE;
    msg_send(fd, VYPR_MSG_HELLO, &hello, sizeof(hello));

    // A stable fake HWND, so a reconnect looks like the same window.
    const std::uint64_t window_id = 0x5a5480000001ull;

    vypr_msg_window desc{};
    desc.window_id = window_id;
    desc.width = w; desc.height = h;
    desc.dpi = 96; desc.pid = (std::uint32_t)getpid();
    desc.flags = VYPR_WIN_RESIZABLE;
    desc.title_bytes = (std::uint32_t)title.size();

    std::vector<std::uint8_t> buf(sizeof(desc) + title.size());
    std::memcpy(buf.data(), &desc, sizeof(desc));
    std::memcpy(buf.data() + sizeof(desc), title.data(), title.size());
    msg_send(fd, VYPR_MSG_WINDOW_ADDED, buf.data(), (std::uint32_t)buf.size());

    std::fprintf(stderr, "vypr-testagent: announced '%s' %ux%u, waiting for attach\n",
                 title.c_str(), w, h);

    vypr::Publisher pub;
    bool streaming = false;
    bool damage_on = false;   /* the daemon asked for damage on this attach */
    const std::uint32_t bs = w > 128 ? 64 : 16;
    std::uint32_t prev_bx = 0, prev_by = 0;

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
                std::fprintf(stderr, "vypr-testagent: daemon closed the link\n");
                break;
            }
            vypr_msg_head head;
            const std::uint8_t* payload;
            while (msg_reader_next(&rx, &head, &payload) == 1) {
                switch (head.type) {
                case VYPR_MSG_ATTACH: {
                    if (head.bytes < sizeof(vypr_msg_attach)) break;
                    auto* at = (const vypr_msg_attach*)payload;
                    vypr_msg_attach_result res{};
                    res.window_id = at->window_id;
                    res.slot      = at->slot;

                    ring.release();
                    ring.at    = *at;
                    ring.pages = (std::uint32_t)(vypr_ring_bytes(at->frame_bytes) / VYPR_PAGE_BYTES);
                    ring.pfns.assign(ring.pages, 0);
                    ring.base  = fake_ram_alloc(&ram, ring.pages, ring.pfns.data());
                    if (!ring.base) {
                        ring.pages = 0;
                        res.status = VYPR_ATTACH_NO_MEMORY;
                        msg_send(fd, VYPR_MSG_ATTACH_RESULT, &res, sizeof(res));
                        break;
                    }
                    ring.nonce = now_ns() ^ ((std::uint64_t)getpid() << 32);
                    vypr::stamp_pages(ring.base, ring.pages, ring.pfns.data(), ring.nonce);
                    if (g_bad_ring)
                        static_cast<std::uint8_t*>(ring.base)[(ring.pages / 2) * VYPR_PAGE_BYTES + 16] ^= 1;

                    std::vector<std::uint8_t> m(VYPR_MAX_MSG_BYTES);
                    for (std::uint32_t first = 0; first < ring.pages; ) {
                        const std::uint32_t n = std::min<std::uint32_t>(VYPR_RING_PAGES_PER_MSG,
                                                                        ring.pages - first);
                        vypr_msg_ring_pages rp{};
                        rp.window_id   = at->window_id;
                        rp.slot        = at->slot;
                        rp.epoch       = at->generation;
                        rp.nonce       = ring.nonce;
                        rp.total_pages = ring.pages;
                        rp.first       = first;
                        rp.count       = n;
                        std::memcpy(m.data(), &rp, sizeof(rp));
                        std::memcpy(m.data() + sizeof(rp), ring.pfns.data() + first, n * 8u);
                        msg_send(fd, VYPR_MSG_RING_PAGES, m.data(), (std::uint32_t)(sizeof(rp) + n * 8u));
                        first += n;
                    }
                    res.status = 0;
                    msg_send(fd, VYPR_MSG_ATTACH_RESULT, &res, sizeof(res));
                    std::fprintf(stderr, "vypr-testagent: ring of %u pages sent for slot %u\n",
                                 ring.pages, at->slot);
                    break;
                }
                case VYPR_MSG_RING_READY: {
                    if (head.bytes < sizeof(vypr_msg_ring_ready)) break;
                    auto* rr = (const vypr_msg_ring_ready*)payload;
                    if (!ring.base || rr->window_id != ring.at.window_id) break;
                    if (rr->status != 0) {
                        std::fprintf(stderr, "vypr-testagent: host rejected the ring (%d)\n", rr->status);
                        ring.release();
                        break;
                    }
                    if (pub.bind(ring.base, (std::size_t)ring.pages * VYPR_PAGE_BYTES, ring.at, ring.nonce)) {
                        streaming = true;
                        damage_on = (ring.at.flags & VYPR_ATTACH_DAMAGE) != 0;
                        prev_bx = prev_by = 0;
                        next = now_ns();
                        std::fprintf(stderr,
                            "vypr-testagent: streaming into slot %u (%ux%u max)%s\n",
                            ring.at.slot, pub.max_width(), pub.max_height(),
                            damage_on ? ", damage on" : "");
                    }
                    break;
                }
                case VYPR_MSG_POINTER: {
                    if (head.bytes < sizeof(vypr_msg_pointer)) break;
                    auto* m = (const vypr_msg_pointer*)payload;
                    if (++input_events <= 5 || input_events % 100 == 0)
                        std::fprintf(stderr, "vypr-testagent: pointer %d,%d buttons %u"
                                             " wheel %d\n", m->x, m->y, m->buttons, m->wheel);
                    break;
                }
                case VYPR_MSG_KEY: {
                    if (head.bytes < sizeof(vypr_msg_key)) break;
                    auto* m = (const vypr_msg_key*)payload;
                    input_events++;
                    std::fprintf(stderr, "vypr-testagent: key scancode 0x%02X %s\n",
                                 m->scancode, m->down ? "down" : "up");
                    break;
                }
                case VYPR_MSG_FOCUS:
                    std::fprintf(stderr, "vypr-testagent: focus\n");
                    break;
                case VYPR_MSG_RESIZE: {
                    if (head.bytes < sizeof(vypr_msg_resize)) break;
                    auto* m = (const vypr_msg_resize*)payload;
                    std::fprintf(stderr, "vypr-testagent: resize request %ux%u\n",
                                 m->width, m->height);
                    break;
                }
                case VYPR_MSG_CLOSE:
                    std::fprintf(stderr, "vypr-testagent: host closed the window\n");
                    g_stop = true;
                    break;
                case VYPR_MSG_DETACH:
                    std::fprintf(stderr, "vypr-testagent: detached\n");
                    streaming = false;
                    pub.close();
                    ring.release();
                    break;
                default:
                    break;
                }
            }
        }

        if (streaming && now_ns() >= next) {
            std::uint32_t stride = 0;
            if (std::uint8_t* dst = pub.begin_frame(&stride)) {
                const bool partial = damage_on && (pub.serial() % 120 != 0);
                if (partial) {
                    const std::uint32_t span = w > bs ? w - bs : 1;
                    const std::uint32_t bx = (pub.serial() * 7) % span;
                    const std::uint32_t by = (h > bs) ? (pub.serial() * 3) % (h - bs) : 0;
                    vypr_rect d;
                    d.x = bx < prev_bx ? bx : prev_bx;
                    d.y = by < prev_by ? by : prev_by;
                    d.w = (bx > prev_bx ? bx - prev_bx : prev_bx - bx) + bs;
                    d.h = (by > prev_by ? by - prev_by : prev_by - by) + bs;
                    if (d.x + d.w > w) d.w = w - d.x;
                    if (d.y + d.h > h) d.h = h - d.y;
                    draw_block_rect(dst, stride, d.x, d.y, d.w, d.h, bx, by, bs);
                    pub.publish(w, h, stride, now_ns(), 1000000000ull,
                                VYPR_PUB_DAMAGE_RECTS, &d, 1);
                    prev_bx = bx; prev_by = by;
                } else {
                    draw(dst, w, h, stride, damage_on ? 0 : pub.serial());
                    pub.publish(w, h, stride, now_ns(), 1000000000ull, VYPR_PUB_DAMAGE_FULL);
                    if (damage_on) prev_bx = prev_by = 0;
                }
            }
            next += period;
            if (next < now_ns()) next = now_ns();
        }
    }

    std::fprintf(stderr, "\nvypr-testagent: %u frames, %llu input events\n",
                 pub.serial(), (unsigned long long)input_events);
    pub.close();
    ring.release();
    fake_ram_close(&ram);
    msg_reader_free(&rx);
    close(fd);
    return 0;
}

int main(int argc, char** argv) {
    std::string path = "/dev/shm/vypr-test-host";
    std::string ram_path = "/dev/shm/vypr-test-ram";
    std::string connect_host, title = "vypr test window";
    std::uint16_t port = VYPR_CONTROL_PORT;
    std::uint32_t w = 1280, h = 720, fps = 60;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--shm" && i + 1 < argc)       path = argv[++i];
        else if (a == "--guest-ram" && i + 1 < argc) ram_path = argv[++i];
        else if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%ux%u", &w, &h);
        else if (a == "--fps" && i + 1 < argc)  fps = (std::uint32_t)std::atoi(argv[++i]);
        else if (a == "--connect" && i + 1 < argc) connect_host = argv[++i];
        else if (a == "--port" && i + 1 < argc) port = (std::uint16_t)std::atoi(argv[++i]);
        else if (a == "--title" && i + 1 < argc) title = argv[++i];
        else if (a == "--bad-ring") g_bad_ring = true;
        else {
            std::fputs("usage: vypr-testagent [--shm PATH] [--guest-ram PATH] [--size WxH] [--fps N]\n"
                       "                      [--connect HOST [--port N] [--title T]]\n"
                       "\nWithout --connect it formats the slot table and publishes standalone.\n"
                       "With --connect it behaves like the guest agent against vyprd, which\n"
                       "must be started with --guest-ram pointing at the same file.\n", stderr);
            return 2;
        }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    if (!connect_host.empty())
        return run_connected(connect_host, port, ram_path, title, w, h, fps);

    // Host half: format and pick a slot.
    struct vypr_shm shm;
    if (vypr_shm_open(&shm, path.c_str(), 1) < 0) return 1;

    const std::uint64_t window_id = 0x5a5480000001ull;
    struct vypr_msg_attach at;
    if (vypr_shm_alloc(&shm, window_id, w, h, &at) < 0) return 1;

    // Guest half: a ring of scattered pages, stamped.
    fake_ram ram{};
    if (fake_ram_open(&ram, ram_path.c_str(), 3ull << 30, 2ull << 30) < 0) return 1;
    TestRing ring;
    ring.ram   = &ram;
    ring.at    = at;
    ring.pages = (std::uint32_t)(vypr_ring_bytes(at.frame_bytes) / VYPR_PAGE_BYTES);
    ring.pfns.assign(ring.pages, 0);
    ring.base  = fake_ram_alloc(&ram, ring.pages, ring.pfns.data());
    if (!ring.base) { std::fputs("vypr-testagent: out of fake guest RAM\n", stderr); return 1; }
    ring.nonce = now_ns() ^ ((std::uint64_t)getpid() << 32);
    vypr::stamp_pages(ring.base, ring.pages, ring.pfns.data(), ring.nonce);

    // Host half again: check every page, as vyprd does, and describe the ring.
    vypr_guest_ram gram{};
    char why[256] = "";
    std::vector<vypr_ring_run> runs(ring.pages);
    if (vypr_guest_ram_open(&gram, ram_path.c_str(), why, sizeof(why)) < 0) {
        std::fprintf(stderr, "vypr-testagent: %s\n", why);
        return 1;
    }
    const int run_count = vypr_guest_ram_verify(&gram, ring.pfns.data(), ring.pages, ring.nonce,
                                                runs.data(), ring.pages, why, sizeof(why));
    if (run_count < 0) { std::fprintf(stderr, "vypr-testagent: ring check failed: %s\n", why); return 1; }
    vypr_shm_set_guest_ram(&shm, ram_path.c_str(), ram.bytes);
    vypr_shm_set_ring(&shm, at.slot, runs.data(), (std::uint32_t)run_count, ring.nonce, ring.pages);

    // Guest half: bind and publish, exactly as the Windows agent does.
    vypr::Publisher pub;
    if (!pub.bind(ring.base, (std::size_t)ring.pages * VYPR_PAGE_BYTES, at, ring.nonce)) {
        std::fprintf(stderr, "vypr-testagent: bind failed for slot %u\n", at.slot);
        return 1;
    }

    std::printf("vypr-testagent: slot %u, %ux%u, stride %u\n", at.slot, w, h, pub.stride());
    std::printf("present it with:  ./build/vypr-window --shm %s --slot %u --window-id %llu --stats\n",
                path.c_str(), at.slot, (unsigned long long)window_id);

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

        if (!pub.publish(w, h, stride, now_ns(), 1000000000ull, VYPR_PUB_DAMAGE_FULL)) {
            std::fprintf(stderr, "vypr-testagent: publish rejected\n");
            break;
        }

        next += period;
        std::uint64_t t = now_ns();
        if (next > t) usleep((useconds_t)((next - t) / 1000));
        else next = t;
    }

    std::printf("\nvypr-testagent: published %u frames\n", pub.serial());
    pub.close();
    vypr_shm_free(&shm, at.slot);
    ring.release();
    fake_ram_close(&ram);
    vypr_guest_ram_close(&gram);
    vypr_shm_close(&shm);
    return 0;
}

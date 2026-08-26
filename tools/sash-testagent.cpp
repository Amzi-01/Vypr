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
#include "shm.h"
}

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop = true; }

static std::uint64_t now_ns() {
    return (std::uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
    std::string path = "/dev/shm/sash-test";
    std::uint32_t w = 1280, h = 720, fps = 60;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--shm" && i + 1 < argc)       path = argv[++i];
        else if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%ux%u", &w, &h);
        else if (a == "--fps" && i + 1 < argc)  fps = (std::uint32_t)std::atoi(argv[++i]);
        else { std::fputs("usage: sash-testagent [--shm PATH] [--size WxH] [--fps N]\n", stderr); return 2; }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

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

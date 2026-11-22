#include "rdp/framebuffer.hpp"

#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

#define WINRDP_CHECK(cond)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "CHECK FAILED: " #cond " (" << __FILE__ << ":"        \
                      << __LINE__ << ")\n";                                    \
            return 1;                                                          \
        }                                                                      \
    } while (0)

using namespace winrdp;

int main() {
    Framebuffer fb;
    fb.resize(100, 80);
    WINRDP_CHECK(fb.width() == 100);
    WINRDP_CHECK(fb.height() == 80);
    WINRDP_CHECK(fb.resize_generation() == 1);

    bool ok1 = false;
    fb.consume([&](uint32_t w, uint32_t h, uint32_t stride, const uint8_t* data,
                   const DirtyRect& dirty, uint64_t rgen, uint64_t fgen) {
        ok1 = (w == 100 && h == 80 && stride == 400 && data != nullptr &&
               dirty.x == 0 && dirty.y == 0 && dirty.width == 100 && dirty.height == 80 &&
               rgen == 1 && fgen == 1);
    });
    WINRDP_CHECK(ok1);

    bool empty_after = false;
    fb.consume([&](uint32_t, uint32_t, uint32_t, const uint8_t*,
                   const DirtyRect& dirty, uint64_t, uint64_t) {
        empty_after = dirty.empty();
    });
    WINRDP_CHECK(empty_after);

    std::vector<uint8_t> src(100 * 80 * 4, 0);
    for (int y = 0; y < 80; ++y) {
        for (int x = 0; x < 100; ++x) {
            size_t off = (static_cast<size_t>(y) * 100 + x) * 4;
            src[off + 0] = static_cast<uint8_t>(x);
            src[off + 1] = static_cast<uint8_t>(y);
            src[off + 2] = 0xAA;
            src[off + 3] = 0xFF;
        }
    }

    fb.update_region(src.data(), 400, 10, 20, 15, 10);
    fb.update_region(src.data(), 400, 90, 70, 30, 30);

    bool ok2 = false;
    fb.consume([&](uint32_t, uint32_t, uint32_t stride, const uint8_t* data,
                   const DirtyRect& dirty, uint64_t, uint64_t) {
        const size_t off = static_cast<size_t>(25) * stride + static_cast<size_t>(12) * 4;
        ok2 = (dirty.x == 10 && dirty.y == 20 && dirty.width == 90 && dirty.height == 60 &&
               data[off + 0] == 12 && data[off + 1] == 25 && data[off + 2] == 0xAA);
    });
    WINRDP_CHECK(ok2);

    std::thread producer([&]() {
        for (int i = 0; i < 200; ++i) {
            fb.update_region(src.data(), 400, i % 50, i % 40, 20, 20);
        }
    });
    std::thread consumer([&]() {
        for (int i = 0; i < 200; ++i) {
            fb.consume([](uint32_t, uint32_t, uint32_t, const uint8_t*,
                          const DirtyRect&, uint64_t, uint64_t) {});
        }
    });
    producer.join();
    consumer.join();

    std::cout << "test_framebuffer: PASS\n";
    return 0;
}

#include "pe/core/PixelBuffer.hpp"
#include "pe_test.hpp"

using namespace pe;

PE_TEST(pixelbuffer_construct_and_fill) {
    PixelBuffer buf(4, 3, Rgba8{1, 2, 3, 4});
    PE_CHECK_EQ(buf.width(), 4);
    PE_CHECK_EQ(buf.height(), 3);
    PE_CHECK(!buf.isEmpty());
    PE_CHECK_EQ(buf.at(0, 0), (Rgba8{1, 2, 3, 4}));
    PE_CHECK_EQ(buf.at(3, 2), (Rgba8{1, 2, 3, 4}));
}

PE_TEST(pixelbuffer_set_get) {
    PixelBuffer buf(2, 2);
    buf.set(1, 1, Rgba8{10, 20, 30, 40});
    PE_CHECK_EQ(buf.at(1, 1), (Rgba8{10, 20, 30, 40}));
    PE_CHECK_EQ(buf.at(0, 0), (Rgba8{0, 0, 0, 0}));
}

PE_TEST(pixelbuffer_out_of_bounds_is_safe) {
    PixelBuffer buf(2, 2);
    // Reads outside bounds return a transparent pixel; writes are ignored.
    PE_CHECK_EQ(buf.at(-1, 0), (Rgba8{}));
    PE_CHECK_EQ(buf.at(5, 5), (Rgba8{}));
    buf.set(99, 99, Rgba8{255, 0, 0, 255}); // must not crash
    PE_CHECK_EQ(buf.at(0, 0), (Rgba8{}));
}

PE_TEST(pixelbuffer_default_empty) {
    PixelBuffer buf;
    PE_CHECK(buf.isEmpty());
    PE_CHECK_EQ(buf.width(), 0);
}

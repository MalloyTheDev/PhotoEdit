#include "pe/core/Channels.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe_test.hpp"

using namespace pe;

PE_TEST(channels_extract_single_channel) {
    PixelBuffer img(2, 1);
    img.set(0, 0, Rgba8{200, 100, 50, 240});
    img.set(1, 0, Rgba8{10, 20, 30, 40});

    PixelBuffer red = extractChannel(img, Channel::Red);
    PE_CHECK_EQ(red.at(0, 0), (Rgba8{200, 200, 200, 255}));  // grayscale, opaque
    PE_CHECK_EQ(red.at(1, 0), (Rgba8{10, 10, 10, 255}));

    PE_CHECK_EQ(extractChannel(img, Channel::Green).at(0, 0), (Rgba8{100, 100, 100, 255}));
    PE_CHECK_EQ(extractChannel(img, Channel::Blue).at(0, 0), (Rgba8{50, 50, 50, 255}));
    PE_CHECK_EQ(extractChannel(img, Channel::Alpha).at(0, 0), (Rgba8{240, 240, 240, 255}));
}

PE_TEST(channels_merge_from_grayscale) {
    auto gray = [](uint8_t v) {
        PixelBuffer p(1, 1);
        p.set(0, 0, Rgba8{v, v, v, 255});
        return p;
    };
    PixelBuffer merged = mergeChannels(gray(200), gray(100), gray(50));
    PE_CHECK_EQ(merged.at(0, 0), (Rgba8{200, 100, 50, 255}));  // alpha defaults opaque

    PixelBuffer withAlpha = mergeChannels(gray(200), gray(100), gray(50), gray(128));
    PE_CHECK_EQ(withAlpha.at(0, 0), (Rgba8{200, 100, 50, 128}));
}

PE_TEST(channels_split_merge_roundtrip) {
    PixelBuffer img(3, 2);
    img.set(0, 0, Rgba8{12, 34, 56, 78});
    img.set(2, 1, Rgba8{255, 0, 128, 200});
    PixelBuffer back =
        mergeChannels(extractChannel(img, Channel::Red), extractChannel(img, Channel::Green),
                      extractChannel(img, Channel::Blue), extractChannel(img, Channel::Alpha));
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 3; ++x) PE_CHECK_EQ(back.at(x, y), img.at(x, y));
    }
}

PE_TEST(channels_dimension_mismatch_is_empty) {
    PixelBuffer a(2, 2);
    PixelBuffer b(3, 1);
    PE_CHECK(mergeChannels(a, b, a).isEmpty());                       // mismatched green
    PE_CHECK(mergeChannels(a, a, a, b).isEmpty());                    // mismatched alpha
    PE_CHECK(extractChannel(PixelBuffer{}, Channel::Red).isEmpty());  // empty input
}

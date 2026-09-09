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

PE_TEST(channels_view_of_all_three_is_the_composite_untouched) {
    PixelBuffer img(2, 1);
    img.set(0, 0, Rgba8{200, 100, 50, 240});
    img.set(1, 0, Rgba8{10, 20, 30, 40});
    PixelBuffer copy = img;

    applyChannelView(img, ChannelView{});  // the default is everything visible
    PE_CHECK_EQ(img.at(0, 0), copy.at(0, 0));
    PE_CHECK_EQ(img.at(1, 0), copy.at(1, 0));
}

PE_TEST(channels_a_single_visible_channel_is_shown_as_grey) {
    // How a channel is actually read: its VALUES, not a tint. A red-tinted red channel is
    // harder to judge than a grey one, which is why Photoshop shows grey too.
    PixelBuffer img(1, 1);
    img.set(0, 0, Rgba8{200, 100, 50, 240});

    PixelBuffer red = img;
    applyChannelView(red, ChannelView{true, false, false});
    PE_CHECK_EQ(red.at(0, 0), (Rgba8{200, 200, 200, 240}));

    PixelBuffer green = img;
    applyChannelView(green, ChannelView{false, true, false});
    PE_CHECK_EQ(green.at(0, 0), (Rgba8{100, 100, 100, 240}));

    PixelBuffer blue = img;
    applyChannelView(blue, ChannelView{false, false, true});
    PE_CHECK_EQ(blue.at(0, 0), (Rgba8{50, 50, 50, 240}));
}

PE_TEST(channels_a_hidden_channel_is_zeroed_and_the_rest_stay_colour) {
    PixelBuffer img(1, 1);
    img.set(0, 0, Rgba8{200, 100, 50, 240});

    PixelBuffer noBlue = img;
    applyChannelView(noBlue, ChannelView{true, true, false});
    PE_CHECK_EQ(noBlue.at(0, 0), (Rgba8{200, 100, 0, 240}));  // still two-colour, not grey

    PixelBuffer noRed = img;
    applyChannelView(noRed, ChannelView{false, true, true});
    PE_CHECK_EQ(noRed.at(0, 0), (Rgba8{0, 100, 50, 240}));

    PixelBuffer none = img;
    applyChannelView(none, ChannelView{false, false, false});
    PE_CHECK_EQ(none.at(0, 0), (Rgba8{0, 0, 0, 240}));  // black, and still transparent
}

PE_TEST(channels_a_view_never_touches_alpha) {
    // Hiding a colour channel must not make a transparent pixel opaque, or the canvas
    // checkerboard would vanish the moment a channel view was switched on: the user would
    // read that as the transparency having been filled in.
    PixelBuffer img(4, 1);
    for (int x = 0; x < 4; ++x) {
        img.set(x, 0, Rgba8{80, 90, 100, static_cast<uint8_t>(x * 85)});
    }
    for (const ChannelView v : {ChannelView{true, false, false}, ChannelView{false, true, true},
                                ChannelView{false, false, false}}) {
        PixelBuffer got = img;
        applyChannelView(got, v);
        for (int x = 0; x < 4; ++x) PE_CHECK_EQ(got.at(x, 0).a, img.at(x, 0).a);
    }
}

PE_TEST(channels_a_single_channel_view_agrees_with_extractChannel) {
    // Two ways to say the same thing, and they must not drift: the panel builds its row
    // thumbnails with extractChannel and the canvas draws with applyChannelView, so a
    // difference would be a thumbnail that does not match the canvas it describes.
    PixelBuffer img(5, 3);
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 5; ++x) {
            img.set(x, y,
                    Rgba8{static_cast<uint8_t>(x * 50), static_cast<uint8_t>(y * 80),
                          static_cast<uint8_t>(x * y * 7), 255});
        }
    }
    const Channel kinds[3] = {Channel::Red, Channel::Green, Channel::Blue};
    const ChannelView views[3] = {ChannelView{true, false, false}, ChannelView{false, true, false},
                                  ChannelView{false, false, true}};
    for (int i = 0; i < 3; ++i) {
        PixelBuffer viewed = img;
        applyChannelView(viewed, views[i]);
        const PixelBuffer extracted = extractChannel(img, kinds[i]);
        for (int y = 0; y < 3; ++y) {
            for (int x = 0; x < 5; ++x) {
                PE_CHECK_EQ(viewed.at(x, y).r, extracted.at(x, y).r);
                PE_CHECK_EQ(viewed.at(x, y).g, extracted.at(x, y).g);
                PE_CHECK_EQ(viewed.at(x, y).b, extracted.at(x, y).b);
            }
        }
    }
}

PE_TEST(channels_an_empty_buffer_is_left_alone) {
    PixelBuffer empty;
    applyChannelView(empty, ChannelView{true, false, false});
    PE_CHECK(empty.isEmpty());
}

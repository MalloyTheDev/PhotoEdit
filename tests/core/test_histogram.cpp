#include "pe/core/Histogram.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe_test.hpp"

using namespace pe;

PE_TEST(histogram_counts_uniform_image) {
    PixelBuffer img(4, 4, Rgba8{10, 20, 30, 255});
    Histogram h = computeHistogram(img);
    PE_CHECK_EQ(h.count, static_cast<uint64_t>(16));
    PE_CHECK_EQ(h.red[10], static_cast<uint64_t>(16));
    PE_CHECK_EQ(h.green[20], static_cast<uint64_t>(16));
    PE_CHECK_EQ(h.blue[30], static_cast<uint64_t>(16));
    PE_CHECK_EQ(h.alpha[255], static_cast<uint64_t>(16));
    // Every other red bin is empty.
    PE_CHECK_EQ(h.red[11], static_cast<uint64_t>(0));
}

PE_TEST(histogram_empty_buffer_is_zero) {
    PixelBuffer img;  // empty
    Histogram h = computeHistogram(img);
    PE_CHECK_EQ(h.count, static_cast<uint64_t>(0));
}

PE_TEST(histogram_luma_of_white_and_black) {
    PixelBuffer img(2, 1);
    img.set(0, 0, Rgba8{255, 255, 255, 255});  // luma 255
    img.set(1, 0, Rgba8{0, 0, 0, 255});        // luma 0
    Histogram h = computeHistogram(img);
    PE_CHECK_EQ(h.luma[255], static_cast<uint64_t>(1));
    PE_CHECK_EQ(h.luma[0], static_cast<uint64_t>(1));
}

PE_TEST(histogram_luma_rec601_red) {
    PixelBuffer img(1, 1, Rgba8{255, 0, 0, 255});
    Histogram h = computeHistogram(img);
    // 0.299 * 255 = 76.245 -> rounds to 76.
    PE_CHECK_EQ(h.luma[76], static_cast<uint64_t>(1));
}

PE_TEST(histogram_stats_mean_and_range) {
    // Two levels: 50 and 150, equal population -> mean 100, median at the lower
    // crossing (50th percentile reaches level 50 first).
    Histogram::Bins bins{};
    bins[50] = 3;
    bins[150] = 3;
    ChannelStats s = channelStats(bins);
    PE_CHECK_EQ(s.count, static_cast<uint64_t>(6));
    PE_CHECK_NEAR(static_cast<float>(s.mean), 100.0f);
    PE_CHECK_EQ(s.minLevel, 50);
    PE_CHECK_EQ(s.maxLevel, 150);
    PE_CHECK_EQ(s.median, 50);     // cumulative reaches 50% (3 of 6) exactly at level 50
    PE_CHECK_EQ(s.modeLevel, 50);  // ties resolve to the first-seen mode
    // stddev = sqrt( (3*(50-100)^2 + 3*(150-100)^2) / 6 ) = 50.
    PE_CHECK_NEAR(static_cast<float>(s.stdDev), 50.0f);
}

PE_TEST(histogram_stats_empty_is_zero) {
    Histogram::Bins bins{};
    ChannelStats s = channelStats(bins);
    PE_CHECK_EQ(s.count, static_cast<uint64_t>(0));
    PE_CHECK_EQ(s.median, 0);
    PE_CHECK_NEAR(static_cast<float>(s.mean), 0.0f);
}

PE_TEST(histogram_percentile_bounds) {
    Histogram::Bins bins{};
    for (int i = 0; i < 256; ++i) bins[static_cast<std::size_t>(i)] = 1;  // flat
    PE_CHECK_EQ(percentileLevel(bins, 0.0), 0);
    PE_CHECK_EQ(percentileLevel(bins, 1.0), 255);
    // Out-of-range percentiles clamp.
    PE_CHECK_EQ(percentileLevel(bins, -5.0), 0);
    PE_CHECK_EQ(percentileLevel(bins, 9.0), 255);
}

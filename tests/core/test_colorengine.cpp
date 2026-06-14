#include "pe/core/ColorEngine.hpp"
#include "pe/core/ColorProfile.hpp"
#include "pe_test.hpp"

#ifdef PHOTOEDIT_HAVE_LCMS2

using namespace pe;

PE_TEST(colorengine_caches_and_reuses_transform) {
    ColorEngine engine;
    auto srgb = ColorProfile::builtin(BuiltinSpace::sRGB);
    auto adobe = ColorProfile::builtin(BuiltinSpace::AdobeRGB1998);
    PE_CHECK(srgb != nullptr && adobe != nullptr);
    PE_CHECK_EQ(engine.cachedTransformCount(), static_cast<std::size_t>(0));

    auto t1 = engine.transform(srgb, adobe);
    PE_CHECK(t1 != nullptr);
    PE_CHECK_EQ(engine.cachedTransformCount(), static_cast<std::size_t>(1));

    // Same key -> same cached transform (pointer identity), no new entry.
    auto t2 = engine.transform(srgb, adobe);
    PE_CHECK(t1.get() == t2.get());
    PE_CHECK_EQ(engine.cachedTransformCount(), static_cast<std::size_t>(1));
}

PE_TEST(colorengine_distinct_keys_make_distinct_entries) {
    ColorEngine engine;
    auto srgb = ColorProfile::builtin(BuiltinSpace::sRGB);
    auto adobe = ColorProfile::builtin(BuiltinSpace::AdobeRGB1998);

    auto a = engine.transform(srgb, adobe, RenderingIntent::RelativeColorimetric, true);
    auto b = engine.transform(srgb, adobe, RenderingIntent::Perceptual, true);  // different intent
    auto c = engine.transform(srgb, adobe, RenderingIntent::RelativeColorimetric, false);  // no BPC
    auto d = engine.transform(adobe, srgb);  // reversed direction
    PE_CHECK(a != nullptr && b != nullptr && c != nullptr && d != nullptr);
    PE_CHECK(a.get() != b.get());
    PE_CHECK(a.get() != c.get());
    PE_CHECK(a.get() != d.get());
    PE_CHECK_EQ(engine.cachedTransformCount(), static_cast<std::size_t>(4));

    engine.clearCache();
    PE_CHECK_EQ(engine.cachedTransformCount(), static_cast<std::size_t>(0));
}

PE_TEST(colorengine_null_profile_is_null) {
    ColorEngine engine;
    auto srgb = ColorProfile::builtin(BuiltinSpace::sRGB);
    PE_CHECK(engine.transform(nullptr, srgb) == nullptr);
    PE_CHECK(engine.transform(srgb, nullptr) == nullptr);
    PE_CHECK_EQ(engine.cachedTransformCount(), static_cast<std::size_t>(0));  // failures not cached
}

#endif  // PHOTOEDIT_HAVE_LCMS2

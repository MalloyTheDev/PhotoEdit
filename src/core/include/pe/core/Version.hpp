#pragma once

// Single source of truth for the engine version. The app shell and file-format
// writers (PSD/native) stamp documents with this.
namespace pe {

struct Version {
    static constexpr int kMajor = 0;
    static constexpr int kMinor = 1;
    static constexpr int kPatch = 0;

    static constexpr const char* string() noexcept { return "0.1.0"; }
};

}  // namespace pe

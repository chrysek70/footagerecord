#pragma once
// Recording rules shared with the Mac app: sizes, file names, elapsed time.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <string>

struct PixelSize {
    int width = 0;
    int height = 0;
    bool operator==(PixelSize const&) const = default;
};

// Encoders need positive, even dimensions. Never scale up. Hardware H.264 encoders
// commonly stop at 4096×2304, so larger sources are scaled down to fit that frame area.
inline PixelSize FitForH264(double width, double height) {
    if (!std::isfinite(width) || !std::isfinite(height) || width < 2 || height < 2) return {};
    double scale = std::min({1.0, 4096.0 / width, 4096.0 / height,
                             std::sqrt(4096.0 * 2304.0 / (width * height))});
    return {std::max(2, static_cast<int>(width * scale / 2 + 1e-6) * 2),
            std::max(2, static_cast<int>(height * scale / 2 + 1e-6) * 2)};
}

// "Footage 2026-09-23 13-32-29.mp4", then " (2)", " (3)"… so nothing is overwritten.
inline std::filesystem::path FinalPath(std::filesystem::path const& folder, std::tm const& when) {
    wchar_t stamp[32]{};
    std::wcsftime(stamp, 32, L"%Y-%m-%d %H-%M-%S", &when);
    std::wstring name = L"Footage " + std::wstring(stamp);
    auto candidate = folder / (name + L".mp4");
    for (int suffix = 2; std::filesystem::exists(candidate); ++suffix)
        candidate = folder / (name + L" (" + std::to_wstring(suffix) + L").mp4");
    return candidate;
}

inline std::wstring Elapsed(double seconds) {
    long long value = std::isfinite(seconds) ? static_cast<long long>(std::clamp(seconds, 0.0, 359999.0)) : 0;
    wchar_t text[16]{};
    if (value >= 3600)
        std::swprintf(text, 16, L"%lld:%02lld:%02lld", value / 3600, value / 60 % 60, value % 60);
    else
        std::swprintf(text, 16, L"%02lld:%02lld", value / 60, value % 60);
    return text;
}

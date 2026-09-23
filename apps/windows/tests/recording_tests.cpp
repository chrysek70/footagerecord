// Checks the recording rules that don't need a capture device. Returns non-zero on failure.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include "recording.h"

static int failures = 0;
#define EXPECT(condition) do { if (!(condition)) { std::printf("FAILED line %d: %s\n", __LINE__, #condition); ++failures; } } while (0)

int main() {
    EXPECT((FitForH264(1920, 1080) == PixelSize{1920, 1080}));
    EXPECT((FitForH264(801, 601) == PixelSize{800, 600}));
    EXPECT((FitForH264(3840, 2160) == PixelSize{3840, 2160}));
    EXPECT((FitForH264(7680, 4320) == PixelSize{4096, 2304}));
    EXPECT((FitForH264(5120, 1440) == PixelSize{4096, 1152}));
    EXPECT((FitForH264(0, 1080) == PixelSize{}));
    EXPECT((FitForH264(-1, 1080) == PixelSize{}));

    auto folder = std::filesystem::temp_directory_path() / L"FootageRecordTests";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    std::tm when{};
    when.tm_year = 126; when.tm_mon = 8; when.tm_mday = 23; when.tm_hour = 13; when.tm_min = 32; when.tm_sec = 29;
    auto first = FinalPath(folder, when);
    EXPECT(first.filename() == L"Footage 2026-09-23 13-32-29.mp4");
    { std::ofstream(first) << "keep this recording"; }
    auto second = FinalPath(folder, when);
    EXPECT(second.filename() == L"Footage 2026-09-23 13-32-29 (2).mp4");
    std::filesystem::remove_all(folder);

    EXPECT(Elapsed(65) == L"01:05");
    EXPECT(Elapsed(3661) == L"1:01:01");
    EXPECT(Elapsed(-1) == L"00:00");
    EXPECT(Elapsed(0.0 / 0.0) == L"00:00");

    std::printf(failures ? "%d failure(s)\n" : "All recording rule tests passed\n", failures);
    return failures ? 1 : 0;
}

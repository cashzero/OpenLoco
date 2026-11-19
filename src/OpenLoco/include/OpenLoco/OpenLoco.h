#pragma once

#include <OpenLoco/Core/FileSystem.hpp>
#include <cstdint>
#include <functional>
#include <string>

namespace OpenLoco::Ui
{
    struct HeadlessScreenshotOptions;
}

namespace OpenLoco
{
    using StringId = uint16_t;

    namespace Engine
    {
        constexpr uint32_t MaxTimeDeltaMs = 500;
        constexpr uint32_t UpdateRateHz = 40;
        constexpr uint32_t UpdateRateInMs = 1000 / UpdateRateHz;
        constexpr uint32_t MaxUpdates = 3;
    }

    void* hInstance();
    void resetSubsystems();
    bool simulateGame(const fs::path& path, int32_t ticks);
    bool screenshotGame(const fs::path& path, int32_t ticks, const fs::path& outputPath, const Ui::HeadlessScreenshotOptions& options);

    void initialise();
    void update();
    void sub_431695(uint16_t var_F253A0);
    uint16_t getTimeSinceLastTick();
    uint16_t getNumFrameUpdates();
    bool promptTickLoop(std::function<bool()> tickAction);
    [[noreturn]] void exitCleanly();
    [[noreturn]] void exitWithError(StringId titleStringId, StringId messageStringId);
}

#pragma once

#include <OpenLoco/Core/FileSystem.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

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
    bool newGame(const fs::path& outputPath, int32_t ticks, const std::vector<std::string>& preferredObjects);
    // OpenGraphics fork：依場景腳本（YAML）建造地圖並存檔（SceneBuilder）
    bool buildScene(const fs::path& scenePath, const fs::path& outputPath);
    bool screenshotGame(const fs::path& path, int32_t ticks, const fs::path& outputPath, const Ui::HeadlessScreenshotOptions& options);

    void initialise();
    void update();
    uint32_t getRenderedFrameCount();
    // OpenGraphics fork：依名稱開啟視窗（--open-window，給 UI 截圖用），回傳成功開啟的數量
    int openWindowsByName(const std::string& names);
    void sub_431695(uint16_t var_F253A0);
    uint16_t getTimeSinceLastTick();
    uint16_t getNumFrameUpdates();
    bool promptTickLoop(std::function<bool()> tickAction);
    [[noreturn]] void exitCleanly();
    [[noreturn]] void exitWithError(StringId titleStringId, StringId messageStringId);
}

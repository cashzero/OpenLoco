#pragma once

#include <OpenLoco/Core/FileSystem.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace OpenLoco::Ui
{
    enum class ScreenshotType : uint8_t
    {
        regular = 0,
        giant = 1,
    };

    void triggerScreenshotCountdown(int8_t numTicks, ScreenshotType type);
    void handleScreenshotCountdown();

    // Headless 截圖：不需要視窗，直接把指定視角畫到記憶體並存成 PNG。
    struct HeadlessScreenshotOptions
    {
        int32_t width = 1280;
        int32_t height = 720;
        int8_t zoom = 0;     // 0 = 最近
        int8_t rotation = 0; // 0..3
        std::optional<std::pair<int32_t, int32_t>> centreTile{}; // 未指定時用主視窗中心，再不行用地圖中心
        bool giant = false;  // 整張地圖
        std::string objectName{}; // 有指定時改為輸出該物件的預覽圖（物件選擇視窗的樣式）
    };

    void saveScreenshotToFile(const fs::path& path, const HeadlessScreenshotOptions& options);
}

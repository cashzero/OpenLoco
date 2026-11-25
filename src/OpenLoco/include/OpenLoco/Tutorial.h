#pragma once

#include "Types.hpp"

namespace OpenLoco::Tutorial
{
    enum class State : uint8_t
    {
        none,
        initialising,
        standby,
        playing,
        recording,
    };

    State state();

    void initialise(int16_t tutorialNumber);
    // OpenGraphics fork：教學檔（錄製的操作）與 Boulder Breakers 情境都存在時才能開始教學（standalone 沒有）
    bool isAvailable();
    void start();
    void stop();

    int32_t nextInput();
    StringId nextString();

    uint8_t getTutorialNumber();
}

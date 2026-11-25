#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace OpenLoco
{
    enum class CommandLineAction
    {
        none,
        host,
        join,
        uncompress,
        simulate,
        compare,
        help,
        version,
        intro,
        screenshot,
        newGame,
        buildScene,
    };

    struct CommandLineOptions
    {
        CommandLineAction action = CommandLineAction::none;
        std::string address;
        std::string path;
        std::string path2;
        std::optional<int32_t> ticks;
        std::string outputPath;
        std::string bind;
        std::optional<uint16_t> port{};
        std::string logLevels;
        std::string all;
        std::optional<std::string> locomotionDataPath{};
        // screenshot（headless 截圖）
        std::optional<int32_t> zoom{};
        std::optional<int32_t> rotation{};
        std::string centre;
        std::string size;
        bool giant = false;
        std::string object;
        std::optional<int32_t> exitAfterFrames{};
        std::string openWindow; // OpenGraphics fork：--exit-after-frames 截圖前開啟的視窗（逗號分隔）
        std::string select;
    };

    std::optional<CommandLineOptions> parseCommandLine(std::vector<std::string>&& argv);
    const CommandLineOptions& getCommandLineOptions();
    void setCommandLineOptions(const CommandLineOptions& options);
}

#include "Tutorial.h"
#include "Config.h"
#include "Environment.h"
#include "GameState.h"
#include "Gui.h"
#include "Localisation/StringIds.h"
#include "Logging.h"
#include "Scenario/Scenario.h"
#include "SceneManager.h"
#include "Ui.h"
#include <OpenLoco/Core/Exception.hpp>

#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <vector>

namespace OpenLoco::Tutorial
{
    static State _state; // 0x00508F19

    static StringId _tutorialString; // 0x009C8708
    static uint8_t _tutorialNumber;  // 0x009C870A

    static std::vector<uint16_t> _tutorialData;
    static std::vector<uint16_t>::const_iterator _tutorialIt;

    constexpr Config::Resolution tutorialResolution = { 1024, 768 };

    State state()
    {
        return _state;
    }

    static std::vector<uint16_t> readTutorialFile(fs::path filename)
    {
        std::ifstream file(filename, std::ios::in | std::ios::binary);
        file.unsetf(std::ios::skipws);
        if (!file)
        {
            throw Exception::RuntimeError("Unable to open tutorial file " + filename.u8string());
        }

        std::vector<uint16_t> tutorial;
        auto start = std::istream_iterator<uint8_t>(file);
        auto const end = std::istream_iterator<uint8_t>();

        for (auto it = start; it != end; ++it)
        {
            auto const firstByte = *it;
            ++it;

            // We expect an even number of bytes
            if (it == end)
            {
                throw Exception::RuntimeError("Tutorial file has an odd number of bytes");
            }

            auto const secondByte = *it;
            tutorial.push_back(secondByte << 8 | firstByte);
        }

        return tutorial;
    }

    // 0x0043C590
    // 不分大小寫找檔案（Locomotion 的檔名大小寫不一致）
    static bool fileExistsIgnoreCase(const fs::path& path)
    {
        std::error_code ec;
        if (fs::exists(path, ec))
        {
            return true;
        }
        auto lower = [](std::string s) {
            for (auto& c : s)
            {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return s;
        };
        const auto wanted = lower(path.filename().u8string());
        for (const auto& entry : fs::directory_iterator(path.parent_path(), ec))
        {
            if (lower(entry.path().filename().u8string()) == wanted)
            {
                return true;
            }
        }
        return false;
    }

    bool isAvailable()
    {
        static std::optional<bool> available;
        if (!available.has_value())
        {
            available = fileExistsIgnoreCase(Environment::getPathNoWarning(Environment::PathId::boulderBreakers))
                && fileExistsIgnoreCase(Environment::getPathNoWarning(Environment::PathId::tut1024_1))
                && fileExistsIgnoreCase(Environment::getPathNoWarning(Environment::PathId::tut1024_2))
                && fileExistsIgnoreCase(Environment::getPathNoWarning(Environment::PathId::tut1024_3));
            if (!*available)
            {
                Diagnostics::Logging::info("Tutorial files not found: tutorials are disabled");
            }
        }
        return *available;
    }

    void initialise(int16_t tutorialNumber)
    {
        if (tutorialNumber < 0 || tutorialNumber > 3 || !isAvailable())
        {
            return;
        }

        _state = State::initialising;

        // NB: only used by tutorial widget drawing after.
        _tutorialNumber = tutorialNumber;

        // Figure out what dimensions to use for the tutorial, and whether we can continue using scaling.
        auto& config = Config::get();
        Config::Resolution newResolution = tutorialResolution;
        if (config.scaleFactor > 1.0)
        {
            newResolution *= config.scaleFactor;
            Config::Resolution desktopResolution = Ui::getDesktopResolution();

            // Don't scale if it means the new window won't fit the desktop.
            if (newResolution > desktopResolution)
            {
                Ui::setWindowScaling(1.0);
                newResolution = tutorialResolution;
            }
        }

        // Ensure that we're in windowed mode, using dimensions 1024x768.
        auto currentResolution = Ui::getResolution();
        if (config.display.mode != Config::ScreenMode::window || currentResolution != newResolution)
        {
            if (!Ui::setDisplayMode(Config::ScreenMode::window, newResolution))
            {
                _state = State::none;
                return;
            }
        }

        // Disable options that interfere with tutorial operations.
        config.buildLockedVehicles = false;
        config.cheatsMenuEnabled = false;
        config.displayLockedVehicles = false;
        config.invertRightMouseViewPan = false;
        config.toolbarAutoMenu = true;
        config.toolbarButtonsCentred = false;

        // Get the environment file for this tutorial.
        static constexpr Environment::PathId tutorialFileIds[] = {
            Environment::PathId::tut1024_1,
            Environment::PathId::tut1024_2,
            Environment::PathId::tut1024_3,
        };

        auto fileId = tutorialFileIds[tutorialNumber];

        auto tutPath = Environment::getPath(fileId);
        _tutorialData = readTutorialFile(tutPath);
        _tutorialIt = _tutorialData.cbegin();

        // Set the first string to show.
        static constexpr StringId openingStringIds[] = {
            StringIds::tutorial_1_string_1,
            StringIds::tutorial_2_string_1,
            StringIds::tutorial_3_string_1,
        };

        _state = State::standby;
        _tutorialString = openingStringIds[_tutorialNumber];

        // Load the scenario
        auto scPath = Environment::getPath(Environment::PathId::boulderBreakers);
        Scenario::load(scPath);

        // Set fixed rng seed
        auto& gameState = getGameState();
        gameState.rng = Core::Prng(0x12345678, 0x9ABCDEF0);

        // Start the scenario
        Scenario::start();
    }

    void start()
    {
        assert(_state == State::standby);
        _state = State::playing;
    }

    // 0x0043C70E
    void stop()
    {
        _state = State::none;
        Config::read();
        Ui::setDisplayMode(Config::get().display.mode);
    }

    // 0x0043C7A2
    int32_t nextInput()
    {
        int32_t next = static_cast<int32_t>(*_tutorialIt);
        _tutorialIt++;

        if (_tutorialIt == _tutorialData.end())
        {
            stop();
            SceneManager::resetSceneAge();
        }

        return next;
    }

    StringId nextString()
    {
        StringId currentString = _tutorialString;
        _tutorialString = currentString + 1;
        return currentString;
    }

    uint8_t getTutorialNumber()
    {
        return _tutorialNumber;
    }
}

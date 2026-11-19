#include "Title.h"
#include "Audio/Audio.h"
#include "Config.h"
#include "Date.h"
#include "EditorController.h"
#include "Environment.h"
#include "Game.h"
#include "GameCommands/GameCommands.h"
#include "GameCommands/General/TogglePause.h"
#include "GameState.h"
#include "GameStateFlags.h"
#include "Graphics/Gfx.h"
#include "Gui.h"
#include "Intro.h"
#include "Map/TileManager.h"
#include "MessageManager.h"
#include "Objects/ClimateObject.h"
#include "Objects/ObjectIndex.h"
#include "Objects/ObjectManager.h"
#include "OpenLoco.h"
#include "S5/S5.h"
#include "Scenario/Scenario.h"
#include "Scenario/ScenarioOptions.h"
#include "SceneManager.h"
#include "Ui/WindowManager.h"
#include "World/CompanyManager.h"

#include <OpenLoco/Diagnostics/Logging.h>
#include <OpenLoco/Utility/String.hpp>
#include <array>
#include <optional>
#include <variant>
#include <vector>

namespace OpenLoco::Title
{
    struct WaitStep
    {
        uint16_t duration;
    };

    struct ReloadStep
    {
        uint16_t arg;
    };

    using MoveStep = World::TilePos2;

    struct RotateStep
    {
    };

    struct ResetStep
    {
    };

    using TitleStep = std::variant<WaitStep, ReloadStep, MoveStep, RotateStep, ResetStep>;
    using TitleSequence = std::vector<TitleStep>;

    static const TitleSequence _titleSequence = {
        MoveStep{ 231, 160 },
        WaitStep{ 368 },
        MoveStep{ 93, 59 },
        WaitStep{ 370 },
        RotateStep{},
        MoveStep{ 314, 230 },
        WaitStep{ 370 },
        MoveStep{ 83, 314 },
        WaitStep{ 370 },
        RotateStep{},
        RotateStep{},
        MoveStep{ 224, 253 },
        WaitStep{ 375 },
        MoveStep{ 210, 322 },
        WaitStep{ 375 },
        MoveStep{ 125, 338 },
        WaitStep{ 375 },
        MoveStep{ 72, 138 },
        WaitStep{ 375 },
        RotateStep{},
        MoveStep{ 178, 158 },
        WaitStep{ 375 },
        MoveStep{ 203, 316 },
        WaitStep{ 375 },
        ReloadStep{ 0 },
        ResetStep{},
    };

    static TitleSequence::const_iterator _sequenceIterator;
    static uint16_t _waitCounter;

    // OpenGraphics fork：沒有 title.dat 時（standalone），用已安裝的物件生成標題畫面用的地圖
    static bool _isGeneratedTitleWorld = false;

    // 選積雪最少（夏季雪線最高）的氣候，標題畫面比較好看
    static void selectLeastSnowyClimate(ObjectManager::ObjectIndexSelection& selection)
    {
        std::optional<ObjectHeader> best;
        int32_t bestSnowLine = -1;
        for (const auto& entry : ObjectManager::getAvailableObjects(ObjectType::climate))
        {
            if (!ObjectManager::loadTemporaryObject(entry.object._header).has_value())
            {
                continue;
            }
            const auto* climate = reinterpret_cast<const ClimateObject*>(ObjectManager::getTemporaryObject());
            if (climate != nullptr && climate->summerSnowLine > bestSnowLine)
            {
                bestSnowLine = climate->summerSnowLine;
                best = entry.object._header;
            }
            ObjectManager::freeTemporaryObject();
        }
        if (best.has_value())
        {
            selection.selectObject(ObjectManager::SelectObjectModes::defaultSelect, *best);
        }
    }

    static void selectGeneratedTitleObjects(ObjectManager::ObjectIndexSelection& selection, bool addAllScenery = true)
    {
        using ObjectManager::SelectObjectModes;
        if (selection.selectionMetaData.numSelectedObjects[enumValue(ObjectType::climate)] == 0)
        {
            selectLeastSnowyClimate(selection);
        }
        // 物件選擇驗證要求的類型，各選一個
        constexpr std::array kRequiredTypes = {
            ObjectType::region, ObjectType::scaffolding, ObjectType::industry, ObjectType::building,
            ObjectType::interfaceSkin, ObjectType::vehicle, ObjectType::land, ObjectType::currency,
            ObjectType::water, ObjectType::townNames, ObjectType::levelCrossing, ObjectType::streetLight,
            ObjectType::snow, ObjectType::climate, ObjectType::hillShapes, ObjectType::cliffEdge,
        };
        for (const auto type : kRequiredTypes)
        {
            if (selection.selectionMetaData.numSelectedObjects[enumValue(type)] != 0)
            {
                continue;
            }
            for (const auto& entry : ObjectManager::getAvailableObjects(type))
            {
                if (selection.selectObject(SelectObjectModes::defaultSelect, entry.object._header))
                {
                    break;
                }
            }
        }
        if (!addAllScenery)
        {
            return;
        }
        // 讓地圖豐富一些：全部的地形、樹、建築、圍牆、道路
        constexpr std::array kAllOfTypes = { ObjectType::land, ObjectType::tree, ObjectType::building, ObjectType::wall, ObjectType::road };
        for (const auto type : kAllOfTypes)
        {
            for (const auto& entry : ObjectManager::getAvailableObjects(type))
            {
                selection.selectObject(SelectObjectModes::defaultSelect, entry.object._header);
            }
        }
    }

    bool loadStandaloneWorldObjects(const std::vector<std::string>& preferredObjects)
    {
        ObjectManager::unloadAll();
        auto& selection = ObjectManager::prepareSelectionList(false, preferredObjects.empty());
        // 先選指定的物件（依內部名稱），其餘必選類型再自動補上
        for (const auto& name : preferredObjects)
        {
            bool found = false;
            const auto numObjects = ObjectManager::getNumInstalledObjects();
            for (uint32_t i = 0; i < numObjects; i++)
            {
                const auto& entry = ObjectManager::getObjectInIndex(static_cast<ObjectManager::ObjectIndexId>(i));
                if (Utility::iequals(Utility::trim(entry._header.getName()), name))
                {
                    found = selection.selectObject(ObjectManager::SelectObjectModes::defaultSelect, entry._header);
                    break;
                }
            }
            if (!found)
            {
                Diagnostics::Logging::warn("Preferred object {} could not be selected", name);
            }
        }
        // 有指定物件時只補齊必選類型，不自動加入全部的樹與建築（避免混入大量 placeholder）
        selectGeneratedTitleObjects(selection, preferredObjects.empty());
        const auto invalidType = ObjectManager::validateObjectSelection(selection.objectFlags);
        if (invalidType.has_value())
        {
            Diagnostics::Logging::warn("Object selection invalid: missing object type {}", enumValue(*invalidType));
        }
        else
        {
            Diagnostics::Logging::info("Object selection valid");
        }
        ObjectManager::loadSelectionListObjects(selection.objectFlags);
        ObjectManager::freeSelectionList();
        ObjectManager::reloadAll();
        Scenario::sub_4748D4();
        return !invalidType.has_value();
    }

    static void generateTitleWorld()
    {
        Diagnostics::Logging::info("Title save not found: generating a title landscape from installed objects");

        loadStandaloneWorldObjects();

        EditorController::setDefaultLandscapeOptions();
        Scenario::getOptions().topographyStyle = Scenario::TopographyStyle::smallHills;
        Scenario::reset();
        // 夏天：避免標題畫面整片積雪
        Scenario::initialiseDate(1950, MonthId::july, 1);
        Scenario::initialiseSnowLine();
        Scenario::generateLandscape();
        Gfx::loadDefaultPalette();
        Gfx::invalidateScreen();
        _isGeneratedTitleWorld = true;
    }

    // 0x004442C4
    static void loadTitle()
    {
        Scenario::sub_46115C();
        if (Intro::state() == Intro::State::none)
        {
            uint16_t backupWord = getGameState().var_014A;
            auto titlePath = Environment::getPathNoWarning(Environment::PathId::title);
            SceneManager::removeSceneFlags(SceneManager::Flags::networked);
            if (!fs::exists(titlePath))
            {
                // 標題序列會定期 reload；生成過一次就沿用同一張地圖
                if (!_isGeneratedTitleWorld)
                {
                    generateTitleWorld();
                }
                getGameState().var_014A = backupWord;
                return;
            }
            S5::importSaveToGameState(titlePath, S5::LoadFlags::titleSequence);

            CompanyManager::setControllingId(CompanyId(0));
            CompanyManager::setSecondaryPlayerId(CompanyId::null);
            if (!SceneManager::isNetworked())
            {
                CompanyManager::setSecondaryPlayerId(CompanyId(1));
                if (!SceneManager::isNetworkHost())
                {
                    CompanyManager::setControllingId(CompanyId(1));
                    CompanyManager::setSecondaryPlayerId(CompanyId(0));
                }
            }

            getGameState().var_014A = backupWord;
        }
    }

    static void reload()
    {
        loadTitle();
        SceneManager::resetSceneAge();
        update();
    }

    // 0x00444357
    void reset()
    {
        _sequenceIterator = _titleSequence.begin();
        _waitCounter = 0;
        reload();
    }

    // 0x0046AD7D
    void start()
    {
        GameCommands::setUpdatingCompanyId(CompanyManager::getControllingId());
        if (SceneManager::isPaused())
        {
            GameCommands::registers regs;
            GameCommands::togglePause(regs, GameCommands::Flags::apply);
        }

        auto currentScreenFlags = SceneManager::getSceneFlags();
        SceneManager::removeSceneFlags(SceneManager::Flags::networked);
        Ui::WindowManager::closeAllFloatingWindows();
        setSceneFlags(currentScreenFlags);
        SceneManager::addSceneFlags(SceneManager::Flags::title);
        SceneManager::setGameSpeed(GameSpeed::Normal);
        ObjectManager::unloadAll();
        auto& selection = ObjectManager::prepareSelectionList(false);
        ObjectManager::loadSelectionListObjects(selection.objectFlags);
        ObjectManager::freeSelectionList();
        ObjectManager::reloadAll();
        Scenario::sub_4748D4();
        Scenario::reset();
        resetSubsystems();
        MessageManager::reset();
        Gui::init();
        reset();
        Gfx::invalidateScreen();
        SceneManager::resetSceneAge();

        if (Config::get().audio.playTitleMusic)
        {
            Audio::playMusic(Environment::PathId::css5, Config::get().audio.mainVolume, true);
        }

        SceneManager::addSceneFlags(SceneManager::Flags::initialised);
    }

    void stop()
    {
        Audio::stopMusic();
    }

    static void handleStep(const WaitStep& step)
    {
        // This loop slightly deviates from the original, so we subtract 1 tick to make up for it.
        _waitCounter = step.duration - 1;
    }

    static void handleStep([[maybe_unused]] const ReloadStep step)
    {
        reload();
    }

    static void handleStep(const MoveStep step)
    {
        if (Game::hasFlags(GameStateFlags::tileManagerLoaded))
        {
            auto pos = World::toWorldSpace(step) + World::Pos2(16, 16);
            auto height = World::TileManager::getHeight(pos);
            auto main = Ui::WindowManager::getMainWindow();
            if (main != nullptr)
            {
                auto pos3d = World::Pos3(pos.x, pos.y, height.landHeight);
                main->viewportCentreOnTile(pos3d);
                main->flags &= ~Ui::WindowFlags::scrollingToLocation;
                main->viewportsUpdatePosition();
            }
        }
    }

    static void handleStep([[maybe_unused]] const RotateStep step)
    {
        if (Game::hasFlags(GameStateFlags::tileManagerLoaded))
        {
            auto main = Ui::WindowManager::getMainWindow();
            if (main != nullptr)
            {
                main->viewportRotateRight();
            }
        }
    }

    static void handleStep([[maybe_unused]] const ResetStep step)
    {
        _sequenceIterator = _titleSequence.begin();
    }

    // 0x00444387
    void update()
    {
        if (!SceneManager::isTitleMode())
        {
            return;
        }

        SceneManager::resetSceneAge();

        if (_waitCounter > 0)
        {
            _waitCounter -= 1;
            return;
        }

        do
        {
            if (_sequenceIterator >= _titleSequence.end())
            {
                return;
            }

            auto& command = *_sequenceIterator++;
            std::visit([](auto&& step) { handleStep(step); }, command);
        } while (_waitCounter == 0);
    }
}

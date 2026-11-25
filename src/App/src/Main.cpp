#include <OpenLoco/Audio/Audio.h>
#include <OpenLoco/CommandLine.h>
#include <OpenLoco/Config.h>
#include <OpenLoco/Core/Exception.hpp>
#include <OpenLoco/Core/FileStream.h>
#include <OpenLoco/Core/MemoryStream.h>
#include <OpenLoco/Environment.h>
#include <OpenLoco/GameSaveCompare.h>
#include <OpenLoco/GameState.h>
#include <OpenLoco/Logging.h>
#include <OpenLoco/Objects/Object.h>
#include <OpenLoco/OpenLoco.h>
#include <OpenLoco/Platform/Crash.h>
#include <OpenLoco/Platform/Platform.h>
#include <OpenLoco/S5/S5.h>
#include <OpenLoco/S5/SawyerStream.h>
#include <OpenLoco/Ui/Screenshot.h>
#include <OpenLoco/Version.hpp>
#include <SDL3/SDL_main.h>
#include <fmt/chrono.h>
#include <iostream>

using namespace OpenLoco::Diagnostics;

namespace OpenLoco
{

    static void printVersion()
    {
        auto versionInfo = Version::getVersionInfo();
        std::cout << versionInfo << std::endl;
    }

    static void printHelp()
    {
        std::cout << "usage: openloco [options] [path]" << std::endl;
        std::cout << "                host [options] <path>" << std::endl;
        std::cout << "                join [options] <address>" << std::endl;
        std::cout << "                uncompress [options] <path>" << std::endl;
        std::cout << "                simulate [options] <path> <ticks> [path]" << std::endl;
        std::cout << "                compare [options] <path1> <path2>" << std::endl;
        std::cout << "                screenshot [options] <path> [ticks] -o <png>" << std::endl;
        std::cout << "                new-game [ticks] -o <sv5> [--select NAME,NAME...]" << std::endl;
        std::cout << "                build-scene <scene.yml> -o <sv5>" << std::endl;
        std::cout << std::endl;
        std::cout << "options:" << std::endl;
        std::cout << "--bind                     Address to bind to when hosting a server" << std::endl;
        std::cout << "--port               -p     Port number for the server" << std::endl;
        std::cout << "                     -o     Output path" << std::endl;
        std::cout << "--help               -h     Print help" << std::endl;
        std::cout << "--version                   Print version" << std::endl;
        std::cout << "--intro                     Run the game intro" << std::endl;
        std::cout << "--log_levels                Comma separated list of log levels, applying a minus prefix" << std::endl;
        std::cout << "                            removes the level from a group such as 'all', valid levels:" << std::endl;
        std::cout << "                            - info, warning, error, verbose, all" << std::endl;
        std::cout << "                              Example: --log_levels \"all, -verbose\", logs all but verbose levels" << std::endl;
        std::cout << "                              Default: \"info, warning, error\"" << std::endl;
        std::cout << "--all                -a     For compare, print out all divergences" << std::endl;
        std::cout << "--locomotion_path           Overrides the path to Locomotion install." << std::endl;
        std::cout << "--zoom                      For screenshot, zoom level 0-3 (default 0)" << std::endl;
        std::cout << "--rotation                  For screenshot, view rotation 0-3 (default 0)" << std::endl;
        std::cout << "--centre                    For screenshot, centre tile \"x,y\" (default: saved view)" << std::endl;
        std::cout << "--size                      For screenshot, image size \"WxH\" (default 1280x720)" << std::endl;
        std::cout << "--giant                     For screenshot, render the whole map" << std::endl;
        std::cout << "--object                    For screenshot, render the named object's selection preview instead" << std::endl;
        std::cout << "--exit-after-frames         Run the game for N frames, save the screen to -o (if given) and exit" << std::endl;
    }

    static int uncompressFile(const CommandLineOptions& options)
    {
        using namespace S5;

        if (options.path.empty())
        {
            Logging::error("No file specified.");
            return EXIT_FAILURE;
        }

        try
        {
            auto path = fs::u8path(options.path);

            // Copy the whole input file into memory to allow writing over input file
            MemoryStream ms;
            {
                FileStream fsInput(path, StreamMode::read);
                ms.resize(fsInput.getLength());
                fsInput.read(ms.data(), fsInput.getLength());
            }
            SawyerStreamReader reader(ms);

            auto outputPath = options.outputPath;
            if (outputPath.empty())
            {
                outputPath = options.path;
            }

            FileStream fsOutput(outputPath, StreamMode::write);
            SawyerStreamWriter writer(fsOutput);

            if (!reader.validateChecksum())
            {
                throw Exception::RuntimeError("Invalid checksum");
            }

            // Header chunk
            Header header;
            reader.readChunk(&header, sizeof(header));
            writer.writeChunk(SawyerEncoding::uncompressed, header);

            // Optional save details chunk
            if (header.hasFlags(HeaderFlags::hasSaveDetails))
            {
                auto saveDetails = std::make_unique<SaveDetails>();
                reader.readChunk(saveDetails.get(), sizeof(SaveDetails));
                writer.writeChunk(SawyerEncoding::uncompressed, *saveDetails);
            }

            // Packed objects
            for (auto i = 0; i < header.numPackedObjects; i++)
            {
                ObjectHeader object;
                reader.read(&object, sizeof(ObjectHeader));
                writer.write(&object, sizeof(ObjectHeader));

                auto chunk = reader.readChunk();
                writer.writeChunk(SawyerEncoding::uncompressed, chunk.data(), chunk.size());
            }

            if (header.type != S5Type::objects)
            {
                // Required objects chunk
                auto chunk = reader.readChunk();
                writer.writeChunk(SawyerEncoding::uncompressed, chunk.data(), chunk.size());

                // Game state chunk
                chunk = reader.readChunk();
                writer.writeChunk(SawyerEncoding::uncompressed, chunk.data(), chunk.size());

                // Tile elements chunk
                chunk = reader.readChunk();
                writer.writeChunk(SawyerEncoding::uncompressed, chunk.data(), chunk.size());
            }

            writer.writeChecksum();

            return EXIT_SUCCESS;
        }
        catch (const std::exception& e)
        {
            Logging::error("Unable to uncompress S5 file: {}", e.what());
            return EXIT_FAILURE;
        }
    }

    static int simulate(const CommandLineOptions& options)
    {
        if (!options.ticks)
        {
            Logging::error("Number of ticks to simulate not specified");
            return EXIT_FAILURE;
        }

        auto inPath = fs::u8path(options.path);
        auto outPath = fs::u8path(options.outputPath);
        auto comparePath = fs::u8path(options.path2);

        const auto timeStarted = std::chrono::high_resolution_clock::now();

        try
        {
            if (!OpenLoco::simulateGame(inPath, *options.ticks))
            {
                return EXIT_FAILURE;
            }
        }
        catch (...)
        {
            Logging::error("Unable to load and simulate {}", inPath.u8string());
            return EXIT_FAILURE;
        }

        if (!options.path2.empty())
        {
            OpenLoco::GameSaveCompare::compareGameStates(comparePath);
        }

        const auto timeElapsed = std::chrono::high_resolution_clock::now() - timeStarted;

        auto& gameState = getGameState();
        Logging::info("--------------------------------");
        Logging::info("- Simulate");
        Logging::info("--------------------------------");
        Logging::info("Input:");
        Logging::info("  path: {}", inPath.u8string());
        Logging::info("  ticks: {} ticks", *options.ticks);
        Logging::info("Output:");
        Logging::info("  scenario ticks: {}", gameState.scenarioTicks);
        Logging::info("  rng:            {{ {}, {} }}", gameState.rng.srand_0(), gameState.rng.srand_1());
        Logging::info("Duration: {:%S} sec", timeElapsed);

        if (!outPath.empty())
        {
            try
            {
                S5::exportGameStateToFile(outPath, S5::SaveFlags::none);
                std::printf("  path:           %s\n", outPath.u8string().c_str());
            }
            catch (...)
            {
                Logging::error("Unable to save game to {}", outPath.u8string());
                return EXIT_FAILURE;
            }
        }

        return EXIT_SUCCESS;
    }

    static int compare(const CommandLineOptions& options)
    {
        auto file1 = fs::u8path(options.path);
        auto file2 = fs::u8path(options.path2);
        auto displayAllDivergences = options.all.empty() == false;

        if (file1.empty() || file2.empty())
        {
            Logging::error("Unable to compare gamestates...");
            Logging::error("    The required compare file paths have not been specified.");
            Logging::error("    compare [options] <path1> <path2>");
            return EXIT_FAILURE;
        }

        auto result = EXIT_SUCCESS;
        try
        {
            if (OpenLoco::GameSaveCompare::compareGameStates(file1, file2, displayAllDivergences))
            {
                Logging::info("MATCHES", file1, file2);
            }
            else
            {
                Logging::error("DOES NOT MATCH", file1, file2);
                result = EXIT_FAILURE;
            }
        }
        catch (std::exception& e)
        {
            Logging::error("Unable to compare gamestates: {} to {}", file1, file2);
            Logging::error("Exception reason {}", e.what());
            result = EXIT_FAILURE;
        }

        return result;
    }

    // 0x00406386
    static void run(const CommandLineOptions& options)
    {
        auto& cfg = Config::get();

        // Window creation must be done before Gfx can be initialized.
        Ui::createWindow(cfg.display);
        Ui::initialiseCursors();
        Audio::initialiseDSound();

        initialise();

        bool windowsOpened = false;
        while (Input::processMessages())
        {
            update();

            // OpenGraphics fork：截圖前 30 個畫面開啟 --open-window 指定的視窗（讓視窗有時間畫出來）
            if (!windowsOpened && !options.openWindow.empty() && options.exitAfterFrames.has_value()
                && getRenderedFrameCount() + 30 >= static_cast<uint32_t>(*options.exitAfterFrames))
            {
                openWindowsByName(options.openWindow);
                windowsOpened = true;
            }

            // OpenGraphics fork：畫完指定的畫面數後存下整個畫面並結束（自動化測試用）
            if (options.exitAfterFrames.has_value() && getRenderedFrameCount() >= static_cast<uint32_t>(*options.exitAfterFrames))
            {
                if (!options.outputPath.empty())
                {
                    Ui::saveScreenToFile(fs::u8path(options.outputPath));
                    Logging::info("Screen saved to {}", options.outputPath);
                }
                break;
            }
        }
    }

    static int screenshot(const CommandLineOptions& options)
    {
        if (options.outputPath.empty())
        {
            Logging::error("Output path (-o) not specified");
            return EXIT_FAILURE;
        }

        Ui::HeadlessScreenshotOptions ssOptions{};
        ssOptions.zoom = static_cast<int8_t>(options.zoom.value_or(0));
        ssOptions.rotation = static_cast<int8_t>(options.rotation.value_or(0));
        ssOptions.giant = options.giant;
        ssOptions.objectName = options.object;
        if (!options.size.empty())
        {
            int32_t w = 0, h = 0;
            if (std::sscanf(options.size.c_str(), "%dx%d", &w, &h) != 2)
            {
                Logging::error("Invalid --size, expected WxH");
                return EXIT_FAILURE;
            }
            ssOptions.width = w;
            ssOptions.height = h;
        }
        if (options.centre.rfind("town:", 0) == 0 || options.centre.rfind("industry:", 0) == 0)
        {
            ssOptions.centreTarget = options.centre;
        }
        else if (!options.centre.empty())
        {
            int32_t x = 0, y = 0;
            if (std::sscanf(options.centre.c_str(), "%d,%d", &x, &y) != 2)
            {
                Logging::error("Invalid --centre, expected x,y (tiles)");
                return EXIT_FAILURE;
            }
            ssOptions.centreTile = std::make_pair(x, y);
        }

        const auto inPath = fs::u8path(options.path);
        const auto outPath = fs::u8path(options.outputPath);
        if (!OpenLoco::screenshotGame(inPath, options.ticks.value_or(0), outPath, ssOptions))
        {
            return EXIT_FAILURE;
        }
        Logging::info("Screenshot saved to {}", outPath.u8string());
        return EXIT_SUCCESS;
    }

    static std::optional<int> runCommandLineOnlyCommand(const CommandLineOptions& options)
    {
        switch (options.action)
        {
            case CommandLineAction::help:
                printHelp();
                return EXIT_FAILURE;
            case CommandLineAction::version:
                printVersion();
                return EXIT_FAILURE;
            case CommandLineAction::uncompress:
                return uncompressFile(options);
            case CommandLineAction::simulate:
                return simulate(options);
            case CommandLineAction::compare:
                return compare(options);
            case CommandLineAction::screenshot:
                return screenshot(options);
            case CommandLineAction::buildScene:
                if (options.outputPath.empty() || options.path.empty())
                {
                    Logging::error("Usage: build-scene <scene.yml> -o <sv5>");
                    return EXIT_FAILURE;
                }
                return OpenLoco::buildScene(fs::u8path(options.path), fs::u8path(options.outputPath)) ? EXIT_SUCCESS : EXIT_FAILURE;
            case CommandLineAction::newGame:
                if (options.outputPath.empty())
                {
                    Logging::error("Output path (-o) not specified");
                    return EXIT_FAILURE;
                }
                {
                    std::vector<std::string> preferred;
                    std::string current;
                    for (const char c : options.select + ",")
                    {
                        if (c == ',')
                        {
                            if (!current.empty())
                            {
                                preferred.push_back(current);
                            }
                            current.clear();
                        }
                        else
                        {
                            current += c;
                        }
                    }
                    return OpenLoco::newGame(fs::u8path(options.outputPath), options.ticks.value_or(0), preferred) ? EXIT_SUCCESS : EXIT_FAILURE;
                }
            default:
                return std::nullopt;
        }
    }

    // 0x00406D13
    static int main(CommandLineOptions&& options)
    {
        // Bootstrap the logging system.
        Logging::initialize(options.logLevels);

        // Always print the product name, version, and platform info first.
        Logging::info("{}", Version::getVersionInfo());
        Logging::info("{}", Version::getPlatformInfo());

        setCommandLineOptions(options);

        Config::read();
        if (options.locomotionDataPath.has_value())
        {
            Config::get().locoInstallPath = options.locomotionDataPath.value();
        }
        Environment::setLocale();
        Environment::resolvePaths();

        auto ret = runCommandLineOnlyCommand(options);
        if (ret)
        {
            return *ret;
        }

        if (!OpenLoco::Platform::isRunningInWine())
        {
            CrashHandler::AppInfo appInfo;
            appInfo.name = "OpenLoco";
            appInfo.version = Version::getVersionInfo();

            CrashHandler::init(appInfo);
        }
        else
        {
            Logging::warn("Detected wine, not installing crash handler as it doesn't provide useful data. Consider using native builds of OpenLoco instead.");
        }

        try
        {
            run(options);
            exitCleanly();
        }
        catch (const std::exception& e)
        {
            Logging::error("Exception: {}", e.what());
            Ui::showMessageBox("Exception", e.what());
            exitCleanly();
        }
        catch (...)
        {
            Ui::showMessageBox("Exception", "Unsure what threw the exception!");
            exitCleanly();
        }
    }

}

int main(int argc, const char** argv)
{
    OpenLoco::Platform::initialise();

    auto options = OpenLoco::parseCommandLine(OpenLoco::Platform::getCmdLineVector(argc, argv));
    if (!options)
    {
        OpenLoco::printHelp();
        return EXIT_FAILURE;
    }

    SDL_SetMainReady();

    return OpenLoco::main(std::move(*options));
}

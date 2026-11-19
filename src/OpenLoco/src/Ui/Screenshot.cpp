#include "Ui/Screenshot.h"
#include "Entities/EntityManager.h"
#include "Environment.h"
#include "GameState.h"
#include "Graphics/Gfx.h"
#include "Graphics/RenderTarget.h"
#include "Graphics/SoftwareDrawingEngine.h"
#include "Localisation/FormatArguments.hpp"
#include "Localisation/StringIds.h"
#include "Map/TileManager.h"
#include "Objects/ObjectIndex.h"
#include "Ui.h"
#include "Ui/WindowManager.h"
#include <OpenLoco/Core/Exception.hpp>
#include <OpenLoco/Platform/Platform.h>
#include <OpenLoco/Utility/String.hpp>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <png.h>
#include <string>

#pragma warning(disable : 4611) // interaction between '_setjmp' and C++ object destruction is non-portable

using namespace OpenLoco::Ui;

namespace OpenLoco::Ui
{
    static int8_t _screenshotCountdown = 0; // 0x00508F16

    static ScreenshotType _screenshotType = ScreenshotType::regular;

    void triggerScreenshotCountdown(int8_t numTicks, ScreenshotType type)
    {
        _screenshotCountdown = numTicks;
        _screenshotType = type;
    }

    static std::string saveScreenshot();
    static std::string saveGiantScreenshot();

    void handleScreenshotCountdown()
    {
        if (_screenshotCountdown != 0)
        {
            _screenshotCountdown--;
            if (_screenshotCountdown == 0)
            {
                try
                {
                    std::string fileName;
                    if (_screenshotType == ScreenshotType::giant)
                    {
                        fileName = saveGiantScreenshot();
                    }
                    else
                    {
                        fileName = saveScreenshot();
                    }

                    FormatArguments::common(fileName.c_str());
                    Windows::Error::openQuiet(StringIds::screenshot_saved_as, StringIds::null);
                }
                catch (const std::exception&)
                {
                    Windows::Error::open(StringIds::screenshot_failed);
                }
            }
        }
    }

    static void pngWriteData(png_structp png_ptr, png_bytep data, png_size_t length)
    {
        auto ostream = static_cast<std::ostream*>(png_get_io_ptr(png_ptr));
        ostream->write((const char*)data, length);
    }

    static void pngFlush(png_structp png_ptr)
    {
        auto ostream = static_cast<std::ostream*>(png_get_io_ptr(png_ptr));
        ostream->flush();
    }

    static void saveRenderTargetToPng(const Gfx::RenderTarget& rt, std::fstream& outputStream)
    {
        auto rgbaPalette = Gfx::getRgbaPalette();

        png_structp pngPtr = nullptr;
        png_colorp palette = nullptr;
        try
        {
            pngPtr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
            if (pngPtr == nullptr)
            {
                throw Exception::RuntimeError("png_create_write_struct failed.");
            }

            png_set_write_fn(pngPtr, &outputStream, pngWriteData, pngFlush);

            // Set error handler
            if (setjmp(png_jmpbuf(pngPtr)))
            {
                throw Exception::RuntimeError("PNG ERROR");
            }

            auto infoPtr = png_create_info_struct(pngPtr);
            if (infoPtr == nullptr)
            {
                throw Exception::RuntimeError("png_create_info_struct failed.");
            }

            palette = (png_colorp)png_malloc(pngPtr, rgbaPalette.size() * sizeof(png_color));
            if (palette == nullptr)
            {
                throw Exception::RuntimeError("png_malloc failed.");
            }

            for (size_t i = 0; i < rgbaPalette.size(); i++)
            {
                palette[i].blue = rgbaPalette[i].b;
                palette[i].green = rgbaPalette[i].g;
                palette[i].red = rgbaPalette[i].r;
            }
            // fork：寫入完整調色盤，避免畫面含有未套 remap 的 index（例如 placeholder）時 libpng 報錯
            png_set_PLTE(pngPtr, infoPtr, palette, static_cast<int>(rgbaPalette.size()));

            png_byte transparentIndex = 0;
            png_set_tRNS(pngPtr, infoPtr, &transparentIndex, 1, nullptr);
            png_set_IHDR(pngPtr, infoPtr, rt.width, rt.height, 8, PNG_COLOR_TYPE_PALETTE, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
            png_write_info(pngPtr, infoPtr);

            uint8_t* data = rt.bits;
            for (int y = 0; y < rt.height; y++)
            {
                png_write_row(pngPtr, data);
                data += rt.pitch + rt.width;
            }

            png_write_end(pngPtr, nullptr);
            png_destroy_info_struct(pngPtr, &infoPtr);
            png_free(pngPtr, palette);
            png_destroy_write_struct(&pngPtr, nullptr);
        }
        catch (const std::exception&)
        {
            png_free(pngPtr, palette);
            png_destroy_write_struct(&pngPtr, nullptr);
            throw;
        }
    }

    // 0x00452667
    static std::string prepareSaveScreenshot(const Gfx::RenderTarget& rt)
    {
        auto screenshotsFolderPath = Environment::getPathNoWarning(Environment::PathId::screenshots);
        Environment::autoCreateDirectory(screenshotsFolderPath);

        std::string scenarioName = getGameState().scenarioName;
        if (scenarioName.length() == 0)
        {
            scenarioName = StringManager::getString(StringIds::screenshot_filename_template);
        }

        std::string fileName = std::string(scenarioName) + ".png";
        fs::path path;
        int16_t suffix;
        for (suffix = 1; suffix < std::numeric_limits<int16_t>().max(); suffix++)
        {
            if (!fs::exists(screenshotsFolderPath / fileName))
            {
                path = screenshotsFolderPath / fileName;
                break;
            }

            fileName = std::string(scenarioName) + " (" + std::to_string(suffix) + ").png";
        }

        if (path.empty())
        {
            throw Exception::RuntimeError("Failed finding filename");
        }

        std::fstream outputStream(path.c_str(), std::ios::out | std::ios::binary);
        saveRenderTargetToPng(rt, outputStream);

        return fileName;
    }

    static std::string saveScreenshot()
    {
        auto& drawingEngine = Gfx::getDrawingEngine();
        auto& rt = drawingEngine.getScreenRT();
        return prepareSaveScreenshot(rt);
    }

    static Ui::Viewport createGiantViewport(const uint16_t resolutionWidth, const uint16_t resolutionHeight, const ZoomLevel zoomLevel)
    {
        Ui::Viewport viewport{};
        viewport.width = resolutionWidth;
        viewport.height = resolutionHeight;
        viewport.x = 0;
        viewport.y = 0;
        viewport.viewWidth = zoomLevel.applyTo<int32_t>(viewport.width);
        viewport.viewHeight = zoomLevel.applyTo<int32_t>(viewport.height);
        viewport.zoom = zoomLevel;
        viewport.pad_11 = 0;
        viewport.flags = ViewportFlags::none;

        const coord_t centreX = (World::kMapColumns / 2) * 32 + 16;
        const coord_t centreY = (World::kMapRows / 2) * 32 + 16;
        const coord_t z = World::TileManager::getHeight({ centreX, centreY }).landHeight;

        auto pos = viewport.centre2dCoordinates({ centreX, centreY, z });
        viewport.viewX = pos.x;
        viewport.viewY = pos.y;

        return viewport;
    }

    static std::string saveGiantScreenshot()
    {
        const auto& main = WindowManager::getMainWindow();

        const auto zoomLevel = ZoomLevel{ std::max<int8_t>(static_cast<int8_t>(main->viewports[0]->zoom), ZoomLevel::full) };

        const uint16_t resolutionWidth = zoomLevel.applyInversedTo(World::kMapColumns * 32 * 2) + 8;
        const uint16_t resolutionHeight = zoomLevel.applyInversedTo(World::kMapRows * 32 * 1) + 128;

        Ui::Viewport viewport = createGiantViewport(resolutionWidth, resolutionHeight, zoomLevel);

        // Ensure sprites appear regardless of rotation
        EntityManager::resetSpatialIndex();

        Gfx::RenderTarget rt{};
        rt.bits = static_cast<uint8_t*>(malloc(resolutionWidth * resolutionHeight));
        if (rt.bits == nullptr)
        {
            return {};
        }

        rt.x = 0;
        rt.y = 0;
        rt.width = resolutionWidth;
        rt.height = resolutionHeight;
        rt.pitch = 0;

        auto& drawingEngine = Gfx::getDrawingEngine();
        auto& drawingCtx = drawingEngine.getDrawingContext();

        drawingCtx.pushRenderTarget(rt);

        viewport.render(drawingCtx);

        drawingCtx.popRenderTarget();

        auto fileName = prepareSaveScreenshot(rt);
        free(rt.bits);

        return fileName;
    }

    static void saveObjectPreviewToFile(const fs::path& path, const std::string& objectName)
    {
        std::optional<ObjectHeader> header;
        const auto numObjects = ObjectManager::getNumInstalledObjects();
        for (uint32_t i = 0; i < numObjects; i++)
        {
            const auto& entry = ObjectManager::getObjectInIndex(static_cast<ObjectManager::ObjectIndexId>(i));
            if (Utility::iequals(Utility::trim(entry._header.getName()), objectName))
            {
                header = entry._header;
                break;
            }
        }
        if (!header.has_value())
        {
            throw Exception::RuntimeError("Object not found in index: " + objectName);
        }

        const auto size = Windows::ObjectSelectionWindow::getObjectPreviewSize();
        Gfx::RenderTarget rt{};
        rt.bits = static_cast<uint8_t*>(malloc(static_cast<size_t>(size.width) * size.height));
        if (rt.bits == nullptr)
        {
            throw Exception::RuntimeError("Out of memory for object preview");
        }
        rt.x = 0;
        rt.y = 0;
        rt.width = size.width;
        rt.height = size.height;
        rt.pitch = 0;

        auto& drawingCtx = Gfx::getDrawingEngine().getDrawingContext();
        drawingCtx.pushRenderTarget(rt);
        drawingCtx.clear(0x0E); // 深灰底，接近物件選擇視窗
        const bool drawn = Windows::ObjectSelectionWindow::drawObjectPreview(*header, drawingCtx);
        drawingCtx.popRenderTarget();
        if (!drawn)
        {
            free(rt.bits);
            throw Exception::RuntimeError("Unable to load object for preview: " + objectName);
        }

        std::fstream outputStream(path, std::ios::out | std::ios::binary);
        if (!outputStream)
        {
            free(rt.bits);
            throw Exception::RuntimeError("Unable to open preview output file");
        }
        saveRenderTargetToPng(rt, outputStream);
        free(rt.bits);
    }

    void saveScreenshotToFile(const fs::path& path, const HeadlessScreenshotOptions& options)
    {
        if (!options.objectName.empty())
        {
            saveObjectPreviewToFile(path, options.objectName);
            return;
        }

        WindowManager::setCurrentRotation(options.rotation & 3);
        // Ensure sprites appear regardless of rotation
        EntityManager::resetSpatialIndex();

        const auto zoomLevel = ZoomLevel{ static_cast<int8_t>(std::clamp<int32_t>(options.zoom, 0, 3)) };

        uint16_t width{};
        uint16_t height{};
        Ui::Viewport viewport{};
        if (options.giant)
        {
            width = zoomLevel.applyInversedTo(World::kMapColumns * 32 * 2) + 8;
            height = zoomLevel.applyInversedTo(World::kMapRows * 32 * 1) + 128;
            viewport = createGiantViewport(width, height, zoomLevel);
        }
        else
        {
            width = static_cast<uint16_t>(std::clamp<int32_t>(options.width, 16, 8192));
            height = static_cast<uint16_t>(std::clamp<int32_t>(options.height, 16, 8192));
            viewport.x = 0;
            viewport.y = 0;
            viewport.width = width;
            viewport.height = height;
            viewport.zoom = zoomLevel;
            viewport.viewWidth = zoomLevel.applyTo<int32_t>(width);
            viewport.viewHeight = zoomLevel.applyTo<int32_t>(height);
            viewport.flags = ViewportFlags::none;

            World::Pos2 centre{ (World::kMapColumns / 2) * 32 + 16, (World::kMapRows / 2) * 32 + 16 };
            if (options.centreTile.has_value())
            {
                centre = World::Pos2(options.centreTile->first * 32 + 16, options.centreTile->second * 32 + 16);
            }
            else if (auto* mainViewport = WindowManager::getMainViewport(); mainViewport != nullptr)
            {
                centre = mainViewport->getCentreMapPosition();
            }
            const coord_t z = World::TileManager::getHeight(centre).landHeight;
            const auto pos = viewport.centre2dCoordinates({ centre.x, centre.y, z });
            viewport.viewX = pos.x;
            viewport.viewY = pos.y;
        }

        Gfx::RenderTarget rt{};
        rt.bits = static_cast<uint8_t*>(malloc(static_cast<size_t>(width) * height));
        if (rt.bits == nullptr)
        {
            throw Exception::RuntimeError("Out of memory for screenshot");
        }
        rt.x = 0;
        rt.y = 0;
        rt.width = width;
        rt.height = height;
        rt.pitch = 0;

        auto& drawingEngine = Gfx::getDrawingEngine();
        auto& drawingCtx = drawingEngine.getDrawingContext();
        drawingCtx.pushRenderTarget(rt);
        viewport.render(drawingCtx);
        drawingCtx.popRenderTarget();

        std::fstream outputStream(path, std::ios::out | std::ios::binary);
        if (!outputStream)
        {
            free(rt.bits);
            throw Exception::RuntimeError("Unable to open screenshot output file");
        }
        saveRenderTargetToPng(rt, outputStream);
        free(rt.bits);
    }
}

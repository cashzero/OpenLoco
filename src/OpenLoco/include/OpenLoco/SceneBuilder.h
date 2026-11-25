#pragma once

#include <OpenLoco/Core/FileSystem.hpp>

// OpenGraphics fork：依場景腳本（YAML）建造地圖並存檔，用來製作標題畫面（title.dat）與範例情境。
// 腳本格式見 OpenGraphics 的 base/title/README.md。
namespace OpenLoco::SceneBuilder
{
    bool build(const fs::path& scenePath, const fs::path& outputPath);
}

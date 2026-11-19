#pragma once

#include "Object.h"
#include <optional>

// OpenGraphics fork：vanilla 物件 → OpenGraphics 物件的別名層。
// 別名表為 <objects 目錄>/aliases.yml（由 OpenGraphics 的 tools 產生），第一次查詢時載入。
// 找不到 vanilla 物件時改用對應的 OpenGraphics 物件，讓存檔、情境與引用 vanilla 的物件
// 在沒有安裝 Locomotion 物件時仍能載入。
namespace OpenLoco::ObjectAliases
{
    // 重新讀取別名表
    void reload();

    // vanilla header → OpenGraphics header；沒有別名時回傳 std::nullopt
    std::optional<ObjectHeader> find(const ObjectHeader& header);

    // 環境變數 OPENLOCO_PREFER_OPENGRAPHICS=1：即使 vanilla 物件存在也優先使用別名
    bool isPreferred();
}

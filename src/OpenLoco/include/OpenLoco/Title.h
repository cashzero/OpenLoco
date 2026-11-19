#pragma once

#include <string>
#include <vector>

namespace OpenLoco::Title
{
    void start();
    void update();
    void stop();
    void reset();

    // OpenGraphics fork：以已安裝的物件選出一組可開新遊戲的物件並載入，回傳物件選擇是否通過 OpenLoco 的驗證。
    // preferredObjects 為空：預設物件 + 每種必選類型各一 + 全部地形／樹／建築／圍牆／道路（標題地圖用）。
    // 指定 preferredObjects（內部名稱）：不選預設物件，先選指定的物件，其餘必選類型各補一個（headless new-game 用）。
    bool loadStandaloneWorldObjects(const std::vector<std::string>& preferredObjects = {});
}

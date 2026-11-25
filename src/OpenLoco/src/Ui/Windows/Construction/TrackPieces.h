#pragma once

#include <cstdint>
#include <optional>

// 建造視窗的軌道／道路片段選擇（直線、各種彎道、S 彎與坡度）→ 實際的 trackId／roadId 與旋轉。
// OpenGraphics fork：從 ConstructionTab.cpp 移出，讓場景建造腳本（SceneBuilder）也能使用。
namespace OpenLoco::Ui::Windows::Construction::Construction
{
    namespace TrackPiece
    {
        enum
        {
            straight,
            left_hand_curve_very_small,
            right_hand_curve_very_small,
            left_hand_curve_small,
            right_hand_curve_small,
            left_hand_curve,
            right_hand_curve,
            left_hand_curve_large,
            right_hand_curve_large,
            s_bend_left,
            s_bend_right,
            s_bend_to_dual_track,
            s_bend_to_single_track,
            turnaround,
            null = 0xFF,
        };
    }

    namespace TrackGradient
    {
        enum
        {
            level = 0,
            slope_up = 2,
            steep_slope_up = 4,
            slope_down = 6,
            steep_slope_down = 8,
        };
    };

    struct TrackPieceId
    {
        uint8_t id;
        uint8_t rotation;
    };

    std::optional<TrackPieceId> getTrackPieceId(uint8_t trackPiece, uint8_t gradient, uint8_t rotation);
    std::optional<TrackPieceId> getRoadPieceId(uint8_t trackPiece, uint8_t gradient, uint8_t rotation);
}

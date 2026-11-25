#include "SceneBuilder.h"
#include "Config.h"
#include "Date.h"
#include "EditorController.h"
#include "Entities/EntityManager.h"
#include "Game.h"
#include "GameCommands/Airports/CreateAirport.h"
#include "GameCommands/Buildings/CreateBuilding.h"
#include "GameCommands/Docks/CreatePort.h"
#include "GameCommands/GameCommands.h"
#include "GameCommands/Industries/CreateIndustry.h"
#include "GameCommands/Industries/RemoveIndustry.h"
#include "GameCommands/Road/CreateRoad.h"
#include "GameCommands/Road/CreateRoadStation.h"
#include "GameCommands/Terraform/ChangeLandMaterial.h"
#include "GameCommands/Terraform/ClearLand.h"
#include "GameCommands/Terraform/CreateTree.h"
#include "GameCommands/Terraform/LowerLand.h"
#include "GameCommands/Terraform/RaiseLand.h"
#include "GameCommands/Town/CreateTown.h"
#include "GameCommands/Track/CreateSignal.h"
#include "GameCommands/Track/CreateTrack.h"
#include "GameCommands/Track/CreateTrainStation.h"
#include "GameCommands/Vehicles/CreateVehicle.h"
#include "GameCommands/Vehicles/VehicleChangeRunningMode.h"
#include "GameCommands/Vehicles/VehicleOrderInsert.h"
#include "GameCommands/Vehicles/VehiclePlace.h"
#include "GameCommands/Vehicles/VehiclePlaceAir.h"
#include "GameCommands/Vehicles/VehiclePlaceWater.h"
#include "GameState.h"
#include "GameStateFlags.h"
#include "Localisation/Formatting.h"
#include "Localisation/StringIds.h"
#include "Localisation/StringManager.h"
#include "Logging.h"
#include "Map/MapGenerator/MapGenerator.h"
#include "Map/MapSelection.h"
#include "Map/StationElement.h"
#include "Map/SurfaceElement.h"
#include "Map/TileManager.h"
#include "Map/Track/TrackData.h"
#include "Map/TrackElement.h"
#include "Objects/AirportObject.h"
#include "Objects/ObjectManager.h"
#include "Objects/TrackObject.h"
#include "S5/S5.h"
#include "Scenario/Scenario.h"
#include "Scenario/ScenarioOptions.h"
#include "SceneManager.h"
#include "Scenes/GameScene.h"
#include "Title.h"
#include "Ui/Windows/Construction/TrackPieces.h"
#include "Vehicles/Orders.h"
#include "Vehicles/VehicleHead.h"
#include "World/CompanyManager.h"
#include "World/IndustryManager.h"
#include "World/Town.h"
#include "World/TownManager.h"
#include <OpenLoco/Core/Prng.h>
#include <array>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

using namespace OpenLoco::Diagnostics;
using namespace OpenLoco::World;
namespace Pieces = OpenLoco::Ui::Windows::Construction::Construction;

// 場景腳本（YAML）：
//   year, seed, heightmap（相對於腳本的 PNG）, sea_level, trees {forests, random, radius, density, altitude},
//   remove_generated_industries, freeze_industries, company_colours [主色, 副色], title_sequence（存成標題畫面存檔）,
//   objects [內部名稱…], ops [...],
//   simulate（存檔前模擬的 tick 數）, date [年, 月, 日]
// 座標一律是「格」（tile），高度是「級」（1 級 = 16 世界單位 = 一段坡道的落差）。
// ops：town、industry、building、track、road、airport、dock、vehicle（train 為別名）、trees、clear、land、raise、lower、simulate、
//   probe（除錯：把區域內容印成文字地圖）。
// track／road 的 path 以空白分隔片段：S 直線、L1–L4／R1–R4 左右彎（極小、小、中、大）、SBL／SBR S 彎、
//   ST 月台（直線＋車站）、F 順著地形的直線（自動上下坡）、SIG（在前一段放號誌，僅 track）；後綴 + 上坡、++ 陡上坡、- 下坡、-- 陡下坡；
//   :名稱 = 標籤（重複時記在最後一段）；*N = 重複 N 次。
namespace OpenLoco::SceneBuilder
{
    static constexpr int32_t kHeightStep = 16;

    struct SceneError : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    enum class LabelKind : uint8_t
    {
        rail,
        road,
        airport,
        dock,
    };

    // 具名位置：車站（月台的某一格）、機場、碼頭。車輛放置與路線用。
    struct Label
    {
        LabelKind kind;
        Pos3 pos;
        uint8_t rotation;
        uint8_t pieceId;
    };

    struct BuildState
    {
        fs::path baseDir;
        std::map<std::string, Label> labels;
        size_t opIndex = 0;
        Core::Prng rng{ 0x1234, 0x5678 };
    };

    static std::string lastErrorText()
    {
        char buffer[512]{};
        StringManager::formatString(buffer, sizeof(buffer), GameCommands::getErrorText());
        return buffer;
    }

    static std::string trimmed(std::string_view s)
    {
        while (!s.empty() && s.back() == ' ')
        {
            s.remove_suffix(1);
        }
        return std::string(s);
    }

    static uint16_t findObject(ObjectType type, const std::string& name)
    {
        for (size_t i = 0; i < ObjectManager::getMaxObjects(type); i++)
        {
            LoadedObjectHandle handle{ type, static_cast<LoadedObjectId>(i) };
            if (ObjectManager::getAny(handle) == nullptr)
            {
                continue;
            }
            if (trimmed(ObjectManager::getHeader(handle).getName()) == name)
            {
                return static_cast<uint16_t>(i);
            }
        }
        throw SceneError("object " + name + " is not loaded (add it to `objects`)");
    }

    static Pos2 tileToWorld(const YAML::Node& node)
    {
        if (!node.IsSequence() || node.size() < 2)
        {
            throw SceneError("expected a tile position [x, y]");
        }
        return Pos2(node[0].as<int32_t>() * kTileSize, node[1].as<int32_t>() * kTileSize);
    }

    static std::string tileText(const Pos3& pos)
    {
        return std::to_string(pos.x / kTileSize) + "," + std::to_string(pos.y / kTileSize) + " z" + std::to_string(pos.z / kHeightStep);
    }

    static int32_t landHeight(const Pos2& pos)
    {
        return TileManager::getHeight(pos + Pos2(16, 16)).landHeight;
    }

    template<typename TArgs>
    static uint32_t run(const TArgs& args, const std::string& what)
    {
        const auto cost = GameCommands::doCommand(args, GameCommands::Flags::apply);
        if (cost == GameCommands::kFailure)
        {
            throw SceneError(what + ": " + lastErrorText());
        }
        return cost;
    }

    // ---------------------------------------------------------------- 片段語法

    struct PieceToken
    {
        uint8_t piece = Pieces::TrackPiece::straight;
        uint8_t gradient = Pieces::TrackGradient::level;
        bool station = false;
        bool signal = false;
        bool follow = false; // F：順著地形的直線（依前方地面高度自動選上坡／下坡／水平）
        std::string label;
        int repeat = 1;
    };

    static PieceToken parsePiece(std::string token)
    {
        PieceToken t;
        if (auto colon = token.find(':'); colon != std::string::npos)
        {
            t.label = token.substr(colon + 1);
            token = token.substr(0, colon);
        }
        if (auto star = token.find('*'); star != std::string::npos)
        {
            t.repeat = std::stoi(token.substr(star + 1));
            token = token.substr(0, star);
        }
        if (token == "SIG")
        {
            t.signal = true;
            return t;
        }
        if (token.ends_with("++"))
        {
            t.gradient = Pieces::TrackGradient::steep_slope_up;
            token.resize(token.size() - 2);
        }
        else if (token.ends_with("--"))
        {
            t.gradient = Pieces::TrackGradient::steep_slope_down;
            token.resize(token.size() - 2);
        }
        else if (token.ends_with("+"))
        {
            t.gradient = Pieces::TrackGradient::slope_up;
            token.resize(token.size() - 1);
        }
        else if (token.ends_with("-"))
        {
            t.gradient = Pieces::TrackGradient::slope_down;
            token.resize(token.size() - 1);
        }
        static const std::map<std::string, uint8_t> kPieces = {
            { "S", Pieces::TrackPiece::straight },
            { "ST", Pieces::TrackPiece::straight },
            { "F", Pieces::TrackPiece::straight },
            { "L1", Pieces::TrackPiece::left_hand_curve_very_small },
            { "L2", Pieces::TrackPiece::left_hand_curve_small },
            { "L3", Pieces::TrackPiece::left_hand_curve },
            { "L4", Pieces::TrackPiece::left_hand_curve_large },
            { "R1", Pieces::TrackPiece::right_hand_curve_very_small },
            { "R2", Pieces::TrackPiece::right_hand_curve_small },
            { "R3", Pieces::TrackPiece::right_hand_curve },
            { "R4", Pieces::TrackPiece::right_hand_curve_large },
            { "SBL", Pieces::TrackPiece::s_bend_left },
            { "SBR", Pieces::TrackPiece::s_bend_right },
        };
        auto it = kPieces.find(token);
        if (it == kPieces.end())
        {
            throw SceneError("unknown piece '" + token + "'");
        }
        t.piece = it->second;
        t.station = token == "ST";
        t.follow = token == "F";
        return t;
    }

    // F 片段：依前方地面決定坡度。遊戲的陡坡片段單格升降 1 級，正好貼合地形斜坡；
    // 緩坡片段跨兩格才升降 1 級，地形做不出這種斜度（需要整地），所以 F 不使用。
    // → (坡度, 佔用格數)
    static std::pair<uint8_t, int> followGradient(const Pos3& pos, uint8_t rotation)
    {
        static constexpr std::array<Pos2, 4> kDir = { Pos2(-32, 0), Pos2(0, 32), Pos2(32, 0), Pos2(0, -32) };
        if (rotation >= 4)
        {
            return { Pieces::TrackGradient::level, 1 };
        }
        const auto d = kDir[rotation];
        // pos 是這一格的原點（角落）；遠端邊緣中點 ≈ 格子中心往行進方向半格
        // 本格近端與遠端邊緣的地面高度（取樣點內縮 1 單位，避免落在交界上讀到相鄰格）
        const Pos2 centre = Pos2(pos.x + 16, pos.y + 16);
        const auto g0 = TileManager::getHeight(centre - Pos2(d.x * 15 / 32, d.y * 15 / 32)).landHeight - pos.z;
        const auto g1 = TileManager::getHeight(centre + Pos2(d.x * 15 / 32, d.y * 15 / 32)).landHeight - pos.z;
        Logging::verbose("build-scene: F at {},{} z{}: ground near {} far {}", pos.x / 32, pos.y / 32, pos.z / kHeightStep, g0 / kHeightStep, g1 / kHeightStep);
        // 只在貼地時跟著地形升降；懸空（例如還在橋上）時維持水平，等地面追上來
        // 取樣點內縮，坡面上量到的高度會差零點幾個單位，所以用半級（8）當容許誤差
        const bool onGround = g0 >= -8;
        if (onGround && g1 >= 8)
        {
            return { Pieces::TrackGradient::steep_slope_up, 1 };
        }
        if (onGround && g0 <= 8 && g1 <= -8)
        {
            return { Pieces::TrackGradient::steep_slope_down, 1 };
        }
        return { Pieces::TrackGradient::level, 1 };
    }

    static Pos3 startPosition(const YAML::Node& op)
    {
        const auto pos2 = tileToWorld(op["start"]);
        const auto z = op["z"] ? op["z"].as<int32_t>() * kHeightStep : landHeight(pos2);
        return Pos3(pos2.x, pos2.y, z);
    }

    // ---------------------------------------------------------------- 軌道

    static void buildTrack(const YAML::Node& op, BuildState& state)
    {
        const auto trackObj = static_cast<uint8_t>(findObject(ObjectType::track, op["object"].as<std::string>()));
        const uint8_t bridge = op["bridge"] ? static_cast<uint8_t>(findObject(ObjectType::bridge, op["bridge"].as<std::string>())) : 0xFF;
        const uint8_t stationObj = op["station"] ? static_cast<uint8_t>(findObject(ObjectType::trainStation, op["station"].as<std::string>())) : 0xFF;
        const uint8_t signalObj = op["signal"] ? static_cast<uint8_t>(findObject(ObjectType::trackSignal, op["signal"].as<std::string>())) : 0xFF;
        // mods 的第 i 位元 = 軌道物件自己的 mods 清單第 i 項（不是 trackExtra 的物件編號）
        uint8_t mods = 0;
        for (const auto& m : op["mods"])
        {
            const auto extra = findObject(ObjectType::trackExtra, m.as<std::string>());
            const auto* track = ObjectManager::get<TrackObject>(trackObj);
            bool found = false;
            for (uint8_t i = 0; i < track->numMods; i++)
            {
                if (track->mods[i] == extra)
                {
                    mods |= 1U << i;
                    found = true;
                }
            }
            if (!found)
            {
                throw SceneError("track mod " + m.as<std::string>() + " is not available for this track");
            }
        }

        auto pos = startPosition(op);
        uint8_t rotation = op["dir"] ? op["dir"].as<uint8_t>() : 0;
        std::optional<GameCommands::TrackPlacementArgs> last;

        std::istringstream path(op["path"].as<std::string>());
        std::string word;
        while (path >> word)
        {
            const auto token = parsePiece(word);
            if (token.signal)
            {
                if (!last || signalObj == 0xFF)
                {
                    throw SceneError("SIG needs a previous piece and `signal` in the track op");
                }
                GameCommands::SignalPlacementArgs sig{};
                sig.pos = last->pos;
                sig.rotation = last->rotation;
                sig.trackId = last->trackId;
                sig.index = 0;
                sig.type = signalObj;
                sig.trackObjType = trackObj;
                sig.sides = 0xC000;
                run(sig, "signal at " + tileText(last->pos));
                continue;
            }
            for (int i = 0; i < token.repeat; i++)
            {
                auto gradient = token.gradient;
                if (token.follow)
                {
                    const auto [g, tiles] = followGradient(pos, rotation);
                    gradient = g;
                    i += tiles - 1; // F*N 以格數計算（緩坡佔兩格）
                }
                const auto pieceId = Pieces::getTrackPieceId(token.piece, gradient, rotation);
                if (!pieceId)
                {
                    throw SceneError("track piece '" + word + "' is not possible in direction " + std::to_string(rotation));
                }
                GameCommands::TrackPlacementArgs args;
                args.pos = pos; // 起點高度（下坡片段的元素會放在起點下方，見 TrackData 的子片段 z）
                args.rotation = pieceId->rotation;
                args.trackId = pieceId->id;
                args.mods = mods;
                args.bridge = bridge;
                args.trackObjectId = trackObj;
                args.unk = false;
                args.unkFlags = 0;
                Logging::verbose("build-scene: track '{}' at {}", word, tileText(pos));
                run(args, "track '" + word + "' at " + tileText(pos));
                last = args;

                if (token.station)
                {
                    if (stationObj == 0xFF)
                    {
                        throw SceneError("ST needs `station` in the track op");
                    }
                    GameCommands::TrainStationPlacementArgs st;
                    st.pos = args.pos;
                    st.rotation = args.rotation;
                    st.trackId = args.trackId;
                    st.index = 0;
                    st.trackObjectId = trackObj;
                    st.type = stationObj;
                    run(st, "station at " + tileText(pos) + " (stations need a town nearby)");
                }
                if (!token.label.empty() && i == token.repeat - 1)
                {
                    state.labels[token.label] = Label{ LabelKind::rail, args.pos, args.rotation, args.trackId };
                }

                const auto& size = TrackData::getUnkTrack((args.trackId << 3) | (args.rotation & 0x3));
                pos = args.pos + size.pos;
                rotation = size.rotationEnd;
            }
        }
        Logging::info("build-scene: track ends at {} dir {}", tileText(pos), rotation);
    }

    // ---------------------------------------------------------------- 道路

    static void buildRoad(const YAML::Node& op, BuildState& state)
    {
        const auto roadObj = static_cast<uint8_t>(findObject(ObjectType::road, op["object"].as<std::string>()));
        const uint8_t bridge = op["bridge"] ? static_cast<uint8_t>(findObject(ObjectType::bridge, op["bridge"].as<std::string>())) : 0xFF;
        const uint8_t stationObj = op["station"] ? static_cast<uint8_t>(findObject(ObjectType::roadStation, op["station"].as<std::string>())) : 0xFF;
        uint8_t mods = 0;
        for (const auto& m : op["mods"])
        {
            mods |= 1U << findObject(ObjectType::roadExtra, m.as<std::string>());
        }

        auto pos = startPosition(op);
        uint8_t rotation = op["dir"] ? op["dir"].as<uint8_t>() : 0;

        std::istringstream path(op["path"].as<std::string>());
        std::string word;
        while (path >> word)
        {
            const auto token = parsePiece(word);
            for (int i = 0; i < token.repeat; i++)
            {
                auto gradient = token.gradient;
                if (token.follow)
                {
                    const auto [g, tiles] = followGradient(pos, rotation);
                    gradient = g;
                    i += tiles - 1; // F*N 以格數計算（緩坡佔兩格）
                }
                const auto pieceId = Pieces::getRoadPieceId(token.piece, gradient, rotation);
                if (!pieceId)
                {
                    throw SceneError("road piece '" + word + "' is not possible in direction " + std::to_string(rotation));
                }
                GameCommands::RoadPlacementArgs args{};
                args.pos = pos;
                args.rotation = pieceId->rotation;
                args.roadId = pieceId->id;
                args.mods = mods;
                args.bridge = bridge;
                args.roadObjectId = roadObj;
                args.unkFlags = 0;
                run(args, "road '" + word + "' at " + tileText(pos));

                if (token.station)
                {
                    if (stationObj == 0xFF)
                    {
                        throw SceneError("ST needs `station` in the road op");
                    }
                    GameCommands::RoadStationPlacementArgs st{};
                    st.pos = args.pos;
                    st.rotation = args.rotation;
                    st.roadId = args.roadId;
                    st.index = 0;
                    st.roadObjectId = roadObj;
                    st.type = stationObj;
                    run(st, "road station at " + tileText(pos) + " (stations need a town nearby)");
                }
                if (!token.label.empty() && i == token.repeat - 1)
                {
                    state.labels[token.label] = Label{ LabelKind::road, args.pos, args.rotation, args.roadId };
                }

                const auto& size = TrackData::getUnkRoad((args.roadId << 3) | (args.rotation & 0x3));
                pos = args.pos + size.pos;
                Logging::verbose("build-scene: road id {} rot {} base z{} piece0.z {} → next {} z{} (size {},{},{})", args.roadId, args.rotation, args.pos.z / kHeightStep, TrackData::getRoadPiece(args.roadId)[0].z, tileText(pos), pos.z, size.pos.x, size.pos.y, size.pos.z);
                rotation = size.rotationEnd;
            }
        }
        Logging::info("build-scene: road ends at {} dir {}", tileText(pos), rotation);
    }

    // ---------------------------------------------------------------- 機場、碼頭

    static void buildAirport(const YAML::Node& op, BuildState& state)
    {
        const auto pos2 = tileToWorld(op["at"]);
        GameCommands::AirportPlacementArgs args{};
        args.pos = Pos3(pos2.x, pos2.y, op["z"] ? op["z"].as<int32_t>() * kHeightStep : landHeight(pos2));
        args.rotation = op["rot"] ? op["rot"].as<uint8_t>() : 0;
        args.type = static_cast<uint8_t>(findObject(ObjectType::airport, op["type"].as<std::string>()));
        run(args, "airport at " + tileText(args.pos) + " (needs a town nearby)");
        if (op["label"])
        {
            state.labels[op["label"].as<std::string>()] = Label{ LabelKind::airport, args.pos, args.rotation, 0 };
        }
    }

    static void buildDock(const YAML::Node& op, BuildState& state)
    {
        const auto pos2 = tileToWorld(op["at"]);
        GameCommands::PortPlacementArgs args{};
        args.pos = Pos3(pos2.x, pos2.y, op["z"] ? op["z"].as<int32_t>() * kHeightStep : landHeight(pos2));
        args.type = static_cast<uint8_t>(findObject(ObjectType::dock, op["type"].as<std::string>()));
        if (op["rot"] && op["rot"].as<std::string>() == "auto")
        {
            // 依序試四個方向，第一個能放的就用（碼頭要面向水）
            bool placed = false;
            for (uint8_t r = 0; r < 4 && !placed; r++)
            {
                args.rotation = r;
                placed = GameCommands::doCommand(args, GameCommands::Flags::apply) != GameCommands::kFailure;
            }
            if (!placed)
            {
                throw SceneError("dock at " + tileText(args.pos) + ": " + lastErrorText());
            }
            Logging::info("build-scene: dock at {} rotation {}", tileText(args.pos), args.rotation);
        }
        else
        {
            args.rotation = op["rot"] ? op["rot"].as<uint8_t>() : 0;
            run(args, "dock at " + tileText(args.pos) + " (needs a town nearby)");
        }
        if (op["label"])
        {
            state.labels[op["label"].as<std::string>()] = Label{ LabelKind::dock, args.pos, args.rotation, 0 };
        }
    }

    // ---------------------------------------------------------------- 車輛

    static const Label& findLabel(const BuildState& state, const std::string& name)
    {
        auto it = state.labels.find(name);
        if (it == state.labels.end())
        {
            throw SceneError("unknown label '" + name + "'");
        }
        return it->second;
    }

    // 標籤所在的車站元素（月台那一格；機場、碼頭找它佔用範圍內的任一格）
    static const StationElement* stationElementAt(const Label& label)
    {
        const auto search = (label.kind == LabelKind::airport || label.kind == LabelKind::dock) ? 3 : 0;
        for (int dx = 0; dx <= search; dx++)
        {
            for (int dy = 0; dy <= search; dy++)
            {
                const auto tile = TileManager::get(Pos2(label.pos.x + dx * kTileSize, label.pos.y + dy * kTileSize));
                for (const auto& el : tile)
                {
                    const auto* station = el.as<StationElement>();
                    if (station == nullptr || station->isGhost())
                    {
                        continue;
                    }
                    if (search == 0 && station->baseHeight() != label.pos.z)
                    {
                        continue;
                    }
                    return station;
                }
            }
        }
        return nullptr;
    }

    static StationId stationAtLabel(const BuildState& state, const std::string& name)
    {
        const auto* station = stationElementAt(findLabel(state, name));
        if (station == nullptr)
        {
            throw SceneError("no station at label '" + name + "'");
        }
        return station->stationId();
    }

    static void placeAircraft(EntityId head, const Label& label, const std::string& at)
    {
        const auto* elStation = stationElementAt(label);
        if (elStation == nullptr)
        {
            throw SceneError("no airport at label '" + at + "'");
        }
        auto* airportObj = ObjectManager::get<AirportObject>(elStation->objectId());
        const auto nodes = airportObj->getMovementNodes();
        for (auto nodeIndex = 0U; nodeIndex < airportObj->numMovementNodes; ++nodeIndex)
        {
            if (!nodes[nodeIndex].hasFlags(AirportMovementNodeFlags::terminal))
            {
                continue;
            }
            GameCommands::VehicleAirPlacementArgs args{};
            args.head = head;
            args.airportNode = static_cast<uint8_t>(nodeIndex);
            args.stationId = elStation->stationId();
            args.convertGhost = false;
            const auto company = GameCommands::getUpdatingCompanyId();
            GameCommands::setUpdatingCompanyId(CompanyId::neutral); // 與 AI 的放置方式相同
            const auto res = GameCommands::doCommand(args, GameCommands::Flags::apply);
            GameCommands::setUpdatingCompanyId(company);
            if (res != GameCommands::kFailure)
            {
                return;
            }
        }
        throw SceneError("no free terminal at airport '" + at + "'");
    }

    static void buildVehicle(const YAML::Node& op, BuildState& state)
    {
        EntityId head = EntityId::null;
        for (const auto& v : op["vehicles"])
        {
            GameCommands::VehicleCreateArgs args{};
            args.vehicleId = head;
            args.vehicleType = findObject(ObjectType::vehicle, v.as<std::string>());
            run(args, "buy " + v.as<std::string>());
            if (head == EntityId::null)
            {
                head = GameCommands::getLegacyReturnState().lastCreatedVehicleId;
            }
        }

        const auto at = op["at"].as<std::string>();
        const auto& label = findLabel(state, at);
        switch (label.kind)
        {
            case LabelKind::rail:
            case LabelKind::road:
            {
                GameCommands::VehiclePlacementArgs place{};
                place.pos = label.pos;
                place.trackAndDirection = (label.pieceId << 3) | (label.rotation & 0x3) | (op["reverse"] && op["reverse"].as<bool>() ? (1U << 2) : 0);
                place.trackProgress = 0;
                place.head = head;
                place.convertGhost = false;
                run(place, "place vehicle at " + at);
                break;
            }
            case LabelKind::airport:
                placeAircraft(head, label, at);
                break;
            case LabelKind::dock:
            {
                const auto* elStation = stationElementAt(label);
                if (elStation == nullptr)
                {
                    throw SceneError("no dock at label '" + at + "'");
                }
                GameCommands::VehicleWaterPlacementArgs place{};
                place.head = head;
                place.pos = Pos3(label.pos.x, label.pos.y, elStation->baseHeight());
                place.convertGhost = false;
                run(place, "place ship at " + at);
                break;
            }
        }

        for (const auto& o : op["orders"])
        {
            auto* vehHead = EntityManager::get<Vehicles::VehicleHead>(head);
            GameCommands::VehicleOrderInsertArgs order{};
            order.head = head;
            order.orderOffset = vehHead->sizeOfOrderTable - 1;
            order.rawOrder = Vehicles::OrderStopAt(stationAtLabel(state, o.as<std::string>())).getRaw();
            run(order, "order " + o.as<std::string>());
        }

        GameCommands::VehicleChangeRunningModeArgs start{};
        start.head = head;
        start.mode = GameCommands::VehicleChangeRunningModeArgs::Mode::startVehicle;
        run(start, "start vehicle");
    }

    // ---------------------------------------------------------------- 城鎮、產業、建築、地景

    static void buildTown(const YAML::Node& op)
    {
        GameCommands::TownPlacementArgs args{};
        args.pos = tileToWorld(op["at"]);
        args.size = op["size"] ? op["size"].as<uint8_t>() : 2;
        run(args, "town");
    }

    static void buildIndustry(const YAML::Node& op)
    {
        GameCommands::IndustryPlacementArgs args{};
        args.pos = tileToWorld(op["at"]);
        args.type = static_cast<uint8_t>(findObject(ObjectType::industry, op["type"].as<std::string>()));
        args.buildImmediately = true;
        args.srand0 = op["seed"] ? op["seed"].as<uint32_t>() : 0x1234;
        args.srand1 = 0x5678;
        run(args, "industry " + op["type"].as<std::string>());
    }

    static void buildBuilding(const YAML::Node& op)
    {
        const auto pos2 = tileToWorld(op["at"]);
        GameCommands::BuildingPlacementArgs args{};
        args.pos = Pos3(pos2.x, pos2.y, landHeight(pos2));
        args.rotation = op["rot"] ? op["rot"].as<uint8_t>() : 0;
        args.type = static_cast<uint8_t>(findObject(ObjectType::building, op["type"].as<std::string>()));
        args.variation = op["variation"] ? op["variation"].as<uint8_t>() : 0;
        args.colour = Colour::black;
        args.buildImmediately = true;
        run(args, "building " + op["type"].as<std::string>());
    }

    static std::pair<Pos2, Pos2> area(const YAML::Node& node)
    {
        if (!node.IsSequence() || node.size() != 4)
        {
            throw SceneError("expected an area [x0, y0, x1, y1]");
        }
        return { Pos2(node[0].as<int32_t>() * kTileSize, node[1].as<int32_t>() * kTileSize),
                 Pos2(node[2].as<int32_t>() * kTileSize, node[3].as<int32_t>() * kTileSize) };
    }

    // 在區域內隨機種樹（失敗的位置略過）
    static void plantTrees(const YAML::Node& op, BuildState& state)
    {
        std::vector<uint8_t> types;
        for (const auto& t : op["types"])
        {
            types.push_back(static_cast<uint8_t>(findObject(ObjectType::tree, t.as<std::string>())));
        }
        const auto [a, b] = area(op["area"]);
        const auto count = op["count"].as<int32_t>();
        int32_t planted = 0;
        for (int32_t i = 0; i < count; i++)
        {
            GameCommands::TreePlacementArgs args{};
            args.pos = Pos2(a.x + static_cast<int32_t>(state.rng.randNext((b.x - a.x) / kTileSize)) * kTileSize, a.y + static_cast<int32_t>(state.rng.randNext((b.y - a.y) / kTileSize)) * kTileSize);
            args.rotation = static_cast<uint8_t>(state.rng.randNext(3));
            args.type = types[state.rng.randNext(static_cast<uint32_t>(types.size() - 1))];
            args.quadrant = static_cast<uint8_t>(state.rng.randNext(3));
            args.colour = Colour::black;
            args.buildImmediately = true;
            args.requiresFullClearance = false;
            if (GameCommands::doCommand(args, GameCommands::Flags::apply) != GameCommands::kFailure)
            {
                planted++;
            }
        }
        Logging::info("build-scene: planted {} of {} trees", planted, count);
    }

    static void clearArea(const YAML::Node& op)
    {
        const auto [a, b] = area(op["area"]);
        GameCommands::ClearLandArgs args{};
        args.pointA = a;
        args.pointB = b;
        args.centre = Pos2((a.x + b.x) / 2, (a.y + b.y) / 2);
        GameCommands::doCommand(args, GameCommands::Flags::apply);
    }

    // 整塊抬高／壓低（每次一級）；區塊外的格子不動，邊緣形成崖面（隧道口需要崖面）
    static void adjustLand(const YAML::Node& op, bool raise)
    {
        const auto [a, b] = area(op["area"]);
        const auto steps = op["steps"] ? op["steps"].as<int32_t>() : 1;
        for (int32_t i = 0; i < steps; i++)
        {
            if (raise)
            {
                GameCommands::RaiseLandArgs args{};
                args.pointA = a;
                args.pointB = b;
                args.centre = Pos2((a.x + b.x) / 2, (a.y + b.y) / 2);
                args.corner = MapSelectionType::full;
                run(args, "raise land");
            }
            else
            {
                GameCommands::LowerLandArgs args{};
                args.pointA = a;
                args.pointB = b;
                args.centre = Pos2((a.x + b.x) / 2, (a.y + b.y) / 2);
                args.corner = MapSelectionType::full;
                run(args, "lower land");
            }
        }
    }

    static void changeLand(const YAML::Node& op)
    {
        const auto [a, b] = area(op["area"]);
        GameCommands::ChangeLandMaterialArgs args{};
        args.pointA = a;
        args.pointB = b;
        args.landType = static_cast<uint8_t>(findObject(ObjectType::land, op["type"].as<std::string>()));
        run(args, "land " + op["type"].as<std::string>());
    }

    // 除錯：把區域內每格的內容印成文字地圖（~ 水、R 道路、T 軌道、S 車站、B 建築、I 產業、t 樹、. 空地；數字 = 地面高度級的個位數）
    static void probe(const YAML::Node& op)
    {
        const auto [a, b] = area(op["area"]);
        Logging::info("build-scene: probe {},{} – {},{}（列 = x，欄 = y）", a.x / 32, a.y / 32, b.x / 32, b.y / 32);
        for (auto x = a.x; x <= b.x; x += kTileSize)
        {
            std::string line;
            for (auto y = a.y; y <= b.y; y += kTileSize)
            {
                char c = '.';
                const auto tile = TileManager::get(Pos2(x, y));
                for (const auto& el : tile)
                {
                    if (el.type() == ElementType::surface)
                    {
                        const auto* surface = el.as<SurfaceElement>();
                        if (surface->water() > 0)
                        {
                            c = '~';
                        }
                        else
                        {
                            c = static_cast<char>('0' + (surface->baseZ() / 4) % 10);
                        }
                    }
                    else if (el.type() == ElementType::road && c != 'S')
                    {
                        c = 'R';
                    }
                    else if (el.type() == ElementType::track && c != 'S')
                    {
                        c = c == 'R' ? 'X' : 'T';
                    }
                    else if (el.type() == ElementType::station)
                    {
                        c = 'S';
                    }
                    else if (el.type() == ElementType::building)
                    {
                        c = 'B';
                    }
                    else if (el.type() == ElementType::industry)
                    {
                        c = 'I';
                    }
                    else if (el.type() == ElementType::tree && c >= '0' && c <= '9')
                    {
                        c = 't';
                    }
                }
                line += c;
            }
            Logging::info("{:4d} {}", x / 32, line);
        }
    }

    // 顏色名稱（Graphics/Colour.h 的 enum 名稱）或數值
    static Colour parseColour(const YAML::Node& node)
    {
        static constexpr std::array<std::string_view, enumValue(Colour::max)> kNames = {
            "black", "grey", "white", "mutedDarkPurple", "mutedPurple", "purple", "darkBlue", "blue", "mutedDarkTeal", "mutedTeal", "darkGreen", "mutedSeaGreen", "mutedGrassGreen", "green", "mutedAvocadoGreen", "mutedOliveGreen", "yellow", "darkYellow", "orange", "amber", "darkOrange", "mutedDarkYellow", "mutedYellow", "brown", "mutedOrange", "mutedDarkRed", "darkRed", "red", "darkPink", "pink", "mutedRed"
        };
        const auto text = node.as<std::string>();
        for (size_t i = 0; i < kNames.size(); i++)
        {
            if (kNames[i] == text)
            {
                return static_cast<Colour>(i);
            }
        }
        const auto value = node.as<int>();
        if (value < 0 || value >= enumValue(Colour::max))
        {
            throw SceneError("unknown colour " + text);
        }
        return static_cast<Colour>(value);
    }

    static void removeAllIndustries()
    {
        std::vector<IndustryId> ids;
        for (const auto& industry : IndustryManager::industries())
        {
            ids.push_back(industry.id());
        }
        for (auto id : ids)
        {
            GameCommands::IndustryRemovalArgs args{};
            args.industryId = id;
            GameCommands::doCommand(args, GameCommands::Flags::apply);
        }
        Logging::info("build-scene: removed {} generated industries", ids.size());
    }

    static void simulate(int32_t ticks)
    {
        for (int32_t i = 0; i < ticks && !SceneManager::isSceneTransitionPending(); i++)
        {
            Scenes::GameScene::tick();
        }
        // 模擬時遊戲會輪流把「執行指令的公司」切給各公司；接下來的建造指令要以玩家公司執行
        GameCommands::setUpdatingCompanyId(CompanyManager::getControllingId());
    }

    // ---------------------------------------------------------------- 地形

    static void generateLandscape(const YAML::Node& scene, const BuildState& state)
    {
        EditorController::setDefaultLandscapeOptions();
        auto& options = Scenario::getOptions();
        options.scenarioStartYear = scene["year"] ? scene["year"].as<uint16_t>() : 1950;
        options.numberOfTowns = 0;
        options.numberOfIndustries = 0;
        options.numRiverbeds = 0;
        options.maxCompetingCompanies = 0;
        options.competitorStartDelay = 0;
        StringManager::formatString(options.scenarioName, StringIds::unnamed);

        if (scene["heightmap"])
        {
            options.generator = Scenario::LandGeneratorType::PngHeightMap;
            MapGenerator::setPngHeightmapPath(state.baseDir / scene["heightmap"].as<std::string>());
        }
        if (const auto trees = scene["trees"])
        {
            options.numberOfForests = trees["forests"].as<uint16_t>(options.numberOfForests);
            options.numberRandomTrees = trees["random"].as<uint16_t>(options.numberRandomTrees);
            if (trees["radius"])
            {
                options.minForestRadius = trees["radius"][0].as<uint8_t>();
                options.maxForestRadius = trees["radius"][1].as<uint8_t>();
            }
            if (trees["density"])
            {
                options.minForestDensity = trees["density"][0].as<uint8_t>();
                options.maxForestDensity = trees["density"][1].as<uint8_t>();
            }
            if (trees["altitude"])
            {
                options.minAltitudeForTrees = trees["altitude"][0].as<uint8_t>();
                options.maxAltitudeForTrees = trees["altitude"][1].as<uint8_t>();
            }
        }
        Scenario::reset();

        auto& gameState = getGameState();
        if (scene["sea_level"])
        {
            gameState.seaLevel = scene["sea_level"].as<uint8_t>();
        }
        const auto seed = scene["seed"] ? scene["seed"].as<uint32_t>() : 1;
        gameState.rng = Core::Prng(0x12345678 ^ seed, 0x9ABCDEF0 + seed);
        gameState.flags &= ~GameStateFlags::tileManagerLoaded;
        Scenario::start();
        SceneManager::applySceneTransition();
        if (SceneManager::getCurrentScene() != SceneManager::SceneId::gameplay)
        {
            throw SceneError("new game did not reach the gameplay scene");
        }
    }

    // ---------------------------------------------------------------- 主流程

    static void runOp(const YAML::Node& op, BuildState& state)
    {
        if (op["track"])
        {
            buildTrack(op["track"], state);
        }
        else if (op["road"])
        {
            buildRoad(op["road"], state);
        }
        else if (op["vehicle"])
        {
            buildVehicle(op["vehicle"], state);
        }
        else if (op["train"])
        {
            buildVehicle(op["train"], state);
        }
        else if (op["airport"])
        {
            buildAirport(op["airport"], state);
        }
        else if (op["dock"])
        {
            buildDock(op["dock"], state);
        }
        else if (op["town"])
        {
            buildTown(op["town"]);
        }
        else if (op["industry"])
        {
            buildIndustry(op["industry"]);
        }
        else if (op["building"])
        {
            buildBuilding(op["building"]);
        }
        else if (op["trees"])
        {
            plantTrees(op["trees"], state);
        }
        else if (op["clear"])
        {
            clearArea(op["clear"]);
        }
        else if (op["land"])
        {
            changeLand(op["land"]);
        }
        else if (op["raise"])
        {
            adjustLand(op["raise"], true);
        }
        else if (op["lower"])
        {
            adjustLand(op["lower"], false);
        }
        else if (op["probe"])
        {
            probe(op["probe"]);
        }
        else if (op["simulate"])
        {
            simulate(op["simulate"].as<int32_t>());
        }
        else
        {
            throw SceneError("unknown op");
        }
    }

    bool build(const fs::path& scenePath, const fs::path& outputPath)
    {
        BuildState state;
        state.baseDir = scenePath.parent_path();
        auto& config = Config::get();
        const auto previousLocked = config.buildLockedVehicles;
        try
        {
            const auto scene = YAML::LoadFile(scenePath.string());

            std::vector<std::string> objects;
            for (const auto& o : scene["objects"])
            {
                objects.push_back(o.as<std::string>());
            }
            if (!Title::loadStandaloneWorldObjects(objects))
            {
                Logging::warn("build-scene: object selection is not valid");
            }

            generateLandscape(scene, state);
            if (scene["remove_generated_industries"] && scene["remove_generated_industries"].as<bool>())
            {
                removeAllIndustries();
            }
            if (scene["freeze_industries"] && scene["freeze_industries"].as<bool>())
            {
                // 不再自動新設或關閉產業：場景裡只有腳本放的產業
                IndustryManager::setFlags(IndustryManager::Flags::disallowIndustriesCloseDown | IndustryManager::Flags::disallowIndustriesStartUp);
            }

            GameCommands::setUpdatingCompanyId(CompanyManager::getControllingId());
            auto* company = CompanyManager::getPlayerCompany();
            company->cash = currency48_t(2'000'000'000);
            if (const auto colours = scene["company_colours"])
            {
                // 公司色固定下來（否則每次重建隨機選色）：車輛、吊橋、站牌都用公司色
                company->mainColours.primary = parseColour(colours[0]);
                company->mainColours.secondary = parseColour(colours[1]);
                for (auto& vehicleColours : company->vehicleColours)
                {
                    vehicleColours = company->mainColours;
                }
            }
            config.buildLockedVehicles = true; // 場景可以混用不同年代的車輛

            for (const auto& op : scene["ops"])
            {
                state.opIndex++;
                // 砍樹、整地會讓城鎮對公司的評價下降，之後的建造會被「地方政府拒絕」；場景建造不受評價限制
                for (auto& town : TownManager::towns())
                {
                    town.companyRatings[enumValue(CompanyManager::getControllingId())] = kMaxCompanyRating;
                }
                try
                {
                    runOp(op, state);
                }
                catch (const std::exception& e)
                {
                    YAML::Emitter out;
                    out << YAML::Flow << op;
                    throw SceneError("op " + std::to_string(state.opIndex) + " " + out.c_str() + ": " + e.what());
                }
            }

            if (const auto date = scene["date"])
            {
                // 最後才設定日期（之前的模擬會讓日期往前走），並重新計算季節與雪線；
                // 之後的收尾模擬讓樹木逐步換到這個季節的樣子（每次更新只前進一季）
                Scenario::initialiseDate(date[0].as<uint16_t>(), static_cast<MonthId>(date[1].as<int>() - 1), date[2].as<uint8_t>());
                Scenario::initialiseSnowLine();
            }
            simulate(scene["simulate"] ? scene["simulate"].as<int32_t>() : 0);

            config.buildLockedVehicles = previousLocked;
            const auto saveFlags = scene["title_sequence"] && scene["title_sequence"].as<bool>() ? S5::SaveFlags::titleSequence : S5::SaveFlags::none;
            if (!S5::exportGameStateToFile(outputPath, saveFlags))
            {
                throw SceneError("unable to save " + outputPath.u8string());
            }
            Logging::info("build-scene: {} ops, saved to {}", state.opIndex, outputPath.u8string());
            return true;
        }
        catch (const std::exception& e)
        {
            config.buildLockedVehicles = previousLocked;
            Logging::error("build-scene failed: {}", e.what());
            if (Game::hasFlags(GameStateFlags::tileManagerLoaded))
            {
                auto failedPath = outputPath;
                failedPath.replace_extension(".failed.SV5");
                S5::exportGameStateToFile(failedPath, S5::SaveFlags::none);
                Logging::info("build-scene: partial result saved to {}", failedPath.u8string());
            }
            return false;
        }
    }
}

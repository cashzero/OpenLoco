#include "Objects/ObjectAliases.h"
#include "Environment.h"
#include <OpenLoco/Diagnostics/Logging.h>
#include <OpenLoco/Platform/Platform.h>
#include <OpenLoco/Utility/String.hpp>
#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <yaml-cpp/exceptions.h>
#include <yaml-cpp/yaml.h>

using namespace OpenLoco::Diagnostics;

namespace OpenLoco::ObjectAliases
{
    static constexpr auto kAliasFileName = "aliases.yml";

    static bool _loaded = false;
    static std::map<std::pair<uint8_t, std::string>, std::string> _aliases;

    static void loadFile(const fs::path& path)
    {
        if (!fs::exists(path))
        {
            return;
        }
        try
        {
            std::ifstream stream(path, std::ios::in | std::ios::binary);
            const auto root = YAML::Load(stream);
            const auto& list = root["aliases"];
            if (!list || !list.IsSequence())
            {
                Logging::warn("Object alias file {} has no 'aliases' list", path.u8string());
                return;
            }
            size_t count = 0;
            for (const auto& item : list)
            {
                // 每列：[ObjectType 數值, vanilla 名稱, OpenGraphics 名稱]
                if (!item.IsSequence() || item.size() != 3)
                {
                    continue;
                }
                const auto type = static_cast<uint8_t>(item[0].as<int>());
                auto vanilla = std::string(Utility::trim(item[1].as<std::string>()));
                auto og = std::string(Utility::trim(item[2].as<std::string>()));
                if (type >= kMaxObjectTypes || vanilla.empty() || og.empty() || og.size() > 8)
                {
                    continue;
                }
                _aliases[{ type, std::move(vanilla) }] = std::move(og);
                count++;
            }
            Logging::info("Loaded {} object aliases from {}", count, path.u8string());
        }
        catch (const YAML::Exception& e)
        {
            Logging::error("Unable to read object alias file {}: {}", path.u8string(), e.what());
        }
    }

    void reload()
    {
        _aliases.clear();
        loadFile(Environment::getPathNoWarning(Environment::PathId::objects) / kAliasFileName);
        loadFile(Environment::getPathNoWarning(Environment::PathId::customObjects) / kAliasFileName);
        _loaded = true;
    }

    std::optional<ObjectHeader> find(const ObjectHeader& header)
    {
        if (!_loaded)
        {
            reload();
        }
        if (_aliases.empty() || header.isEmpty())
        {
            return std::nullopt;
        }
        const auto type = static_cast<uint8_t>(header.getType());
        auto it = _aliases.find({ type, std::string(Utility::trim(header.getName())) });
        if (it == _aliases.end())
        {
            return std::nullopt;
        }

        ObjectHeader alias{};
        alias.flags = type | (static_cast<uint32_t>(SourceGame::openLoco) << 6);
        std::fill(std::begin(alias.name), std::end(alias.name), ' ');
        std::copy(it->second.begin(), it->second.end(), std::begin(alias.name));
        alias.checksum = 0;
        return alias;
    }

    bool isPreferred()
    {
        static const bool preferred = Platform::getEnvironmentVariable("OPENLOCO_PREFER_OPENGRAPHICS") == "1";
        return preferred;
    }
}

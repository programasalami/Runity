#include "runity/content/content_db.hpp"

#include "runity/content/conditions.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include <zlib.h>

namespace runity::content {

namespace {

using Error = std::unexpected<std::string>;

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

/// "0x030e", "782" or "-1".
std::optional<long long> parse_int(std::string_view s) {
    s = trim(s);
    bool negative = false;
    if (!s.empty() && s.front() == '-') {
        negative = true;
        s.remove_prefix(1);
    }
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s.remove_prefix(2);
    }
    long long v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v, base);
    if (s.empty() || r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
    return negative ? -v : v;
}

std::optional<float> parse_float(std::string_view s) {
    s = trim(s);
    float v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (s.empty() || r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

std::expected<std::vector<int>, std::string> parse_int_list(std::string_view s) {
    std::vector<int> out;
    while (!trim(s).empty()) {
        const auto comma = s.find(',');
        const auto part = s.substr(0, comma);
        const auto v = parse_int(part);
        if (!v) return Error("bad number in list: '" + std::string(trim(part)) + "'");
        out.push_back(static_cast<int>(*v));
        if (comma == std::string_view::npos) break;
        s.remove_prefix(comma + 1);
    }
    return out;
}

std::string_view strip_bom(std::string_view s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.remove_prefix(3);
    }
    return s;
}

std::expected<std::vector<std::uint8_t>, std::string> base64_decode(std::string_view in) {
    static constexpr auto table = [] {
        std::array<int, 256> t{};
        t.fill(-1);
        const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) t[static_cast<unsigned char>(chars[i])] = i;
        return t;
    }();
    std::vector<std::uint8_t> out;
    out.reserve(in.size() * 3 / 4);
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        if (c == '\r' || c == '\n') continue;
        const int v = table[static_cast<unsigned char>(c)];
        if (v < 0) return Error("invalid base64 character");
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

std::expected<std::vector<std::uint8_t>, std::string> zlib_inflate(const std::vector<std::uint8_t>& in, std::size_t expected_size) {
    std::vector<std::uint8_t> out(expected_size);
    uLongf out_len = static_cast<uLongf>(expected_size);
    const int rc = uncompress(out.data(), &out_len, in.data(), static_cast<uLong>(in.size()));
    if (rc != Z_OK) return Error("zlib data is corrupt or larger than width x height (code " + std::to_string(rc) + ")");
    if (out_len != expected_size) return Error("tile data has " + std::to_string(out_len) + " bytes, expected " + std::to_string(expected_size));
    return out;
}

std::expected<std::string, std::string> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return Error("cannot open " + path.string());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

constexpr std::array<std::pair<const char*, Stat>, kStatCount> kStatElements = {{
    {"MaxHitPoints", Stat::MaxHp},
    {"MaxMagicPoints", Stat::MaxMp},
    {"Attack", Stat::Attack},
    {"Defense", Stat::Defense},
    {"Speed", Stat::Speed},
    {"Dexterity", Stat::Dexterity},
    {"HpRegen", Stat::HpRegen},
    {"MpRegen", Stat::MpRegen},
}};

/// One <Projectile> element (items and monsters). Returns nullopt if it cannot fly.
std::optional<ProjectileDesc> parse_projectile(const pugi::xml_node& pn) {
    ProjectileDesc pd;
    pd.object_id = std::string(trim(pn.child("ObjectId").text().as_string()));
    pd.speed = parse_float(pn.child("Speed").text().as_string()).value_or(0.0f) / 10.0f;
    pd.lifetime_ms = static_cast<int>(parse_float(pn.child("LifetimeMS").text().as_string()).value_or(0.0f));
    pd.min_damage = static_cast<int>(parse_int(pn.child("MinDamage").text().as_string()).value_or(0));
    pd.max_damage = static_cast<int>(parse_int(pn.child("MaxDamage").text().as_string()).value_or(pd.min_damage));
    if (auto d = pn.child("Damage")) pd.min_damage = pd.max_damage = static_cast<int>(parse_int(d.text().as_string()).value_or(0));
    pd.multi_hit = static_cast<bool>(pn.child("MultiHit"));
    pd.passes_cover = static_cast<bool>(pn.child("PassesCover"));
    pd.armor_piercing = static_cast<bool>(pn.child("ArmorPiercing"));
    pd.amplitude = parse_float(pn.child("Amplitude").text().as_string()).value_or(0.0f);
    pd.frequency = parse_float(pn.child("Frequency").text().as_string()).value_or(1.0f);
    if (auto sz = pn.child("Size")) pd.size = static_cast<int>(parse_int(sz.text().as_string()).value_or(100));
    for (const auto& eff : pn.children("ConditionEffect")) {
        if (const auto c = condition_from_string(trim(eff.text().as_string()))) {
            pd.effects.push_back({static_cast<int>(*c), static_cast<int>(eff.attribute("duration").as_float() * 1000.0f)});
        }
    }
    // The original ProjectilePathSegment.ParsePath(ProjectileDesc): Amplitude / Frequency, then Wavy, then Boomerang, else Line.
    pd.path = (pn.child("Amplitude") || pn.child("Frequency")) ? PathKind::Amplitude
              : pn.child("Wavy")                               ? PathKind::Wavy
              : pn.child("Boomerang")                          ? PathKind::Boomerang
                                                               : PathKind::Line;
    if (pd.object_id.empty() || pd.speed <= 0.0f || pd.lifetime_ms <= 0 || pd.max_damage < pd.min_damage) return std::nullopt;
    return pd;
}

}  // namespace

std::optional<Stat> stat_from_reference_id(int id) noexcept {
    switch (id) {
        case 0: return Stat::MaxHp;
        case 3: return Stat::MaxMp;
        case 20: return Stat::Attack;
        case 21: return Stat::Defense;
        case 22: return Stat::Speed;
        case 26: return Stat::HpRegen;  // Vitality
        case 27: return Stat::MpRegen;  // Wisdom
        case 28: return Stat::Dexterity;
        default: return std::nullopt;
    }
}

std::uint16_t ContentDb::loot_bag_type(int bag_type) const noexcept {
    static constexpr std::array<std::uint16_t, 6> kBags = {0x0500, 0x0506, 0x0508, 0x0509, 0x0510, 0x0507};
    return bag_type >= 0 && bag_type < static_cast<int>(kBags.size()) ? kBags[static_cast<std::size_t>(bag_type)] : kBags[0];
}

std::string_view to_string(Stat s) noexcept {
    for (const auto& [name, stat] : kStatElements) {
        if (stat == s) return name;
    }
    return "?";
}

std::expected<void, std::string> ContentDb::add_xml(std::string_view xml_text, std::string_view source_name) {
    pugi::xml_document doc;
    const auto parsed = doc.load_buffer(xml_text.data(), xml_text.size());
    const std::string src(source_name);
    if (!parsed) return Error(src + ": " + parsed.description());

    for (const auto& node : doc.document_element().children()) {
        const std::string_view tag = node.name();
        if (tag != "Object" && tag != "Ground") continue;
        const std::string id = node.attribute("id").as_string();
        const auto type = parse_int(node.attribute("type").as_string());
        if (!type || *type < 0 || *type > 0xFFFF) return Error(src + ": bad type on '" + id + "'");
        if (id.empty()) return Error(src + ": a definition has no id");
        const auto type16 = static_cast<std::uint16_t>(*type);

        if (tag == "Object" && type16 == 0) {
            warnings_.push_back(src + ": object type 0 is reserved for 'no object' ('" + id + "'), skipped");
            continue;
        }
        if (tag == "Ground") {
            if (grounds_.contains(type16) || ground_by_id_.contains(id)) {
                warnings_.push_back(src + ": duplicate ground " + std::to_string(type16) + " '" + id + "' skipped (first wins)");
                continue;
            }
            GroundDesc g;
            g.type = type16;
            g.id = id;
            g.no_walk = static_cast<bool>(node.child("NoWalk"));
            g.sinking = static_cast<bool>(node.child("Sinking"));
            if (auto s = node.child("Speed")) g.speed = parse_float(s.text().as_string()).value_or(1.0f);
            if (auto d = node.child("Damage")) g.damage = static_cast<int>(parse_int(d.text().as_string()).value_or(0));
            ground_by_id_[id] = type16;
            grounds_.emplace(type16, std::move(g));
            continue;
        }

        if (objects_.contains(type16) || object_by_id_.contains(id)) {
            warnings_.push_back(src + ": duplicate object " + std::to_string(type16) + " '" + id + "' skipped (first wins)");
            continue;
        }
        ObjectDesc o;
        o.type = type16;
        o.id = id;
        o.object_class = trim(node.child("Class").text().as_string());
        o.is_static = static_cast<bool>(node.child("Static"));
        o.occupy_square = static_cast<bool>(node.child("OccupySquare"));
        o.full_occupy = static_cast<bool>(node.child("FullOccupy"));
        o.enemy_occupy_square = static_cast<bool>(node.child("EnemyOccupySquare"));
        o.blocks_sight = static_cast<bool>(node.child("BlocksSight")) || o.object_class == "Wall" || o.object_class == "CaveWall" ||
                         o.object_class == "ConnectedWall";
        o.enemy = static_cast<bool>(node.child("Enemy"));
        o.player = static_cast<bool>(node.child("Player"));
        o.item = static_cast<bool>(node.child("Item"));
        if (auto n = node.child("MaxHitPoints")) o.max_hp = static_cast<int>(parse_int(n.text().as_string()).value_or(0));
        if (auto n = node.child("Defense")) o.defense = static_cast<int>(parse_int(n.text().as_string()).value_or(0));
        if (auto n = node.child("Size")) o.size = static_cast<int>(parse_int(n.text().as_string()).value_or(100));
        if (auto n = node.child("XpMult")) o.xp_mult = parse_float(n.text().as_string()).value_or(1.0f);
        if (auto n = node.child("Level")) o.level = static_cast<int>(parse_int(n.text().as_string()).value_or(0));
        o.group = trim(node.child("Group").text().as_string());
        if (auto n = node.child("Terrain")) {
            const auto t = terrain_from_string(trim(n.text().as_string()));
            if (!t) warnings_.push_back(src + ": '" + id + "' has unknown terrain '" + n.text().as_string() + "'");
            o.terrain = t.value_or(Terrain::None);
        }
        if (auto n = node.child("SpawnProb")) o.spawn_prob = parse_float(n.text().as_string()).value_or(1.0f);
        if (auto n = node.child("Spawn")) {
            auto value = [&](const char* name, int fallback) {
                return static_cast<int>(parse_int(n.child(name).text().as_string()).value_or(fallback));
            };
            o.spawn = SpawnGroup{value("Mean", 1), value("StdDev", 0), value("Min", 1), value("Max", 1)};
        }
        if (!o.item) {
            for (const auto& pn : node.children("Projectile")) {
                const int index = pn.attribute("id").as_int(0);
                if (auto pd = parse_projectile(pn)) {
                    o.projectiles.emplace(index, std::move(*pd));
                } else {
                    warnings_.push_back(src + ": '" + id + "' has an unusable <Projectile id=" + std::to_string(index) + ">");
                }
            }
        }

        if (o.player) {
            PlayerClassDesc pc;
            pc.type = type16;
            pc.id = id;
            for (const auto& [element, stat] : kStatElements) {
                const auto n = node.child(element);
                if (!n) return Error(src + ": class '" + id + "' has no <" + element + ">");
                const auto start = parse_int(n.text().as_string());
                const auto max = parse_int(n.attribute("max").as_string());
                if (!start || !max) return Error(src + ": class '" + id + "' has a bad <" + element + ">");
                pc.stats[static_cast<std::size_t>(stat)] = {static_cast<int>(*start), static_cast<int>(*max)};
            }
            for (const auto& inc : node.children("LevelIncrease")) {
                const std::string_view stat_name = trim(inc.text().as_string());
                auto it = std::find_if(kStatElements.begin(), kStatElements.end(), [&](const auto& e) { return stat_name == e.first; });
                if (it == kStatElements.end()) {
                    warnings_.push_back(src + ": class '" + id + "' raises unknown stat '" + std::string(stat_name) + "'");
                    continue;
                }
                pc.level_increase[static_cast<std::size_t>(it->second)] = {inc.attribute("min").as_int(), inc.attribute("max").as_int()};
            }
            auto slots = parse_int_list(node.child("SlotTypes").text().as_string());
            auto equipment = parse_int_list(node.child("Equipment").text().as_string());
            if (!slots || !equipment) return Error(src + ": class '" + id + "': " + (!slots ? slots.error() : equipment.error()));
            pc.slot_types = std::move(*slots);
            pc.equipment = std::move(*equipment);
            classes_.emplace(type16, std::move(pc));
        }

        if (o.item) {
            ItemDesc item;
            item.type = type16;
            item.id = id;
            if (auto n = node.child("SlotType")) item.slot_type = static_cast<int>(parse_int(n.text().as_string()).value_or(0));
            if (auto n = node.child("Tier")) item.tier = static_cast<int>(parse_int(n.text().as_string()).value_or(-1));
            if (auto n = node.child("RateOfFire")) item.rate_of_fire = parse_float(n.text().as_string()).value_or(1.0f);
            if (auto n = node.child("NumProjectiles")) item.num_projectiles = static_cast<int>(parse_int(n.text().as_string()).value_or(1));
            if (auto n = node.child("ArcGap")) item.arc_gap_degrees = parse_float(n.text().as_string()).value_or(11.25f);
            if (auto n = node.child("BagType")) item.bag_type = static_cast<int>(parse_int(n.text().as_string()).value_or(0));
            item.consumable = static_cast<bool>(node.child("Consumable"));
            item.soulbound = static_cast<bool>(node.child("Soulbound"));
            for (const auto& boost : node.children("ActivateOnEquip")) {
                if (std::string_view(trim(boost.text().as_string())) != "IncrementStat") continue;
                if (const auto stat = stat_from_reference_id(boost.attribute("stat").as_int(-1))) {
                    item.stat_bonuses[static_cast<std::size_t>(*stat)] += boost.attribute("amount").as_int();
                }
            }
            for (const auto& act : node.children("Activate")) {
                const std::string_view what = trim(act.text().as_string());
                if (what == "Heal") item.heal_amount = act.attribute("amount").as_int();
                if (what == "Magic") item.magic_amount = act.attribute("amount").as_int();
                if (what == "IncrementStat") {
                    if (const auto stat = stat_from_reference_id(act.attribute("stat").as_int(-1))) {
                        item.stat_increments[static_cast<std::size_t>(*stat)] += act.attribute("amount").as_int();
                    }
                }
            }
            if (auto pn = node.child("Projectile")) {
                item.projectile = parse_projectile(pn);
                if (!item.projectile) warnings_.push_back(src + ": item '" + id + "' has an unusable <Projectile>, it will not fire");
            }
            item_by_id_[id] = type16;
            items_.emplace(type16, std::move(item));
        }
        object_by_id_[id] = type16;
        objects_.emplace(type16, std::move(o));
    }
    return {};
}

std::expected<void, std::string> ContentDb::add_map(std::string_view name, std::string_view jm_text) {
    const std::string src(name);
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(strip_bom(jm_text));
    } catch (const nlohmann::json::exception& e) {
        return Error(src + ": " + e.what());
    }
    MapData map;
    map.name = src;
    try {
        const auto width = j.at("width").get<int>();
        const auto height = j.at("height").get<int>();
        if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return Error(src + ": bad map size");
        map.width = static_cast<std::uint16_t>(width);
        map.height = static_cast<std::uint16_t>(height);
        for (const auto& entry : j.at("dict")) {
            TileTemplate t;
            // Like the original (MapData.LoadJMap): unknown names become ground 0 / no object; only the first object is used.
            if (auto g = entry.find("ground"); g != entry.end() && !g->is_null()) {
                const auto* ground_desc = ground(g->get<std::string>());
                if (ground_desc == nullptr) warnings_.push_back(src + ": unknown ground '" + g->get<std::string>() + "'");
                t.ground_type = ground_desc != nullptr ? ground_desc->type : 0;
            }
            if (auto objs = entry.find("objs"); objs != entry.end() && !objs->is_null() && !objs->empty()) {
                const auto& obj = (*objs)[0];
                const auto object_id = obj.at("id").get<std::string>();
                const auto* object_desc = object(object_id);
                if (object_desc == nullptr) {
                    warnings_.push_back(src + ": unknown object '" + object_id + "'");
                } else {
                    t.object_type = object_desc->type;
                    t.object_name = obj.value("name", "");
                }
            }
            if (auto regions = entry.find("regions"); regions != entry.end() && !regions->is_null()) {
                for (const auto& r : *regions) t.regions.push_back(r.at("id").get<std::string>());
            }
            map.palette.push_back(std::move(t));
        }
        auto compressed = base64_decode(j.at("data").get<std::string>());
        if (!compressed) return Error(src + ": " + compressed.error());
        const std::size_t count = std::size_t{map.width} * map.height;
        auto raw = zlib_inflate(*compressed, count * 2);
        if (!raw) return Error(src + ": " + raw.error());
        map.tiles.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            // Big-endian int16 per tile (the reference's NetworkReader, WorldSystem.md).
            const auto index = static_cast<std::uint16_t>(((*raw)[i * 2] << 8) | (*raw)[i * 2 + 1]);
            if (index >= map.palette.size()) return Error(src + ": tile index out of the palette");
            map.tiles[i] = index;
        }
    } catch (const nlohmann::json::exception& e) {
        return Error(src + ": " + e.what());
    }
    maps_.emplace(src, std::move(map));
    return {};
}

namespace {

/// The original's TileRegion byte values (Common/Resources/World/MapData.cs), as the names the .jm maps use.
std::string_view region_name(std::uint8_t v) {
    switch (v) {
        case 0x01: return "Spawn";
        case 0x02: return "Realm Portals";
        case 0x09: return "Vault";
        case 0x0a: return "Loot";
        case 0x0b: return "Defender";
        case 0x0c: return "Hallway";
        case 0x0d: return "Enemy";
        case 0x0e: return "Hallway 1";
        case 0x0f: return "Hallway 2";
        case 0x10: return "Hallway 3";
        case 0x14: return "Gifting Chest";
        case 0x25: return "PetRegion";
        case 0x26: return "Outside Arena";
        case 0x27: return "Item Spawn Point";
        case 0x28: return "Arena Central Spawn";
        case 0x29: return "Arena Edge Spawn";
        case 0x3a: return "Quest Monster Region 2";
        default: return {};
    }
}

constexpr std::array<std::string_view, static_cast<std::size_t>(Terrain::Count)> kTerrainNames = {
    "None", "Mountains", "HighSand", "HighPlains", "HighForest", "MidSand", "MidPlains", "MidForest", "LowSand", "LowPlains",
    "LowForest", "ShoreSand", "ShorePlains", "BeachTowels"};

}  // namespace

std::string_view to_string(Terrain t) noexcept {
    const auto i = static_cast<std::size_t>(t);
    return i < kTerrainNames.size() ? kTerrainNames[i] : "?";
}

std::optional<Terrain> terrain_from_string(std::string_view name) noexcept {
    for (std::size_t i = 0; i < kTerrainNames.size(); ++i) {
        if (kTerrainNames[i] == name) return static_cast<Terrain>(i);
    }
    return std::nullopt;
}

std::expected<void, std::string> ContentDb::add_wmap(std::string_view name, std::span<const std::uint8_t> bytes) {
    const std::string src(name);
    if (bytes.size() < 2) return Error(src + ": empty file");
    const int version = bytes[0];
    if (version > 2) return Error(src + ": unsupported .wmap version " + std::to_string(version));

    // Inflate with a growing buffer (the size is not stored).
    std::vector<std::uint8_t> data;
    {
        z_stream zs{};
        if (inflateInit(&zs) != Z_OK) return Error(src + ": zlib init failed");
        zs.next_in = const_cast<Bytef*>(bytes.data() + 1);
        zs.avail_in = static_cast<uInt>(bytes.size() - 1);
        std::array<std::uint8_t, 1 << 16> chunk{};
        int rc = Z_OK;
        while (rc != Z_STREAM_END) {
            zs.next_out = chunk.data();
            zs.avail_out = static_cast<uInt>(chunk.size());
            rc = inflate(&zs, Z_NO_FLUSH);
            if (rc != Z_OK && rc != Z_STREAM_END) {
                inflateEnd(&zs);
                return Error(src + ": zlib data is corrupt (code " + std::to_string(rc) + ")");
            }
            data.insert(data.end(), chunk.data(), chunk.data() + (chunk.size() - zs.avail_out));
        }
        inflateEnd(&zs);
    }

    std::size_t pos = 0;
    auto need = [&](std::size_t n) { return pos + n <= data.size(); };
    auto u8 = [&]() { return data[pos++]; };
    auto u16 = [&]() {
        const auto v = static_cast<std::uint16_t>(data[pos] | (data[pos + 1] << 8));
        pos += 2;
        return v;
    };
    auto i32 = [&]() {
        const auto v = static_cast<std::int32_t>(data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (std::uint32_t{data[pos + 3]} << 24));
        pos += 4;
        return v;
    };
    auto text = [&](std::size_t n) {
        std::string s(reinterpret_cast<const char*>(data.data() + pos), n);
        pos += n;
        return s;
    };
    const auto truncated = Error(src + ": truncated .wmap");

    if (!need(2)) return truncated;
    const auto count = static_cast<std::int16_t>(u16());
    if (count <= 0) return Error(src + ": no tile templates");
    MapData map;
    map.name = src;
    std::unordered_map<std::string, std::optional<std::uint16_t>> object_cache;
    for (int i = 0; i < count; ++i) {
        if (!need(3)) return truncated;
        TileTemplate t;
        t.ground_type = u16();
        const std::size_t id_len = u8();
        if (!need(id_len + 1)) return truncated;
        const auto object_id = text(id_len);
        const std::size_t cfg_len = u8();
        if (!need(cfg_len + 2)) return truncated;
        pos += cfg_len;  // object config: unused by the original server
        const auto terrain = u8();
        t.terrain = terrain < kTerrainCount ? static_cast<Terrain>(terrain) : Terrain::None;
        if (const auto region = region_name(u8()); !region.empty()) t.regions.emplace_back(region);
        if (version == 1) {
            if (!need(1)) return truncated;
            pos += 1;  // elevation: presentation only
        }
        if (!object_id.empty()) {
            auto [it, inserted] = object_cache.try_emplace(object_id);
            if (inserted) {
                const auto* o = object(object_id);
                if (o == nullptr) warnings_.push_back(src + ": unknown object '" + object_id + "'");
                if (o != nullptr) it->second = o->type;
            }
            t.object_type = it->second;
        }
        if (ground(t.ground_type) == nullptr && t.ground_type != 0xFF) {
            warnings_.push_back(src + ": unknown ground type " + std::to_string(t.ground_type));
        }
        map.palette.push_back(std::move(t));
    }
    if (!need(8)) return truncated;
    const auto width = i32();
    const auto height = i32();
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return Error(src + ": bad map size");
    map.width = static_cast<std::uint16_t>(width);
    map.height = static_cast<std::uint16_t>(height);
    const std::size_t tiles = std::size_t(width) * std::size_t(height);
    if (!need(tiles * 2)) return truncated;
    map.tiles.resize(tiles);
    {
        // Raw pointers: this loop runs 4 million times for a realm, and checked iterators make debug builds crawl.
        const std::uint8_t* in = data.data() + pos;
        std::uint16_t* out = map.tiles.data();
        const auto palette_size = map.palette.size();
        for (std::size_t i = 0; i < tiles; ++i) {
            const auto index = static_cast<std::uint16_t>(in[2 * i] | (in[2 * i + 1] << 8));  // little-endian, unlike .jm
            if (index >= palette_size) return Error(src + ": tile index out of the palette");
            out[i] = index;
        }
        pos += tiles * 2;
    }
    if (version == 2) pos += tiles;  // per-tile elevation
    if (pos != data.size()) return Error(src + ": " + std::to_string(data.size() - pos) + " unexpected bytes at the end");
    maps_.emplace(src, std::move(map));
    return {};
}

std::expected<void, std::string> ContentDb::add_world(std::string_view json_text, std::string_view source_name) {
    const std::string src(source_name);
    try {
        const auto j = nlohmann::json::parse(strip_bom(json_text));
        WorldConfig w;
        w.id = j.value("Id", 0);
        w.name = j.at("Name").get<std::string>();
        w.display_name = j.value("DisplayName", w.name);
        w.music = j.value("Music", "");
        w.difficulty = j.value("Difficulty", 0);
        w.max_players = j.value("MaxPlayers", -1);
        w.blocks_sight = j.value("Blocksight", 0) != 0;
        w.maps = j.value("Maps", std::vector<std::string>{});
        w.long_lasting = j.value("LongLasting", false);
        w.raw_json = std::string(strip_bom(json_text));
        if (worlds_.contains(w.name)) {
            warnings_.push_back(src + ": duplicate world name '" + w.name + "' skipped (first wins)");
            return {};
        }
        worlds_.emplace(w.name, std::move(w));
    } catch (const nlohmann::json::exception& e) {
        return Error(src + ": " + e.what());
    }
    return {};
}

std::expected<void, std::string> ContentDb::link() {
    auto link_projectile = [this](ProjectileDesc& pd, const std::string& owner) {
        const auto* o = object(pd.object_id);
        if (o == nullptr || o->object_class != "Projectile") {
            warnings_.push_back("'" + owner + "' fires unknown projectile '" + pd.object_id + "'");
            return false;
        }
        pd.object_type = o->type;
        return true;
    };
    for (auto& [type, item] : items_) {
        if (item.projectile && !link_projectile(*item.projectile, item.id)) item.projectile.reset();
    }
    for (auto& [type, o] : objects_) {
        std::erase_if(o.projectiles, [&](auto& entry) { return !link_projectile(entry.second, o.id); });
    }

    // TierLoot pools: the original's ItemType classes by slot type (classic RotMG).
    tier_items_.clear();
    auto tier_class = [](int slot_type) -> std::optional<behavior::TierClass> {
        switch (slot_type) {
            case 1: case 2: case 3: case 8: case 17: case 24: return behavior::TierClass::Weapon;
            case 6: case 7: case 14: return behavior::TierClass::Armor;
            case 9: return behavior::TierClass::Ring;
            case 4: case 5: case 11: case 12: case 13: case 15: case 16: case 18: case 19: case 20: case 21: case 22: case 23: case 25:
                return behavior::TierClass::Ability;
            default: return std::nullopt;
        }
    };
    for (const auto& [type, item] : items_) {
        if (item.tier < 0) continue;
        if (const auto cls = tier_class(item.slot_type)) tier_items_[{static_cast<int>(*cls), item.tier}].push_back(type);
    }

    for (auto& list : terrain_spawns_) list.clear();
    for (const auto& [type, o] : objects_) {
        if (o.terrain == Terrain::None || !o.enemy) continue;
        terrain_spawns_[static_cast<std::size_t>(o.terrain)].push_back({type, o.spawn_prob, o.spawn.value_or(SpawnGroup{})});
    }

    for (auto& b : pending_behaviors_) {
        const auto* o = object(b.object_id);
        if (o == nullptr) {
            warnings_.push_back(b.source + ": behaviour for unknown object '" + b.object_id + "' skipped");
            continue;
        }
        b.object_type = o->type;
        if (behaviors_.contains(o->type)) {
            warnings_.push_back(b.source + ": second behaviour for '" + b.object_id + "' skipped (first wins)");
            continue;
        }
        link_behavior(b);
        behaviors_.emplace(o->type, std::move(b));
    }
    pending_behaviors_.clear();
    return {};
}

std::expected<ContentDb, std::string> ContentDb::load(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    ContentDb db;
    auto t0 = std::chrono::steady_clock::now();
#define PROFILE(x) do { auto t1 = std::chrono::steady_clock::now(); std::fprintf(stderr, "%s %lld ms\n", x, (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()); t0 = t1; } while (0)
    auto sorted_files = [](const fs::path& dir, std::string_view ext) {
        std::vector<fs::path> files;
        if (fs::exists(dir)) {
            for (const auto& e : fs::directory_iterator(dir)) {
                if (e.is_regular_file() && e.path().extension() == ext) files.push_back(e.path());
            }
        }
        std::sort(files.begin(), files.end());  // deterministic order (the reference loaded in parallel: order varied)
        return files;
    };
    const auto definitions = sorted_files(root / "Definitions", ".xml");
    if (definitions.empty()) return Error("no definition files in " + (root / "Definitions").string());
    for (const auto& file : definitions) {
        auto text = read_file(file);
        if (!text) return Error(text.error());
        if (auto r = db.add_xml(*text, file.filename().string()); !r) return Error(r.error());
    }
    for (const auto& file : sorted_files(root / "Maps", ".jm")) {
        auto text = read_file(file);
        if (!text) return Error(text.error());
        if (auto r = db.add_map(file.filename().string(), *text); !r) return Error(r.error());
    }
    for (const auto& file : sorted_files(root / "Maps", ".wmap")) {
        auto text = read_file(file);
        if (!text) return Error(text.error());
        const std::span bytes(reinterpret_cast<const std::uint8_t*>(text->data()), text->size());
        if (auto r = db.add_wmap(file.filename().string(), bytes); !r) return Error(r.error());
    }
    for (const auto& file : sorted_files(root / "Worlds", ".json")) {
        auto text = read_file(file);
        if (!text) return Error(text.error());
        if (auto r = db.add_world(*text, file.filename().string()); !r) return Error(r.error());
    }
    for (const auto& file : sorted_files(root / "Behaviors", ".json")) {
        auto text = read_file(file);
        if (!text) return Error(text.error());
        if (auto r = db.add_behaviors(*text, file.filename().string()); !r) return Error(r.error());
    }
    if (auto linked = db.link(); !linked) return Error(linked.error());
    for (const auto& [name, world] : db.worlds_) {
        for (const auto& m : world.maps) {
            if (!db.maps_.contains(m)) return Error("world '" + name + "' names missing map '" + m + "'");
        }
    }
    return db;
}

const GroundDesc* ContentDb::ground(std::uint16_t type) const {
    auto it = grounds_.find(type);
    return it == grounds_.end() ? nullptr : &it->second;
}
const GroundDesc* ContentDb::ground(std::string_view id) const {
    auto it = ground_by_id_.find(std::string(id));
    return it == ground_by_id_.end() ? nullptr : ground(it->second);
}
const ObjectDesc* ContentDb::object(std::uint16_t type) const {
    auto it = objects_.find(type);
    return it == objects_.end() ? nullptr : &it->second;
}
const ObjectDesc* ContentDb::object(std::string_view id) const {
    auto it = object_by_id_.find(std::string(id));
    return it == object_by_id_.end() ? nullptr : object(it->second);
}
const PlayerClassDesc* ContentDb::player_class(std::uint16_t type) const {
    auto it = classes_.find(type);
    return it == classes_.end() ? nullptr : &it->second;
}
const MapData* ContentDb::map(std::string_view name) const {
    auto it = maps_.find(name);
    return it == maps_.end() ? nullptr : &it->second;
}
const ItemDesc* ContentDb::item(std::uint16_t type) const {
    auto it = items_.find(type);
    return it == items_.end() ? nullptr : &it->second;
}
const ItemDesc* ContentDb::item(std::string_view id) const {
    auto it = item_by_id_.find(std::string(id));
    return it == item_by_id_.end() ? nullptr : item(it->second);
}
const behavior::Behavior* ContentDb::behavior(std::uint16_t object_type) const {
    auto it = behaviors_.find(object_type);
    return it == behaviors_.end() ? nullptr : &it->second;
}
const std::vector<std::uint16_t>& ContentDb::tier_items(behavior::TierClass cls, int tier) const {
    static const std::vector<std::uint16_t> kNone;
    auto it = tier_items_.find({static_cast<int>(cls), tier});
    return it == tier_items_.end() ? kNone : it->second;
}
const std::vector<TerrainSpawn>& ContentDb::terrain_spawns(Terrain terrain) const {
    return terrain_spawns_[std::min(static_cast<std::size_t>(terrain), kTerrainCount - 1)];
}
std::vector<std::uint16_t> ContentDb::group_members(std::string_view group) const {
    std::vector<std::uint16_t> out;
    if (group.empty()) return out;
    for (const auto& [type, o] : objects_) {
        if (o.group == group) out.push_back(type);
    }
    return out;
}
const WorldConfig* ContentDb::world(std::string_view name) const {
    auto it = worlds_.find(name);
    return it == worlds_.end() ? nullptr : &it->second;
}

}  // namespace runity::content

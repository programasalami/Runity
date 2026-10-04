#pragma once

// Small hand-made worlds for simulation tests. Legend:
//   '.' grass   '#' wall (FullOccupy, static)   'o' rock (OccupySquare, static)   '~' water (NoWalk)
//   's' sinking mud   'S' grass with a Spawn region   ' ' void (no ground)   'P' grass with a portal entity

#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "runity/content/content_db.hpp"

namespace runity::test {

inline content::ContentDb fixture_content() {
    content::ContentDb db;
    auto r = db.add_xml(R"(<GroundTypes>
        <Ground type="0x01" id="Grass"/>
        <Ground type="0x02" id="Water"><NoWalk/></Ground>
        <Ground type="0x03" id="Mud"><Sinking/><Speed>0.5</Speed></Ground>
        <Ground type="0xff" id="Empty"><NoWalk/></Ground>
    </GroundTypes>)", "grounds");
    REQUIRE(r.has_value());
    r = db.add_xml(R"(<Objects>
        <Object type="0x0100" id="Wall"><Class>Wall</Class><Static/><FullOccupy/><OccupySquare/></Object>
        <Object type="0x0101" id="Rock"><Class>GameObject</Class><Static/><OccupySquare/></Object>
        <Object type="0x0102" id="Portal"><Class>Portal</Class></Object>
    </Objects>)", "objects");
    REQUIRE(r.has_value());
    return db;
}

inline content::MapData fixture_map(const content::ContentDb& db, const std::vector<std::string>& rows) {
    content::MapData m;
    m.name = "fixture";
    m.height = static_cast<std::uint16_t>(rows.size());
    m.width = static_cast<std::uint16_t>(rows.at(0).size());
    auto palette_index = [&m](content::TileTemplate t) {
        m.palette.push_back(std::move(t));
        return static_cast<std::uint16_t>(m.palette.size() - 1);
    };
    for (const auto& row : rows) {
        REQUIRE(row.size() == m.width);
        for (char c : row) {
            content::TileTemplate t;
            t.ground_type = db.ground("Grass")->type;
            switch (c) {
                case '#': t.object_type = db.object("Wall")->type; break;
                case 'o': t.object_type = db.object("Rock")->type; break;
                case '~': t.ground_type = db.ground("Water")->type; break;
                case 's': t.ground_type = db.ground("Mud")->type; break;
                case 'S': t.regions.push_back("Spawn"); break;
                case 'P': t.object_type = db.object("Portal")->type; break;
                case ' ': t.ground_type = 0xFF; break;
                default: break;
            }
            m.tiles.push_back(palette_index(std::move(t)));
        }
    }
    return m;
}

inline content::WorldConfig fixture_world_config() {
    content::WorldConfig c;
    c.id = 1;
    c.name = "Fixture";
    c.display_name = "Fixture";
    return c;
}

}  // namespace runity::test

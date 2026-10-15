// Set pieces: the bridge's side (SET_PIECES.md in the narrator's repository).
//
// A set piece is the world's, not the narrator's: a creature appears and
// walks a road, a crier yells, the watch fights. The narrator plays one as it
// was written, through the commands here, and hears what becomes of it
// through the events here, as it hears anything else in the world.
//
//   world.zone   a zone read by its name, with nobody standing there: its
//                areas with a point in each, the creatures spawned in it, the
//                hostile ones spawned nowhere at all (only scripts put those
//                in the world), and its quests. What a set piece is written
//                from, by a frontier model, before anything plays.
//   npc.summon   a creature by its template at a point, for a while
//   npc.go       one of those sent to a point: a trip, by bot.go's method
//   npc.attack   one creature set on another
//   npc.find     the living creatures of one template nearest a point
//   npc.despawn  one of ours, taken out of the world now
//
//   npc.arrived  a summon's trip is over: there, or given up
//   npc.state    a summon's alive, fighting, seen or gone changed
//
// npc.say and weather, which a set piece also uses, are the bridge's own
// (BridgeCommands.cpp) and are used as they are.
//
// Every handler runs on the world thread (Bridge.h). A summon is kept by its
// guid and looked up again on its map every time it is needed: nothing here
// holds a creature past the call it was found in. Names at file scope carry
// a prefix, since three platforms' system headers are in play: macOS
// declares a global `Point`, and Windows defines `near` and `far`.

#include "playerbot/bridge/Bridge.h"

#include "playerbot/PlayerbotAI.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/playerbot.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/WorldPosition.h"

#include "AI/BaseAI/UnitAI.h"
#include "Entities/Creature.h"
#include "Entities/GameObject.h"
#include "Entities/Object.h"
#include "Entities/Player.h"
#include "Globals/ObjectMgr.h"
#include "Maps/GridDefines.h"
#include "Maps/GridMap.h"
#include "Maps/Map.h"
#include "Maps/MapManager.h"
#include "MotionGenerators/MotionMaster.h"
#include "Movement/MoveSpline.h"
#include "Quests/QuestDef.h"
#include "Server/DBCStores.h"
#include "Server/DBCStructure.h"
#include "Server/SQLStorages.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace BridgeProtocol;

namespace
{
    // A creature's trip ends as a bot's does (Bridge::WatchTrips): there
    // within five yards, or given up after it has stood still out of a fight
    // this many bubble intervals.
    constexpr float SET_PIECE_ARRIVED_YARDS = 5.0f;
    constexpr uint32 SET_PIECE_MOST_RETRIES = 30;

    double SetPieceNumber(const Json& args, const char* key, double fallback)
    {
        if (!args.contains(key) || args[key].is_null())
            return fallback;
        if (!args[key].is_number())
            throw BridgeCommandError(std::string("argument '") + key + "' must be a number");
        return args[key].get<double>();
    }

    double SetPieceRequireNumber(const Json& args, const char* key)
    {
        if (!args.contains(key) || !args[key].is_number())
            throw BridgeCommandError(std::string("argument '") + key + "' must be a number");
        return args[key].get<double>();
    }

    uint32 SetPieceUnsigned(const Json& args, const char* key, uint32 fallback)
    {
        if (!args.contains(key) || args[key].is_null())
            return fallback;
        if (!args[key].is_number_unsigned())
            throw BridgeCommandError(std::string("argument '") + key + "' must be an unsigned integer");
        return args[key].get<uint32>();
    }

    uint32 SetPieceRequireUnsigned(const Json& args, const char* key)
    {
        if (!args.contains(key) || !args[key].is_number_unsigned())
            throw BridgeCommandError(std::string("argument '") + key + "' must be an unsigned integer");
        return args[key].get<uint32>();
    }

    bool SetPieceBool(const Json& args, const char* key, bool fallback)
    {
        if (!args.contains(key) || args[key].is_null())
            return fallback;
        if (!args[key].is_boolean())
            throw BridgeCommandError(std::string("argument '") + key + "' must be true or false");
        return args[key].get<bool>();
    }

    ObjectGuid SetPieceGuid(const Json& args, const char* key)
    {
        if (!args.contains(key) || !args[key].is_string() || args[key].get<std::string>().empty())
            throw BridgeCommandError(std::string("argument '") + key + "' must be a guid (a decimal string)");
        try
        {
            return ObjectGuid(uint64(std::stoull(args[key].get<std::string>())));
        }
        catch (std::exception const&)
        {
            throw BridgeCommandError(std::string("argument '") + key + "' must be a decimal string");
        }
    }

    // A point the caller gave, checked before anything is put there: the core
    // logs and drops an object added off the map's grid.
    void SetPieceRequirePoint(float x, float y, float z)
    {
        if (!MaNGOS::IsValidMapCoord(x, y, z))
            throw BridgeCommandError("that point is off the map");
    }

    Json SetPiecePos(float x, float y, float z)
    {
        Json pos;
        pos["x"] = x;
        pos["y"] = y;
        pos["z"] = z;
        return pos;
    }

    bool SetPieceSameName(const std::string& a, const char* b)
    {
        if (!b || a.size() != std::strlen(b))
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(uint8(a[i])) != std::tolower(uint8(b[i])))
                return false;
        return true;
    }

    // A creature's type as the game names it (CreatureInfo::CreatureType).
    // The catalog is read by a frontier model, which knows "undead" and not 6.
    const char* SetPieceTypeName(uint32 type)
    {
        switch (type)
        {
            case CREATURE_TYPE_BEAST:      return "beast";
            case CREATURE_TYPE_DRAGONKIN:  return "dragonkin";
            case CREATURE_TYPE_DEMON:      return "demon";
            case CREATURE_TYPE_ELEMENTAL:  return "elemental";
            case CREATURE_TYPE_GIANT:      return "giant";
            case CREATURE_TYPE_UNDEAD:     return "undead";
            case CREATURE_TYPE_HUMANOID:   return "humanoid";
            case CREATURE_TYPE_CRITTER:    return "critter";
            case CREATURE_TYPE_MECHANICAL: return "mechanical";
            case CREATURE_TYPE_TOTEM:      return "totem";
            default:                       return "not specified";
        }
    }

    // How strong a rank is, for putting the strongest first.
    uint32 SetPieceRankWeight(uint32 rank)
    {
        switch (rank)
        {
            case CREATURE_ELITE_WORLDBOSS: return 4;
            case CREATURE_ELITE_RAREELITE: return 3;
            case CREATURE_ELITE_ELITE:     return 2;
            case CREATURE_ELITE_RARE:      return 1;
            default:                       return 0;
        }
    }

    Json SetPieceServices(uint32 flags)
    {
        Json services = Json::array();
        if (flags & UNIT_NPC_FLAG_GOSSIP)       services.push_back("gossip");
        if (flags & UNIT_NPC_FLAG_QUESTGIVER)   services.push_back("questgiver");
        if (flags & UNIT_NPC_FLAG_VENDOR)       services.push_back("vendor");
        if (flags & UNIT_NPC_FLAG_FLIGHTMASTER) services.push_back("flightmaster");
        if (flags & UNIT_NPC_FLAG_TRAINER)      services.push_back("trainer");
        if (flags & UNIT_NPC_FLAG_SPIRITHEALER) services.push_back("spirithealer");
        if (flags & UNIT_NPC_FLAG_INNKEEPER)    services.push_back("innkeeper");
        if (flags & UNIT_NPC_FLAG_BANKER)       services.push_back("banker");
        if (flags & UNIT_NPC_FLAG_AUCTIONEER)   services.push_back("auctioneer");
        if (flags & UNIT_NPC_FLAG_STABLEMASTER) services.push_back("stablemaster");
        if (flags & UNIT_NPC_FLAG_REPAIR)       services.push_back("repair");
        if (flags & UNIT_NPC_FLAG_BATTLEMASTER) services.push_back("battlemaster");
        return services;
    }

    // Which side's players a faction attacks: the faction against each side's
    // own (a human's for the Alliance, an orc's for the Horde), as the core
    // judges it (FactionTemplateEntry::IsHostileTo).
    Json SetPieceHostileTo(uint32 faction)
    {
        Json sides = Json::array();
        FactionTemplateEntry const* own = sFactionTemplateStore.LookupEntry(faction);
        if (!own)
            return sides;
        const std::pair<uint32, const char*> SIDES[] = {{RACE_HUMAN, "alliance"}, {RACE_ORC, "horde"}};
        for (auto const& [race, side] : SIDES)
            if (ChrRacesEntry const* entry = sChrRacesStore.LookupEntry(race))
                if (FactionTemplateEntry const* theirs = sFactionTemplateStore.LookupEntry(entry->FactionID))
                    if (own->IsHostileTo(*theirs))
                        sides.push_back(side);
        return sides;
    }

    // A creature template as the catalog gives it.
    Json SetPieceTemplate(CreatureInfo const* info)
    {
        Json entry;
        entry["entry"] = info->Entry;
        entry["name"] = std::string(info->Name ? info->Name : "");
        if (info->SubName && *info->SubName)
            entry["sub_name"] = std::string(info->SubName);
        entry["level_min"] = info->MinLevel;
        entry["level_max"] = info->MaxLevel;
        entry["rank"] = Bridge::RankName(info->Rank);
        entry["type"] = SetPieceTypeName(info->CreatureType);
        entry["hostile_to"] = SetPieceHostileTo(info->Faction);
        entry["services"] = SetPieceServices(info->NpcFlags);
        return entry;
    }

    // The zone an area is in: its parents followed to the top.
    uint32 SetPieceTopZone(uint32 areaId)
    {
        AreaTableEntry const* area = GetAreaEntryByAreaID(areaId);
        for (int depth = 0; area && area->zone && depth < 8; ++depth)
            area = GetAreaEntryByAreaID(area->zone);
        return area ? area->ID : 0;
    }
}

// The zone ---------------------------------------------------------------------
//
// Everything a set piece may name, read off what the server already holds:
// the area table and the module's explore locations for the places, the
// spawn table (each spawn's zone the core worked out from the terrain at
// load, CreatureData::OriginalZoneId) and the terrain for who stands where,
// every creature template for what could be summoned, and the quest
// templates for the zone's own story in the game's own words. Nobody needs to
// be in the zone.
//
// It walks the whole spawn table and every creature template, and reads the
// zone's terrain from disk the first time: tens to a few hundred
// milliseconds on the world thread, once, when a brief is made. Play never
// calls it.

std::optional<Json> Bridge::CmdWorldZone(const BridgeInbound&, const Json& args)
{
    const std::string wanted = RequireString(args, "zone");
    AreaTableEntry const* zone = nullptr;
    for (uint32 i = 0; i < sAreaStore.GetNumRows() && !zone; ++i)
        if (AreaTableEntry const* entry = sAreaStore.LookupEntry(i))
            if (entry->zone == 0 && SetPieceSameName(wanted, entry->area_name[0]))
                zone = entry;
    if (!zone)
        throw BridgeCommandError("no zone is called '" + wanted + "'");
    const uint32 zoneId = zone->ID;
    const uint32 mapId = zone->mapid;
    const std::string zoneName = zone->area_name[0];

    // The areas. Each area's point is the module's own for it: of every
    // creature spawn in the area (its explore locations), leaving out those
    // under water, the real spawn position nearest their centre. It is how
    // the module makes the travel node it names for the area
    // (TravelNodeMap::generateZoneMeanNodes), and a place a creature
    // actually stands. The name is the area table's. An area nothing is
    // spawned in has no point and is not here.
    decltype(sTravelMgr.GetExploreLocs()) explore;
    try
    {
        explore = sTravelMgr.GetExploreLocs();
    }
    catch (const std::out_of_range&)
    {
        throw BridgeCommandError("the module's travel data is not loaded: there are no places to name");
    }
    std::vector<std::pair<std::string, Json>> areaList;
    for (auto const& [areaId, dests] : explore)
    {
        AreaTableEntry const* area = GetAreaEntryByAreaID(uint32(areaId));
        if (!area || area->mapid != mapId || SetPieceTopZone(area->ID) != zoneId)
            continue;
        std::vector<ai::WorldPosition*> all, dry;
        for (auto* dest : dests)
            for (ai::WorldPosition* point : dest->GetPoints())
            {
                all.push_back(point);
                if (!point->isUnderWater())
                    dry.push_back(point);
            }
        if (all.empty())
            continue;
        const ai::WorldPosition middle(dry.empty() ? all : dry, ai::WP_MEAN_CENTROID);
        Json entry;
        entry["id"] = area->ID;
        entry["name"] = std::string(area->area_name[0]);
        if (area->zone && area->zone != zoneId)
            if (AreaTableEntry const* parent = GetAreaEntryByAreaID(area->zone))
                entry["within"] = std::string(parent->area_name[0]);
        entry["pos"] = SetPiecePos(middle.getX(), middle.getY(), middle.getZ());
        areaList.emplace_back(entry["name"].get<std::string>(), entry);
    }
    std::sort(areaList.begin(), areaList.end(), [](auto const& a, auto const& b) { return a.first < b.first; });

    // Who is spawned in the zone, by template, with how many stand in each
    // area; and every template spawned anywhere at all, random entries
    // included (creature_spawn_entry), for the summonable list.
    struct Spawned
    {
        uint32 count = 0;
        std::map<std::string, uint32> areas;
    };
    std::map<uint32, Spawned> inZone;
    std::set<uint32> spawnedAnywhere;
    auto worker = [&](CreatureDataPair const& pair) -> bool
    {
        const CreatureData& data = pair.second;
        std::vector<uint32> entries;
        if (data.id)
            entries.push_back(data.id);
        else if (std::vector<uint32> const* random = sObjectMgr.GetAllRandomCreatureEntries(pair.first))
            entries = *random;
        spawnedAnywhere.insert(entries.begin(), entries.end());
        if (data.mapid != mapId || data.OriginalZoneId != zoneId)
            return false;
        AreaTableEntry const* area = GetAreaEntryByAreaID(sTerrainMgr.GetAreaId(data.mapid, data.posX, data.posY, data.posZ));
        const std::string areaName = area ? std::string(area->area_name[0]) : zoneName;
        for (uint32 entry : entries)
        {
            Spawned& spawned = inZone[entry];
            ++spawned.count;
            ++spawned.areas[areaName];
        }
        return false;
    };
    sObjectMgr.DoCreatureData(worker);

    // The zone's levels: its hostile creatures', unless the caller says.
    uint32 levelLow = 0, levelHigh = 0;
    Json creatures = Json::array();
    for (auto const& [entry, spawned] : inZone)
    {
        CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(entry);
        if (!info)
            continue;
        Json creature = SetPieceTemplate(info);
        creature["count"] = spawned.count;
        Json where = Json::object();
        for (auto const& [area, count] : spawned.areas)
            where[area] = count;
        creature["areas"] = where;
        if (!creature["hostile_to"].empty())
        {
            levelLow = levelLow ? std::min(levelLow, info->MinLevel) : info->MinLevel;
            levelHigh = std::max(levelHigh, info->MaxLevel);
        }
        creatures.push_back(creature);
    }
    levelLow = SetPieceUnsigned(args, "level_min", levelLow);
    levelHigh = SetPieceUnsigned(args, "level_max", levelHigh);

    // What could be summoned: hostile templates spawned nowhere, at the
    // zone's levels, strongest first. Nothing is dropped by its name: a
    // developer's entry ("[DND] Trigger") goes with the rest, and the brief
    // says what one looks like (the owner, 15 October).
    std::vector<CreatureInfo const*> unspawned;
    for (SQLStorageBase::SQLSIterator<CreatureInfo> it = sCreatureStorage.getDataBegin<CreatureInfo>(); it < sCreatureStorage.getDataEnd<CreatureInfo>(); ++it)
    {
        CreatureInfo const* info = *it;
        if (!info || spawnedAnywhere.count(info->Entry))
            continue;
        if (info->MaxLevel < levelLow || info->MinLevel > levelHigh)
            continue;
        if (SetPieceHostileTo(info->Faction).empty())
            continue;
        unspawned.push_back(info);
    }
    std::sort(unspawned.begin(), unspawned.end(), [](CreatureInfo const* a, CreatureInfo const* b) {
        if (SetPieceRankWeight(a->Rank) != SetPieceRankWeight(b->Rank))
            return SetPieceRankWeight(a->Rank) > SetPieceRankWeight(b->Rank);
        if (a->MaxLevel != b->MaxLevel)
            return a->MaxLevel > b->MaxLevel;
        return a->Entry < b->Entry;
    });
    Json summonable = Json::array();
    for (CreatureInfo const* info : unspawned)
        summonable.push_back(SetPieceTemplate(info));

    // The zone's quests, every word of them, with who gives each and who
    // takes it, and the areas they stand in when they stand in this zone.
    std::map<uint32, Quest const*> quests;
    for (auto const& [id, quest] : sObjectMgr.GetQuestTemplates())
        if (quest && quest->GetZoneOrSort() == int32(zoneId))
            quests[id] = quest.get();
    std::map<uint32, Json> givers, takers;
    auto person = [&inZone](uint32 entry, const char* name, const char* kind)
    {
        Json who;
        who["entry"] = entry;
        who["name"] = std::string(name ? name : "");
        who["kind"] = kind;
        Json where = Json::array();
        auto found = inZone.find(entry);
        if (found != inZone.end())
            for (auto const& [area, count] : found->second.areas)
                where.push_back(area);
        who["areas"] = where;
        return who;
    };
    if (!quests.empty())
    {
        for (SQLStorageBase::SQLSIterator<CreatureInfo> it = sCreatureStorage.getDataBegin<CreatureInfo>(); it < sCreatureStorage.getDataEnd<CreatureInfo>(); ++it)
        {
            CreatureInfo const* info = *it;
            if (!info)
                continue;
            QuestRelationsMapBounds gives = sObjectMgr.GetCreatureQuestRelationsMapBounds(info->Entry);
            for (auto rel = gives.first; rel != gives.second; ++rel)
                if (quests.count(rel->second))
                    givers[rel->second].push_back(person(info->Entry, info->Name, "creature"));
            QuestRelationsMapBounds takes = sObjectMgr.GetCreatureQuestInvolvedRelationsMapBounds(info->Entry);
            for (auto rel = takes.first; rel != takes.second; ++rel)
                if (quests.count(rel->second))
                    takers[rel->second].push_back(person(info->Entry, info->Name, "creature"));
        }
        for (SQLStorageBase::SQLSIterator<GameObjectInfo> it = sGOStorage.getDataBegin<GameObjectInfo>(); it < sGOStorage.getDataEnd<GameObjectInfo>(); ++it)
        {
            GameObjectInfo const* info = *it;
            if (!info)
                continue;
            QuestRelationsMapBounds gives = sObjectMgr.GetGOQuestRelationsMapBounds(info->id);
            for (auto rel = gives.first; rel != gives.second; ++rel)
                if (quests.count(rel->second))
                    givers[rel->second].push_back(person(info->id, info->name, "object"));
            QuestRelationsMapBounds takes = sObjectMgr.GetGOQuestInvolvedRelationsMapBounds(info->id);
            for (auto rel = takes.first; rel != takes.second; ++rel)
                if (quests.count(rel->second))
                    takers[rel->second].push_back(person(info->id, info->name, "object"));
        }
    }
    std::vector<Quest const*> questOrder;
    for (auto const& [id, quest] : quests)
        questOrder.push_back(quest);
    std::sort(questOrder.begin(), questOrder.end(), [](Quest const* a, Quest const* b) {
        if (a->GetQuestLevel() != b->GetQuestLevel())
            return a->GetQuestLevel() < b->GetQuestLevel();
        return a->GetQuestId() < b->GetQuestId();
    });
    Json questList = Json::array();
    for (Quest const* quest : questOrder)
    {
        Json entry;
        entry["id"] = quest->GetQuestId();
        entry["title"] = quest->GetTitle();
        entry["level"] = quest->GetQuestLevel();
        entry["level_min"] = quest->GetMinLevel();
        entry["details"] = quest->GetDetails();
        entry["objectives"] = quest->GetObjectives();
        if (!quest->GetRequestItemsText().empty())
            entry["request_text"] = quest->GetRequestItemsText();
        if (!quest->GetOfferRewardText().empty())
            entry["reward_text"] = quest->GetOfferRewardText();
        if (!quest->GetEndText().empty())
            entry["end_text"] = quest->GetEndText();
        auto given = givers.find(quest->GetQuestId());
        entry["givers"] = given != givers.end() ? given->second : Json::array();
        auto taken = takers.find(quest->GetQuestId());
        entry["takers"] = taken != takers.end() ? taken->second : Json::array();
        questList.push_back(entry);
    }

    Json result;
    Json zoneRef;
    zoneRef["id"] = zoneId;
    zoneRef["name"] = zoneName;
    zoneRef["map"] = mapId;
    zoneRef["level_min"] = levelLow;
    zoneRef["level_max"] = levelHigh;
    result["zone"] = zoneRef;
    Json areas = Json::array();
    for (auto& [name, area] : areaList)
        areas.push_back(area);
    result["areas"] = areas;
    result["creatures"] = creatures;
    result["summonable"] = summonable;
    result["quests"] = questList;
    return result;
}

// Playing ----------------------------------------------------------------------

// One of the bridge's own summons, from `guid`: refused for anybody else, and
// when it is no longer in the world.
Creature* Bridge::FindSummon(const Json& args, ObjectGuid* guidOut)
{
    const ObjectGuid guid = SetPieceGuid(args, "guid");
    auto found = summons_.find(guid);
    if (found == summons_.end())
        throw BridgeCommandError("that creature is not one a set piece summoned");
    Map* map = sMapMgr.FindMap(found->second.map, found->second.instance);
    Creature* creature = map ? map->GetCreature(guid) : nullptr;
    if (!creature || !creature->IsInWorld())
        throw BridgeCommandError("that creature is no longer in the world");
    if (guidOut)
        *guidOut = guid;
    return creature;
}

// A creature by its template at a point, for `lifetime` seconds (60 to 7200,
// 1800 by default), its body left `corpse` seconds after it dies (0 to 3600,
// 300 by default) so it can be looted. It is an active object, so its stretch
// of the map stays loaded and it goes on with nobody near; its own movement
// is set to standing, so a template with a route of its own does not walk
// off on it. `level` (1 to 63) overrides the template's.
std::optional<Json> Bridge::CmdNpcSummon(const BridgeInbound&, const Json& args)
{
    const uint32 entry = SetPieceRequireUnsigned(args, "entry");
    CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(entry);
    if (!info)
        throw BridgeCommandError("there is no creature template " + std::to_string(entry));
    const uint32 mapId = SetPieceRequireUnsigned(args, "map");
    const uint32 instance = SetPieceUnsigned(args, "instance", 0);
    Map* map = sMapMgr.FindMap(mapId, instance);
    if (!map)
        throw BridgeCommandError("that map is not loaded");
    const float x = float(SetPieceRequireNumber(args, "x"));
    const float y = float(SetPieceRequireNumber(args, "y"));
    const float z = float(SetPieceRequireNumber(args, "z"));
    const float o = float(SetPieceNumber(args, "o", 0.0));
    SetPieceRequirePoint(x, y, z);
    const double lifetime = SetPieceNumber(args, "lifetime", 1800.0);
    if (lifetime < 60.0 || lifetime > 7200.0)
        throw BridgeCommandError("argument 'lifetime' must be 60 to 7200 seconds");
    const double corpse = SetPieceNumber(args, "corpse", 300.0);
    if (corpse < 0.0 || corpse > 3600.0)
        throw BridgeCommandError("argument 'corpse' must be 0 to 3600 seconds");
    const uint32 level = SetPieceUnsigned(args, "level", 0);
    if (level > 63)
        throw BridgeCommandError("argument 'level' must be 1 to 63");
    const bool run = SetPieceBool(args, "run", false);

    TempSpawnSettings settings(nullptr, entry, x, y, z, o, TEMPSPAWN_TIMED_OR_DEAD_DESPAWN, uint32(lifetime * 1000.0),
                               true /* active */, run, 0 /* path */, 0 /* faction */, 0 /* model */, false, false, 0,
                               int32(IDLE_MOTION_TYPE), level);
    Creature* creature = WorldObject::SummonCreature(settings, map);
    if (!creature)
        throw BridgeCommandError("the core would not summon it");
    creature->SetCorpseDelay(uint32(corpse));

    SetPieceSummon& summon = summons_[creature->GetObjectGuid()];
    summon = SetPieceSummon();
    summon.map = mapId;
    summon.instance = instance;

    Json result;
    result["unit"] = UnitRef(creature);
    result["map"] = mapId;
    result["pos"] = SetPiecePos(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ());
    result["lifetime"] = uint32(lifetime);
    result["corpse"] = uint32(corpse);
    return result;
}

// One of our summons sent to a point, the whole way: a creature's trip, by
// the method of a bot's (bot.go, Bridge::WatchTrips), kept apart from the
// bots' trips. Pathfound over the ground (MotionMaster::MovePoint), and taken
// up again at the bubble interval whenever it stands still out of a fight,
// so a fight, and the core walking it back to where the fight began, are left
// alone. npc.arrived says when the trip is over, there or given up. Sent
// again while on a trip, the new trip replaces the old one, which ends
// without an event, as a bot's does.
std::optional<Json> Bridge::CmdNpcGo(const BridgeInbound&, const Json& args)
{
    ObjectGuid guid;
    Creature* creature = FindSummon(args, &guid);
    if (!creature->IsAlive())
        throw BridgeCommandError("the creature is dead");
    const float x = float(SetPieceRequireNumber(args, "x"));
    const float y = float(SetPieceRequireNumber(args, "y"));
    const float z = float(SetPieceRequireNumber(args, "z"));
    SetPieceRequirePoint(x, y, z);

    SetPieceSummon& summon = summons_[guid];
    summon.going = true;
    summon.x = x;
    summon.y = y;
    summon.z = z;
    summon.to = OptionalString(args, "to");
    summon.run = SetPieceBool(args, "run", false);
    summon.retries = 0;
    if (!creature->IsInCombat())
        creature->GetMotionMaster()->MovePoint(0, ::Position(x, y, z, 0.0f), summon.run ? FORCED_MOVEMENT_RUN : FORCED_MOVEMENT_WALK);

    Json result;
    result["unit"] = UnitRef(creature);
    Json to;
    to["name"] = summon.to;
    to["pos"] = SetPiecePos(x, y, z);
    to["dist"] = creature->GetDistance(x, y, z);
    result["to"] = to;
    result["going"] = true;
    return result;
}

// One creature set on another on the same map: `guid` (and its `map`) the
// attacker, `target` the one it goes for. Either may be anybody in the world,
// a resident or a summon. Refused when the core says the attacker cannot
// attack its target.
std::optional<Json> Bridge::CmdNpcAttack(const BridgeInbound&, const Json& args)
{
    Creature* attacker = FindCreature(args);
    const ObjectGuid targetGuid = SetPieceGuid(args, "target");
    Creature* target = targetGuid.IsCreatureOrPet() ? attacker->GetMap()->GetCreature(targetGuid) : nullptr;
    if (!target || !target->IsInWorld())
        throw BridgeCommandError("the target is not in the world on the attacker's map");
    if (!attacker->IsAlive())
        throw BridgeCommandError("the attacker is dead");
    if (!target->IsAlive())
        throw BridgeCommandError("the target is dead");
    if (!attacker->CanAttack(target))
        throw BridgeCommandError("the game will not let it attack that one");
    if (!attacker->AI())
        throw BridgeCommandError("the attacker has no mind to fight with");
    attacker->AI()->AttackStart(target);

    Json result;
    result["unit"] = UnitRef(attacker);
    result["target"] = UnitRef(target);
    return result;
}

// The living creatures of one template nearest a point on a map: within
// `radius` yards (300 by default), at most `limit` of them (1 by default, 10
// at most), nearest first. Read from the spawn table, then each looked up
// live by its spawn: only creatures in a loaded stretch of the map are in the
// world to be found.
std::optional<Json> Bridge::CmdNpcFind(const BridgeInbound&, const Json& args)
{
    const uint32 entry = SetPieceRequireUnsigned(args, "entry");
    const uint32 mapId = SetPieceRequireUnsigned(args, "map");
    const uint32 instance = SetPieceUnsigned(args, "instance", 0);
    Map* map = sMapMgr.FindMap(mapId, instance);
    if (!map)
        throw BridgeCommandError("that map is not loaded");
    const float x = float(SetPieceRequireNumber(args, "x"));
    const float y = float(SetPieceRequireNumber(args, "y"));
    const float z = float(SetPieceRequireNumber(args, "z"));
    const float radius = float(SetPieceNumber(args, "radius", 300.0));
    const uint32 limit = SetPieceUnsigned(args, "limit", 1);
    if (limit < 1 || limit > 10)
        throw BridgeCommandError("argument 'limit' must be 1 to 10");

    std::vector<std::pair<float, Creature*>> found;
    auto worker = [&](CreatureDataPair const& pair) -> bool
    {
        const CreatureData& data = pair.second;
        if (data.mapid != mapId)
            return false;
        if (data.id != entry)
        {
            std::vector<uint32> const* random = data.id ? nullptr : sObjectMgr.GetAllRandomCreatureEntries(pair.first);
            if (!random || std::find(random->begin(), random->end(), entry) == random->end())
                return false;
        }
        Creature* creature = map->GetCreature(uint32(pair.first));
        if (!creature || !creature->IsInWorld() || !creature->IsAlive() || creature->GetEntry() != entry)
            return false;
        const float dist = creature->GetDistance(x, y, z);
        if (dist <= radius)
            found.emplace_back(dist, creature);
        return false;
    };
    sObjectMgr.DoCreatureData(worker);
    std::sort(found.begin(), found.end(), [](auto const& a, auto const& b) { return a.first < b.first; });

    Json units = Json::array();
    for (auto const& [dist, creature] : found)
    {
        if (units.size() >= limit)
            break;
        Json unit;
        unit["unit"] = UnitRef(creature);
        unit["pos"] = SetPiecePos(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ());
        unit["dist"] = dist;
        units.push_back(unit);
    }
    Json result;
    result["map"] = mapId;
    result["units"] = units;
    return result;
}

// One of our summons taken out of the world now. Its npc.state says gone at
// the next watch, as for any summon that leaves.
std::optional<Json> Bridge::CmdNpcDespawn(const BridgeInbound&, const Json& args)
{
    Creature* creature = FindSummon(args);
    Json result;
    result["unit"] = UnitRef(creature);
    creature->ForcedDespawn();
    return result;
}

// What becomes of our summons, at the bubble interval, while a client is
// connected (Bridge::Update): a trip ended (npc.arrived), and alive, fighting,
// seen or gone changed (npc.state). The first watch after a summon reports it
// whatever it is. `seen` is the bridge's own measure of what is round the
// party: within its scene radius of a real player, as unit.entered is.
void Bridge::WatchSummons()
{
    if (summons_.empty())
        return;
    const float radius = sPlayerbotAIConfig.bridgeSceneRadius > 0.0f ? sPlayerbotAIConfig.bridgeSceneRadius : 40.0f;
    const std::vector<Player*> reals = RealPlayersOnline();
    for (auto it = summons_.begin(); it != summons_.end();)
    {
        const ObjectGuid guid = it->first;
        SetPieceSummon& summon = it->second;
        Map* map = sMapMgr.FindMap(summon.map, summon.instance);
        Creature* creature = map ? map->GetCreature(guid) : nullptr;
        const bool gone = !creature || !creature->IsInWorld();
        Json ref;
        if (gone)
            ref["guid"] = GuidString(guid);
        else
            ref = UnitRef(creature);

        if (summon.going)
        {
            bool done = false;
            bool reached = false;
            if (gone || !creature->IsAlive())
                done = true;
            else if (creature->GetDistance(summon.x, summon.y, summon.z) <= SET_PIECE_ARRIVED_YARDS)
                done = reached = true;
            else if (!creature->IsInCombat() && !creature->IsMoving() && creature->movespline->Finalized())
            {
                if (++summon.retries > SET_PIECE_MOST_RETRIES)
                    done = true;
                else
                    creature->GetMotionMaster()->MovePoint(0, ::Position(summon.x, summon.y, summon.z, 0.0f),
                                                           summon.run ? FORCED_MOVEMENT_RUN : FORCED_MOVEMENT_WALK);
            }
            if (done)
            {
                summon.going = false;
                Json data;
                data["unit"] = ref;
                data["to"] = summon.to;
                data["reached"] = reached;
                data["pos"] = SetPiecePos(summon.x, summon.y, summon.z);
                Emit(EV_NPC_ARRIVED, data);
            }
        }

        const bool alive = !gone && creature->IsAlive();
        const bool fighting = !gone && creature->IsInCombat();
        bool seen = false;
        if (!gone)
            for (Player* real : reals)
                if (real->GetMap() == map && real->GetDistance(creature) <= radius)
                {
                    seen = true;
                    break;
                }
        if (gone || !summon.reported || alive != summon.alive || fighting != summon.fighting || seen != summon.seen)
        {
            Json data;
            data["guid"] = GuidString(guid);
            data["unit"] = ref;
            data["alive"] = alive;
            data["fighting"] = fighting;
            data["seen"] = seen;
            data["gone"] = gone;
            Emit(EV_NPC_STATE, data);
            summon.reported = true;
            summon.alive = alive;
            summon.fighting = fighting;
            summon.seen = seen;
        }
        if (gone)
            it = summons_.erase(it);
        else
            ++it;
    }
}

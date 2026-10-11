// Set pieces: real creatures put in the world for a scripted moment, the way
// the game's own Stitches walks from Raven Hill to Darkshire. The narrator
// holds the script (Azeroth_Narrator_State, SET_PIECES.md) and plays it a
// step at a time; this side only does what one step asks, and reports back
// what became of the creatures it made:
//
//   npc.summon    a creature by template entry, at a point, for a while
//   npc.walk      a summon along a list of points, a leg at a time
//   npc.attack    one creature set on another (a guard on the summon)
//   npc.despawn   a summon taken out of the world
//   npc.find      the live creature of an entry nearest a point (a resident to speak)
//   world.catalog what a script may name: the creatures and areas round a player,
//                 and the creatures the game only ever summons
//
// and, every world tick, npc.arrived, npc.combat, npc.died and npc.gone for
// each summon. Nothing is saved: a summon is a temporary spawn, and a server
// restart clears them all, which is what the narrator's once-per-start relies on.
//
// Still not a console: entries and points come in, never a command string.

#include "playerbot/bridge/Bridge.h"

#include "playerbot/WorldPosition.h"

#include "Entities/Creature.h"
#include "Entities/Player.h"
#include "Entities/TemporarySpawn.h"
#include "Globals/ObjectMgr.h"
#include "Maps/Map.h"
#include "Maps/MapManager.h"
#include "MotionGenerators/MotionMaster.h"
#include "Server/DBCStores.h"
#include "Server/DBCStructure.h"
#include "Server/SQLStorages.h"
#include "AI/BaseAI/UnitAI.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

using namespace BridgeProtocol;

namespace
{
    // How near a point counts as reached, in yards. A path ends a little off
    // its point, and a creature that has stopped two yards short is there.
    constexpr float ARRIVED_YARDS = 4.0f;
    // Legs issued toward one point before it is given up and the next one
    // taken: a point the path finder cannot reach would otherwise be asked
    // for every tick, for ever. A leg is issued only when the creature is
    // standing, so this is twenty stops short of it, not twenty ticks.
    constexpr uint32 MAX_TRIES = 20;
    constexpr uint32 MAX_POINTS = 32;

    uint32 OptionalUnsigned(const Json& args, const char* key, uint32 fallback)
    {
        if (!args.contains(key) || args[key].is_null())
            return fallback;
        if (!args[key].is_number_unsigned())
            throw BridgeCommandError(std::string("argument '") + key + "' must be an unsigned integer");
        return args[key].get<uint32>();
    }

    double OptionalNumber(const Json& args, const char* key, double fallback)
    {
        if (!args.contains(key) || args[key].is_null())
            return fallback;
        if (!args[key].is_number())
            throw BridgeCommandError(std::string("argument '") + key + "' must be a number");
        return args[key].get<double>();
    }

    double RequireNumber(const Json& args, const char* key)
    {
        if (!args.contains(key) || !args[key].is_number())
            throw BridgeCommandError(std::string("argument '") + key + "' must be a number");
        return args[key].get<double>();
    }

    bool OptionalBool(const Json& args, const char* key, bool fallback)
    {
        if (!args.contains(key) || args[key].is_null())
            return fallback;
        if (!args[key].is_boolean())
            throw BridgeCommandError(std::string("argument '") + key + "' must be true or false");
        return args[key].get<bool>();
    }

    Json PointJson(float x, float y, float z)
    {
        Json pos;
        pos["x"] = x;
        pos["y"] = y;
        pos["z"] = z;
        return pos;
    }

    // The map a command means: `map` when given (a continent, instance 0),
    // else the first real player's.
    Map* MapFor(const Json& args, const std::vector<Player*>& reals)
    {
        if (args.contains("map") && !args["map"].is_null())
        {
            if (!args["map"].is_number_unsigned())
                throw BridgeCommandError("argument 'map' must be an unsigned integer");
            if (Map* map = sMapMgr.FindMap(args["map"].get<uint32>(), 0))
                return map;
            throw BridgeCommandError("map is not loaded");
        }
        if (reals.empty())
            throw BridgeCommandError("no real player is online; pass 'map'");
        return reals.front()->GetMap();
    }

    // Whether a template is somebody to cast in a set piece's catalog at all:
    // a named creature, not a test entry, a trigger or a totem.
    bool Castable(CreatureInfo const* info)
    {
        if (!info || !info->Name || !*info->Name)
            return false;
        const std::string name(info->Name);
        if (name[0] == '[' || name.find("DND") != std::string::npos || name.find("Trigger") != std::string::npos
                || name.find("(") != std::string::npos)
            return false;
        return info->CreatureType != CREATURE_TYPE_TOTEM && info->CreatureType != CREATURE_TYPE_CRITTER;
    }

    Json Template(CreatureInfo const* info)
    {
        Json entry;
        entry["entry"] = info->Entry;
        entry["name"] = std::string(info->Name);
        if (info->SubName && *info->SubName)
            entry["sub_name"] = std::string(info->SubName);
        entry["level"] = Json::array({info->MinLevel, info->MaxLevel});
        entry["rank"] = Bridge::RankName(info->Rank);
        entry["type"] = Bridge::TypeName(info->CreatureType);
        return entry;
    }
}

// npc.summon --------------------------------------------------------------------

std::optional<Json> Bridge::CmdNpcSummon(const BridgeInbound&, const Json& args)
{
    // A creature by its template, at a point, as a temporary spawn: it lives
    // `lifetime` seconds at most, and its body stays `corpse` seconds after it
    // dies so it can be looted, then it is gone. Active, so it keeps walking
    // where no player is and the grids under it stay loaded. Its own
    // movement is set to standing: a template that walks a set route
    // elsewhere would otherwise fight the route npc.walk gives it.
    const uint32 entry = OptionalUnsigned(args, "entry", 0);
    CreatureInfo const* info = entry ? ObjectMgr::GetCreatureTemplate(entry) : nullptr;
    if (!info)
        throw BridgeCommandError("argument 'entry' must be a creature template that exists");
    const std::vector<Player*> reals = RealPlayersOnline();
    Map* map = MapFor(args, reals);
    const float x = float(RequireNumber(args, "x"));
    const float y = float(RequireNumber(args, "y"));
    const float z = float(RequireNumber(args, "z"));
    const float o = float(OptionalNumber(args, "o", 0.0));
    const double lifetime = OptionalNumber(args, "lifetime", 1800.0);
    if (lifetime < 30.0 || lifetime > 7200.0)
        throw BridgeCommandError("argument 'lifetime' must be from 30 to 7200 seconds");
    const double corpse = OptionalNumber(args, "corpse", 300.0);
    if (corpse < 0.0 || corpse > 3600.0)
        throw BridgeCommandError("argument 'corpse' must be from 0 to 3600 seconds");
    const uint32 level = OptionalUnsigned(args, "level", 0);
    if (level > 63)
        throw BridgeCommandError("argument 'level' must be from 1 to 63");
    const bool run = OptionalBool(args, "run", false);

    TempSpawnSettings settings(nullptr, entry, x, y, z, o, TEMPSPAWN_TIMED_OR_DEAD_DESPAWN, uint32(lifetime * 1000.0),
                               true, run, 0, 0, 0, false, false, 0, int32(IDLE_MOTION_TYPE), level);
    settings.corpseDespawnTime = uint32(corpse);   // seconds: Creature::SetCorpseDelay
    Creature* creature = WorldObject::SummonCreature(settings, map);
    if (!creature)
        throw BridgeCommandError("the creature could not be put in the world there");

    Summon summon;
    summon.map = map->GetId();
    summon.run = run;
    summons_[creature->GetObjectGuid()] = summon;

    Json result;
    result["unit"] = UnitRef(creature);
    result["map"] = map->GetId();
    result["pos"] = PointJson(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ());
    result["lifetime"] = lifetime;
    return result;
}

// npc.walk ----------------------------------------------------------------------

std::optional<Json> Bridge::CmdNpcWalk(const BridgeInbound&, const Json& args)
{
    // A summon along a list of points, in order. Each leg is a path the core
    // finds over the ground, issued when the creature stands: a fight on the
    // way is its own business, and once it is over (the creature walked back
    // to where the fight began, as the core does) the walk carries on from
    // the point it was heading for. Arriving at the last point is the
    // npc.arrived event. A new walk replaces the old one.
    Creature* creature = FindCreature(args);
    auto it = summons_.find(creature->GetObjectGuid());
    if (it == summons_.end())
        throw BridgeCommandError("only a creature npc.summon made can be walked; npc.move walks somebody who lives here");
    if (!args.contains("points") || !args["points"].is_array() || args["points"].empty())
        throw BridgeCommandError("argument 'points' must be a non-empty list of {x, y, z}");
    if (args["points"].size() > MAX_POINTS)
        throw BridgeCommandError("argument 'points' takes at most 32 points");
    std::vector<SummonPoint> route;
    for (const Json& p : args["points"])
    {
        if (!p.is_object())
            throw BridgeCommandError("each point must be {x, y, z}");
        SummonPoint point;
        point.x = float(RequireNumber(p, "x"));
        point.y = float(RequireNumber(p, "y"));
        point.z = float(RequireNumber(p, "z"));
        route.push_back(point);
    }
    Summon& summon = it->second;
    summon.route = route;
    summon.run = OptionalBool(args, "run", summon.run);
    summon.tries = 0;
    creature->SetWalk(!summon.run);
    // Whatever walk it was on comes off, so the new route starts now rather
    // than after the old leg ends; the tick issues the first leg.
    if (creature->IsAlive() && !creature->IsInCombat())
    {
        MotionMaster* motion = creature->GetMotionMaster();
        if (motion->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
            motion->MovementExpired(false);
    }

    Json result;
    result["unit"] = UnitRef(creature);
    result["points"] = route.size();
    result["run"] = summon.run;
    return result;
}

// npc.attack --------------------------------------------------------------------

std::optional<Json> Bridge::CmdNpcAttack(const BridgeInbound&, const Json& args)
{
    // One creature set on another: the guards at the gate on the thing
    // coming up the road. Any creature may be the attacker; the core decides
    // whether it can attack the target at all (friends cannot), and says so.
    Creature* attacker = FindCreature(args);
    Json target;
    target["guid"] = RequireString(args, "target");
    if (args.contains("map"))
        target["map"] = args["map"];
    Creature* victim = FindCreature(target);
    if (!attacker->IsAlive())
        throw BridgeCommandError("the attacker is dead");
    if (!victim->IsAlive())
        throw BridgeCommandError("the target is dead");
    if (attacker->GetMap() != victim->GetMap())
        throw BridgeCommandError("the two are on different maps");
    if (!attacker->CanAttack(victim))
        throw BridgeCommandError("'" + std::string(attacker->GetName()) + "' cannot attack '" + victim->GetName() + "' (friends, or one cannot be attacked)");
    if (!attacker->AI())
        throw BridgeCommandError("the attacker has no mind to fight with");
    attacker->AI()->AttackStart(victim);

    Json result;
    result["unit"] = UnitRef(attacker);
    result["target"] = UnitRef(victim);
    return result;
}

// npc.despawn -------------------------------------------------------------------

std::optional<Json> Bridge::CmdNpcDespawn(const BridgeInbound&, const Json& args)
{
    // A summon taken out of the world now, alive or a body. Only the bridge's
    // own: somebody who lives here is not this command's to remove.
    Creature* creature = FindCreature(args);
    if (!summons_.count(creature->GetObjectGuid()))
        throw BridgeCommandError("only a creature npc.summon made can be despawned");
    Json result;
    result["unit"] = UnitRef(creature);
    summons_.erase(creature->GetObjectGuid());
    creature->ForcedDespawn();
    return result;
}

// npc.find ----------------------------------------------------------------------

std::optional<Json> Bridge::CmdNpcFind(const BridgeInbound&, const Json& args)
{
    // The live creature of one entry nearest a point, from the spawn table:
    // a set piece names a resident (the Town Crier, a Night Watch guard) by
    // entry and place, and this finds the one standing there now. Only a
    // creature whose grid is loaded is found; one in a town nobody is near
    // is not in the world to speak.
    const uint32 entry = OptionalUnsigned(args, "entry", 0);
    if (!entry || !ObjectMgr::GetCreatureTemplate(entry))
        throw BridgeCommandError("argument 'entry' must be a creature template that exists");
    const std::vector<Player*> reals = RealPlayersOnline();
    Map* map = MapFor(args, reals);
    const float x = float(RequireNumber(args, "x"));
    const float y = float(RequireNumber(args, "y"));
    const float z = float(OptionalNumber(args, "z", 0.0));
    const float radius = float(OptionalNumber(args, "radius", 200.0));
    if (radius <= 0.0f || radius > 1000.0f)
        throw BridgeCommandError("argument 'radius' must be from 0 to 1000 yards");
    const uint32 most = OptionalUnsigned(args, "limit", 1);

    const WorldPosition at(map->GetId(), x, y, z);
    std::vector<std::pair<float, Creature*>> found;
    for (CreatureDataPair const* pair : at.getCreaturesNear(radius, entry))
    {
        if (pair->second.mapid != map->GetId())
            continue;
        Creature* creature = map->GetCreature(pair->first);
        if (!creature || !creature->IsAlive() || !creature->IsInWorld())
            continue;
        found.emplace_back(creature->GetDistance2d(x, y), creature);
    }
    if (found.empty())
        throw BridgeCommandError("nobody of that entry is alive and in the world near there");
    std::sort(found.begin(), found.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
    Json units = Json::array();
    for (auto const& [dist, creature] : found)
    {
        if (units.size() >= std::max(1u, most))
            break;
        Json unit = UnitRef(creature);
        unit["pos"] = PointJson(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ());
        unit["dist"] = dist;
        units.push_back(unit);
    }
    Json result;
    result["unit"] = units.front();
    result["units"] = units;
    return result;
}

// world.catalog -----------------------------------------------------------------

std::optional<Json> Bridge::CmdWorldCatalog(const BridgeInbound&, const Json& args)
{
    // What a set piece's script may name, for the brief a frontier model
    // writes it from: a script that names an entry or a place that is not
    // here is refused when it is loaded, so the model is given the list.
    //
    //   areas      the areas the game names round the player, each at a point
    //   creatures  every creature entry spawned round the player: residents
    //              and the wildlife alike, with the nearest spawn
    //   summoned   creatures the game has no spawn for anywhere, at the
    //              levels of the hostiles here: the ones only a script ever
    //              puts in the world (Stitches is one), the natural cast of a
    //              set piece
    //
    // One walk over the spawn table for the round, and one for which entries
    // are spawned anywhere. A brief is written once; this is not for a tick.
    const std::string name = OptionalString(args, "player");
    Player* player = nullptr;
    if (!name.empty())
        player = RequireOnlinePlayer(name, "player");
    else
    {
        std::vector<Player*> reals = RealPlayersOnline();
        if (reals.empty())
            throw BridgeCommandError("no real player is online; pass 'player'");
        player = reals.front();
    }
    const float radius = float(OptionalNumber(args, "radius", 1500.0));
    if (radius <= 0.0f || radius > 4000.0f)
        throw BridgeCommandError("argument 'radius' must be from 0 to 4000 yards");
    const WorldPosition here(player);
    FactionTemplateEntry const* ours = player->GetFactionTemplateEntry();
    auto hostile = [ours](CreatureInfo const* info) {
        FactionTemplateEntry const* ft = sFactionTemplateStore.LookupEntry(info->Faction);
        return ft && ours ? ft->IsHostileTo(*ours) : false;
    };

    Json areas = Json::array();
    std::vector<std::pair<float, Json>> named = AreasNear(player, radius);
    std::sort(named.begin(), named.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
    for (auto& [dist, place] : named)
    {
        place.erase("kind");
        areas.push_back(place);
    }

    // The creatures round about, one line per entry, nearest spawn first.
    struct Seen
    {
        uint32 spawns = 0;
        float dist = 0.0f;
        CreatureData const* nearest = nullptr;
    };
    std::unordered_map<uint32, Seen> seen;
    for (CreatureDataPair const* pair : here.getCreaturesNear(radius))
    {
        const CreatureData& data = pair->second;
        if (data.mapid != here.getMapId())
            continue;
        const WorldPosition at(data.mapid, data.posX, data.posY, data.posZ);
        const float dist = here.distance(at);
        Seen& s = seen[data.id];
        ++s.spawns;
        if (!s.nearest || dist < s.dist)
        {
            s.nearest = &data;
            s.dist = dist;
        }
    }
    std::vector<std::pair<float, uint32>> order;
    for (auto const& [entry, s] : seen)
        order.emplace_back(s.dist, entry);
    std::sort(order.begin(), order.end());
    uint32 low = 0, high = 0;
    Json creatures = Json::array();
    for (auto const& [dist, entry] : order)
    {
        CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(entry);
        if (!Castable(info))
            continue;
        const Seen& s = seen[entry];
        Json line = Template(info);
        line["hostile"] = hostile(info);
        line["guard"] = (info->ExtraFlags & CREATURE_EXTRA_FLAG_GUARD) != 0;
        line["spawns"] = s.spawns;
        line["pos"] = PointJson(s.nearest->posX, s.nearest->posY, s.nearest->posZ);
        line["area"] = WorldPosition(s.nearest->mapid, s.nearest->posX, s.nearest->posY, s.nearest->posZ).getAreaName(false, false);
        line["dist"] = dist;
        creatures.push_back(line);
        if (line["hostile"].get<bool>())
        {
            low = low ? std::min(low, info->MinLevel) : info->MinLevel;
            high = std::max(high, info->MaxLevel);
        }
    }

    // The creatures only a script puts in the world, at the hostiles' levels
    // here (a little above: a set piece may be a threat). Nobody hostile here
    // means no band, and no list.
    Json summoned = Json::array();
    if (high)
    {
        std::set<uint32> spawned;
        auto collect = [&spawned](CreatureDataPair const& pair) { spawned.insert(pair.second.id); return false; };
        sObjectMgr.DoCreatureData(collect);
        const uint32 from = low > 3 ? low - 3 : 1, to = high + 5;
        std::vector<CreatureInfo const*> found;
        for (uint32 id = 0; id < sCreatureStorage.GetMaxEntry(); ++id)
        {
            CreatureInfo const* info = sCreatureStorage.LookupEntry<CreatureInfo>(id);
            if (!Castable(info) || spawned.count(id) || !hostile(info) || info->NpcFlags)
                continue;
            if (info->MaxLevel < from || info->MinLevel > to)
                continue;
            found.push_back(info);
        }
        // The strongest first, so a long list keeps its threats.
        std::sort(found.begin(), found.end(), [](CreatureInfo const* a, CreatureInfo const* b) {
            const bool ae = a->Rank != CREATURE_ELITE_NORMAL, be = b->Rank != CREATURE_ELITE_NORMAL;
            return ae != be ? ae : a->MaxLevel > b->MaxLevel;
        });
        const size_t most = size_t(OptionalNumber(args, "summoned", 150.0));
        for (CreatureInfo const* info : found)
        {
            if (summoned.size() >= most)
                break;
            summoned.push_back(Template(info));
        }
    }

    Json result;
    result["player"] = PlayerRef(player);
    AddZone(player, result);
    result["radius"] = radius;
    result["areas"] = areas;
    result["creatures"] = creatures;
    result["summoned"] = summoned;
    if (high)
        result["band"] = Json::array({low, high});
    return result;
}

// The watch ---------------------------------------------------------------------

void Bridge::WatchSummons()
{
    for (auto it = summons_.begin(); it != summons_.end();)
    {
        const ObjectGuid guid = it->first;
        Summon& summon = it->second;
        Map* map = sMapMgr.FindMap(summon.map, 0);
        Creature* creature = map ? map->GetCreature(guid) : nullptr;
        if (!creature || !creature->IsInWorld())
        {
            // Its time ran out, or its body went: nothing left to watch.
            Json data;
            data["guid"] = GuidString(guid);
            Emit(EV_NPC_GONE, data);
            it = summons_.erase(it);
            continue;
        }
        ++it;

        if (!creature->IsAlive())
        {
            if (summon.alive)
            {
                summon.alive = false;
                summon.inCombat = false;
                summon.route.clear();
                Json data;
                data["unit"] = UnitRef(creature);
                data["pos"] = PointJson(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ());
                Emit(EV_NPC_DIED, data);
            }
            continue;
        }

        const bool fighting = creature->IsInCombat();
        if (fighting != summon.inCombat)
        {
            summon.inCombat = fighting;
            Json data;
            data["unit"] = UnitRef(creature);
            data["fighting"] = fighting;
            data["target"] = UnitRef(creature->GetVictim());
            data["pos"] = PointJson(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ());
            Emit(EV_NPC_COMBAT, data);
        }
        if (fighting || summon.route.empty())
            continue;

        const SummonPoint& next = summon.route.front();
        if (creature->GetDistance2d(next.x, next.y) <= ARRIVED_YARDS || summon.tries >= MAX_TRIES)
        {
            summon.route.erase(summon.route.begin());
            summon.tries = 0;
            if (summon.route.empty())
            {
                Json data;
                data["unit"] = UnitRef(creature);
                data["pos"] = PointJson(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ());
                Emit(EV_NPC_ARRIVED, data);
            }
            continue;   // the next leg goes out on the next tick
        }

        // A leg only while it stands: walking a leg already, or walking back
        // to where a fight began (the core's evade), is left to finish.
        const MovementGeneratorType now = creature->GetMotionMaster()->GetCurrentMovementGeneratorType();
        if (now == POINT_MOTION_TYPE || now == HOME_MOTION_TYPE)
            continue;
        ++summon.tries;
        creature->GetMotionMaster()->MovePoint(1, ::Position(next.x, next.y, next.z, 0.0f),
                                               summon.run ? FORCED_MOVEMENT_RUN : FORCED_MOVEMENT_WALK);
    }
}

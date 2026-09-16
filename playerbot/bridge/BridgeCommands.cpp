// Command side of the bridge. Every handler runs on the world thread, before
// the maps update, so it may touch any player or creature directly. A handler
// returns its result, or std::nullopt when it will reply later itself.
//
// Deliberately absent: a way to run server console commands from the socket.
// The bridge exposes scoped game actions instead: a line spoken as a player, a
// bot from the random-bot roster logged in and placed near someone, the weather
// in one zone. Nothing here takes a command string for the server.

#include "playerbot/bridge/Bridge.h"

#include "playerbot/PlayerbotAI.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/PlayerbotMgr.h"
#include "playerbot/RandomPlayerbotMgr.h"
#include "playerbot/playerbot.h"
#include "playerbot/strategy/Event.h"

#include "Entities/Creature.h"
#include "Entities/Player.h"
#include "Globals/ObjectAccessor.h"
#include "Globals/ObjectMgr.h"
#include "Maps/Map.h"
#include "Maps/MapManager.h"
#include "Database/DatabaseEnv.h"
#include "Server/DBCStores.h"
#include "Server/DBCStructure.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <sstream>

namespace
{
    uint32 ChatTypeFor(const std::string& channel)
    {
        if (channel == "whisper") return CHAT_MSG_WHISPER;
        if (channel == "party")   return CHAT_MSG_PARTY;
        if (channel == "say")     return CHAT_MSG_SAY;
        if (channel == "raid")    return CHAT_MSG_RAID;
        if (channel == "guild")   return CHAT_MSG_GUILD;
        throw BridgeCommandError("unknown channel '" + channel + "' (whisper, party, say, raid, guild)");
    }
}

// Helpers ----------------------------------------------------------------------

std::string Bridge::RequireString(const Json& args, const char* key)
{
    if (!args.contains(key) || !args[key].is_string() || args[key].get<std::string>().empty())
        throw BridgeCommandError(std::string("argument '") + key + "' must be a non-empty string");
    return args[key];
}

std::string Bridge::OptionalString(const Json& args, const char* key, const std::string& fallback)
{
    if (!args.contains(key) || args[key].is_null())
        return fallback;
    if (!args[key].is_string())
        throw BridgeCommandError(std::string("argument '") + key + "' must be a string");
    return args[key];
}

Player* Bridge::FindOnlinePlayer(const std::string& rawName)
{
    std::string name = rawName;
    if (!normalizePlayerName(name))
        return nullptr;
    Player* player = ObjectAccessor::FindPlayerByName(name.c_str());
    return player && player->IsInWorld() ? player : nullptr;
}

// Speech never runs commands. A line handed to player.say enters the
// session as typed input and the core's chat handler parses commands before
// it broadcasts speech; bot.say goes through the bot's session the same way.
// So text that begins with a command prefix is refused here, whoever asks,
// before it reaches any session. Type game commands in the game.
static void RequireSpeech(const std::string& text)
{
    const size_t first = text.find_first_not_of(" \t");
    if (first != std::string::npos && (text[first] == '.' || text[first] == '!'))
        throw BridgeCommandError("text begins with a command prefix; the bridge speaks, it does not run commands");
}

Player* Bridge::RequireOnlinePlayer(const std::string& name, const char* what)
{
    if (Player* player = FindOnlinePlayer(name))
        return player;
    throw BridgeCommandError(std::string(what) + " '" + name + "' is not online");
}

Player* Bridge::RequireBot(const std::string& name)
{
    Player* player = RequireOnlinePlayer(name, "bot");
    PlayerbotAI* ai = player->GetPlayerbotAI();
    if (!ai || ai->IsRealPlayer())
        throw BridgeCommandError("'" + name + "' is not a bot");
    return player;
}

std::vector<Player*> Bridge::RealPlayersOnline()
{
    std::vector<Player*> reals;
    for (auto const& entry : sObjectAccessor.GetPlayers())
    {
        Player* player = entry.second;
        if (!player || !player->IsInWorld())
            continue;
        PlayerbotAI* ai = player->GetPlayerbotAI();
        if (!ai || ai->IsRealPlayer())
            reals.push_back(player);
    }
    return reals;
}

Creature* Bridge::FindCreature(const Json& args)
{
    const std::string guidText = RequireString(args, "guid");
    uint64 raw = 0;
    try
    {
        raw = std::stoull(guidText);
    }
    catch (std::exception const&)
    {
        throw BridgeCommandError("argument 'guid' must be a decimal string");
    }
    const ObjectGuid guid(raw);
    if (!guid.IsCreatureOrPet())
        throw BridgeCommandError("argument 'guid' is not a creature guid");

    if (args.contains("map"))
    {
        if (!args["map"].is_number_unsigned())
            throw BridgeCommandError("argument 'map' must be an unsigned integer");
        const uint32 instance = args.contains("instance") && args["instance"].is_number_unsigned() ? args["instance"].get<uint32>() : 0u;
        Map* map = sMapMgr.FindMap(args["map"].get<uint32>(), instance);
        if (!map)
            throw BridgeCommandError("map is not loaded");
        if (Creature* creature = map->GetCreature(guid))
            return creature;
        throw BridgeCommandError("creature is not in the world");
    }

    for (Player* real : RealPlayersOnline())
        if (Creature* creature = real->GetMap()->GetCreature(guid))
            return creature;
    throw BridgeCommandError("creature not found on any real player's map; pass 'map'");
}

// Handlers ---------------------------------------------------------------------

std::optional<Json> Bridge::CmdBotList(const BridgeInbound&, const Json&)
{
    Json bots = Json::array();
    for (auto const& entry : sObjectAccessor.GetPlayers())
    {
        Player* player = entry.second;
        if (!player || !player->IsInWorld())
            continue;
        PlayerbotAI* ai = player->GetPlayerbotAI();
        if (!ai || ai->IsRealPlayer())
            continue;
        Json bot;
        bot["unit"] = PlayerRef(player);
        Player* master = ai->GetMaster();
        bot["master"] = master ? Json(master->GetName()) : Json(nullptr);
        bot["has_real_master"] = ai->HasRealPlayerMaster();
        bots.push_back(bot);
    }
    Json result;
    result["bots"] = bots;
    return result;
}

std::optional<Json> Bridge::CmdBotAdd(const BridgeInbound&, const Json& args)
{
    Player* master = RequireOnlinePlayer(RequireString(args, "master"), "master");
    PlayerbotMgr* mgr = master->GetPlayerbotMgr();
    if (!mgr)
        throw BridgeCommandError("master cannot control bots (no bot manager on that player)");
    // Same path as the in-game ".bot add" command, ownership rules included.
    std::string name = RequireString(args, "bot");
    std::list<std::string> messages = mgr->HandlePlayerbotCommand("add " + name, master);
    // For a character on a random-bot or cast account the module's add only
    // logs it in; it stands in the world with no master until something puts
    // it in the player's group. The first live session found Ansel like that.
    // The bridge finishes the job on the world tick, once the character is
    // in the world with its AI: master set, group joined, bot.login announced.
    if (normalizePlayerName(name))
    {
        const ObjectGuid guid = sObjectMgr.GetPlayerGuidByName(name);
        if (!guid.IsEmpty() && sPlayerbotAIConfig.IsInRandomAccountList(sObjectMgr.GetPlayerAccountIdByGUID(guid)))
            pendingAttach_[guid] = std::make_pair(master->GetObjectGuid(), WorldTimer::getMSTime() + 30000);
    }
    Json result;
    result["messages"] = messages;
    return result;
}

void Bridge::FlushPendingAttach()
{
    if (pendingAttach_.empty())
        return;
    const uint32 now = WorldTimer::getMSTime();
    for (auto it = pendingAttach_.begin(); it != pendingAttach_.end();)
    {
        const ObjectGuid guid = it->first;
        Player* master = sObjectMgr.GetPlayer(it->second.first);
        Player* bot = sObjectMgr.GetPlayer(guid);
        PlayerbotAI* ai = bot ? bot->GetPlayerbotAI() : nullptr;
        if (!master || !master->IsInWorld())
        {
            it = pendingAttach_.erase(it);
            continue;
        }
        if (!bot || !bot->IsInWorld() || !ai)
        {
            if (int32(it->second.second - now) < 0)
            {
                sLog.outString("Bridge: gave up waiting for %s to log in for %s", GuidString(guid).c_str(), master->GetName());
                it = pendingAttach_.erase(it);
            }
            else
                ++it;
            continue;
        }
        // The same three steps a pool bot goes through when it accepts a
        // real player's group invitation.
        if (ai->GetMaster() != master)
        {
            ai->SetMaster(master);
            ai->ResetStrategies();
        }
        if (!bot->GetGroup() || bot->GetGroup() != master->GetGroup())
            ai->DoSpecificAction("join", Event("bridge", "", master));
        sLog.outString("Bridge: %s is now %s's companion", bot->GetName(), master->GetName());
        EmitBotLogin(bot);   // it has a master now, so this is a client's business
        it = pendingAttach_.erase(it);
    }
}

std::optional<Json> Bridge::CmdBotRemove(const BridgeInbound&, const Json& args)
{
    Player* master = RequireOnlinePlayer(RequireString(args, "master"), "master");
    PlayerbotMgr* mgr = master->GetPlayerbotMgr();
    if (!mgr)
        throw BridgeCommandError("master cannot control bots (no bot manager on that player)");
    std::list<std::string> messages = mgr->HandlePlayerbotCommand("remove " + RequireString(args, "bot"), master);
    Json result;
    result["messages"] = messages;
    return result;
}

std::optional<Json> Bridge::CmdBotCommand(const BridgeInbound&, const Json& args)
{
    Player* bot = RequireBot(RequireString(args, "bot"));
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    const std::string text = RequireString(args, "text");
    const std::string fromName = OptionalString(args, "from");
    Player* from = fromName.empty() ? ai->GetMaster() : RequireOnlinePlayer(fromName, "from");
    if (!from)
        throw BridgeCommandError("bot has no master; pass 'from'");
    const uint32 type = ChatTypeFor(OptionalString(args, "channel", "whisper"));
    // Queued on the bot; it runs on the bot's next AI update with the module's
    // own security checks against 'from'.
    ai->HandleCommand(type, text, *from);
    Json result;
    result["queued"] = true;
    return result;
}

std::optional<Json> Bridge::CmdBotSay(const BridgeInbound&, const Json& args)
{
    Player* bot = RequireBot(RequireString(args, "bot"));
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    const std::string text = RequireString(args, "text");
    RequireSpeech(text);
    const std::string channel = OptionalString(args, "channel", "say");
    // likePlayer = true sends real chat through the bot's session, so other
    // players and bots hear it and the bridge sees it come back as a chat event.
    if (channel == "say")
        ai->Say(text, true);
    else if (channel == "yell")
        ai->Yell(text, true);
    else if (channel == "party")
        ai->SayToParty(text, true);
    else if (channel == "guild")
        ai->SayToGuild(text, true);
    else if (channel == "whisper")
        ai->Whisper(text, RequireString(args, "to"), true);
    else if (channel == "emote")
        bot->TextEmote(text);
    else
        throw BridgeCommandError("unknown channel '" + channel + "' (say, yell, party, guild, whisper, emote)");
    return Json::object();
}

std::optional<Json> Bridge::CmdNpcSay(const BridgeInbound&, const Json& args)
{
    Creature* creature = FindCreature(args);
    const std::string text = RequireString(args, "text");
    RequireSpeech(RequireString(args, "text"));
    const std::string kind = OptionalString(args, "kind", "say");
    const std::string to = OptionalString(args, "to");
    Player* target = to.empty() ? nullptr : RequireOnlinePlayer(to, "to");

    if (kind == "say")
        creature->MonsterSay(text.c_str(), LANG_UNIVERSAL, target);
    else if (kind == "yell")
        creature->MonsterYell(text.c_str(), LANG_UNIVERSAL, target);
    else if (kind == "emote")
        creature->MonsterTextEmote(text.c_str(), target);
    else if (kind == "whisper")
    {
        if (!target)
            throw BridgeCommandError("a whisper needs 'to'");
        creature->MonsterWhisper(text.c_str(), target);
    }
    else
        throw BridgeCommandError("unknown kind '" + kind + "' (say, yell, emote, whisper)");

    Json result;
    result["unit"] = UnitRef(creature);
    return result;
}

std::optional<Json> Bridge::CmdNpcEmote(const BridgeInbound&, const Json& args)
{
    Creature* creature = FindCreature(args);
    if (!args.contains("emote_id") || !args["emote_id"].is_number_unsigned())
        throw BridgeCommandError("argument 'emote_id' must be an unsigned integer");
    creature->HandleEmote(args["emote_id"].get<uint32>());
    Json result;
    result["unit"] = UnitRef(creature);
    return result;
}

std::optional<Json> Bridge::CmdSceneGet(const BridgeInbound&, const Json& args)
{
    const std::string centerName = OptionalString(args, "center");
    Player* center = nullptr;
    if (!centerName.empty())
        center = RequireOnlinePlayer(centerName, "center");
    else
    {
        std::vector<Player*> reals = RealPlayersOnline();
        if (reals.empty())
            throw BridgeCommandError("no real player is online; pass 'center'");
        center = reals.front();
    }
    return BuildScene(center);
}

// Cast --------------------------------------------------------------------------
//
// Bots as the control center's cast: pick a character from the random-bot
// roster, log it in, place it near a player, hold it still, let it speak
// (bot.say) and log it out again. Login is asynchronous in the core, so
// bot.login only queues the character load and the bot.login event says when
// it stands in the world. Logout is immediate.

namespace
{
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

    bool SameName(const std::string& a, const char* b)
    {
        if (!b || a.size() != std::strlen(b))
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(uint8(a[i])) != std::tolower(uint8(b[i])))
                return false;
        return true;
    }

    // A race or class given by id or by its DBC name ("Night Elf", "warrior").
    uint32 RaceId(const Json& value)
    {
        if (value.is_number_unsigned())
            return value.get<uint32>();
        if (value.is_string())
            for (uint32 i = 0; i < sChrRacesStore.GetNumRows(); ++i)
                if (ChrRacesEntry const* entry = sChrRacesStore.LookupEntry(i))
                    if (SameName(value.get<std::string>(), entry->name[0]))
                        return i;
        throw BridgeCommandError("unknown race");
    }

    uint32 ClassId(const Json& value)
    {
        if (value.is_number_unsigned())
            return value.get<uint32>();
        if (value.is_string())
            for (uint32 i = 0; i < sChrClassesStore.GetNumRows(); ++i)
                if (ChrClassesEntry const* entry = sChrClassesStore.LookupEntry(i))
                    if (SameName(value.get<std::string>(), entry->name[0]))
                        return i;
        throw BridgeCommandError("unknown class");
    }

    std::string RacesOfTeam(Team team)
    {
        std::ostringstream list;
        for (uint32 i = 0; i < sChrRacesStore.GetNumRows(); ++i)
            if (sChrRacesStore.LookupEntry(i) && Player::TeamForRace(uint8(i)) == team)
                list << (list.tellp() > 0 ? "," : "") << i;
        return list.str();
    }

    BotState StateFor(const std::string& name)
    {
        if (name == "combat" || name == "co")
            return BotState::BOT_STATE_COMBAT;
        if (name == "non combat" || name == "nc")
            return BotState::BOT_STATE_NON_COMBAT;
        if (name == "dead")
            return BotState::BOT_STATE_DEAD;
        if (name == "reaction" || name == "react")
            return BotState::BOT_STATE_REACTION;
        if (name == "all")
            return BotState::BOT_STATE_ALL;
        throw BridgeCommandError("unknown state '" + name + "' (combat, non combat, dead, reaction, all)");
    }
}

std::optional<Json> Bridge::CmdBotRoster(const BridgeInbound&, const Json& args)
{
    // The pool by default; with cast=true, only the characters bot.create made.
    // The two never mix: a made-to-order figure is not a walk-on.
    const bool wantCast = OptionalBool(args, "cast", false);
    std::list<uint32> accounts;
    for (uint32 accountId : sPlayerbotAIConfig.randomBotAccounts)
        if (sPlayerbotAIConfig.IsInCastAccountList(accountId) == wantCast)
            accounts.push_back(accountId);
    if (accounts.empty())
    {
        if (wantCast)
        {
            Json empty;
            empty["bots"] = Json::array();
            return empty;
        }
        throw BridgeCommandError("no random bot accounts exist; the module creates them on first start");
    }

    std::ostringstream where;
    where << "account IN (";
    for (auto it = accounts.begin(); it != accounts.end(); ++it)
        where << (it == accounts.begin() ? "" : ",") << *it;
    where << ")";
    if (uint32 minLevel = OptionalUnsigned(args, "level_min", 0))
        where << " AND level >= " << minLevel;
    if (uint32 maxLevel = OptionalUnsigned(args, "level_max", 0))
        where << " AND level <= " << maxLevel;
    if (args.contains("race") && !args["race"].is_null())
        where << " AND race = " << RaceId(args["race"]);
    if (args.contains("class") && !args["class"].is_null())
        where << " AND class = " << ClassId(args["class"]);
    const std::string team = OptionalString(args, "team");
    if (team == "alliance" || team == "horde")
        where << " AND race IN (" << RacesOfTeam(team == "alliance" ? ALLIANCE : HORDE) << ")";
    else if (!team.empty())
        throw BridgeCommandError("argument 'team' must be alliance or horde");
    if (args.contains("online") && !args["online"].is_null())
        where << " AND online = " << (OptionalBool(args, "online", false) ? 1 : 0);
    const uint32 limit = std::min(std::max(OptionalUnsigned(args, "limit", 50), 1u), 200u);

    // One synchronous read, the way the module reads its own bot lists.
    auto rows = CharacterDatabase.PQuery(
        "SELECT guid, name, race, class, level, zone, map, online, gender FROM characters WHERE %s ORDER BY RAND() LIMIT %u",
        where.str().c_str(), limit);
    Json bots = Json::array();
    if (rows)
    {
        do
        {
            Field* fields = rows->Fetch();
            const uint32 counter = fields[0].GetUInt32();
            const uint32 race = fields[2].GetUInt8();
            const uint32 cls = fields[3].GetUInt8();
            const uint32 zone = fields[5].GetUInt32();
            Json bot;
            bot["guid"] = GuidString(ObjectGuid(HIGHGUID_PLAYER, counter));
            bot["name"] = fields[1].GetCppString();
            bot["race_id"] = race;
            bot["class_id"] = cls;
            bot["level"] = fields[4].GetUInt32();
            bot["zone"] = zone;
            bot["map"] = fields[6].GetUInt32();
            bot["online"] = fields[7].GetUInt8() != 0 || sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, counter)) != nullptr;
            // So a control center writing lines for this character gets the pronouns right.
            bot["gender"] = fields[8].GetUInt8() == 0 ? "male" : "female";
            if (ChrRacesEntry const* entry = sChrRacesStore.LookupEntry(race))
                bot["race"] = entry->name[0];
            if (ChrClassesEntry const* entry = sChrClassesStore.LookupEntry(cls))
                bot["class"] = entry->name[0];
            if (AreaTableEntry const* entry = GetAreaEntryByAreaID(zone))
                bot["zone_name"] = entry->area_name[0];
            bots.push_back(bot);
        }
        while (rows->NextRow());
    }
    Json result;
    result["bots"] = bots;
    return result;
}

std::optional<Json> Bridge::CmdBotLogin(const BridgeInbound&, const Json& args)
{
    std::string name = RequireString(args, "name");
    if (!normalizePlayerName(name))
        throw BridgeCommandError("'" + name + "' is not a valid character name");

    Json result;
    if (Player* online = FindOnlinePlayer(name))
    {
        PlayerbotAI* ai = online->GetPlayerbotAI();
        if (!ai || ai->IsRealPlayer())
            throw BridgeCommandError("'" + name + "' is a real player");
        result["queued"] = false;
        result["unit"] = PlayerRef(online);
        return result;
    }

    const ObjectGuid guid = sObjectMgr.GetPlayerGuidByName(name);
    if (guid.IsEmpty())
        throw BridgeCommandError("no character named '" + name + "'");
    if (!sPlayerbotAIConfig.IsInRandomAccountList(sObjectMgr.GetPlayerAccountIdByGUID(guid)))
        throw BridgeCommandError("'" + name + "' is not on a random bot account (see bot.roster)");

    // The module's own random-bot login. The character loads asynchronously;
    // the bot.login event says when it stands in the world.
    requestedLogins_.insert(guid);   // announce this one's arrival even though it has no master
    if (!sRandomPlayerbotMgr.LoginRandomBot(guid.GetCounter()))
    {
        requestedLogins_.erase(guid);
        throw BridgeCommandError("the module refused to log in '" + name + "'");
    }
    result["queued"] = true;
    result["guid"] = GuidString(guid);
    result["name"] = name;
    return result;
}

std::optional<Json> Bridge::CmdBotLogout(const BridgeInbound&, const Json& args)
{
    Player* bot = RequireBot(RequireString(args, "bot"));
    const uint32 counter = bot->GetGUIDLow();
    Json result;
    result["unit"] = PlayerRef(bot);   // built before the player object goes away

    if (sRandomPlayerbotMgr.GetPlayerBot(counter))
        sRandomPlayerbotMgr.LogoutRandomBot(counter);          // masterless: forget its timers so it can come back later
    else
    {
        Player* master = bot->GetPlayerbotAI()->GetMaster();
        PlayerbotMgr* mgr = master ? master->GetPlayerbotMgr() : nullptr;
        if (!mgr || !mgr->GetPlayerBot(counter))
            throw BridgeCommandError(std::string("no bot holder owns '") + bot->GetName() + "'");
        mgr->LogoutPlayerBot(counter);
    }
    return result;
}

std::optional<Json> Bridge::CmdBotPlace(const BridgeInbound&, const Json& args)
{
    Player* bot = RequireBot(RequireString(args, "bot"));
    uint32 map;
    float x, y, z, o;

    const std::string nearName = OptionalString(args, "near");
    if (!nearName.empty())
    {
        Player* anchor = RequireOnlinePlayer(nearName, "near");
        const float distance = float(OptionalNumber(args, "distance", 4.0));
        const float angle = float(OptionalNumber(args, "angle", 0.0)) * M_PI_F / 180.0f;   // from where the anchor faces; 0 = in front
        // No searcher: the core's near-point search clamps the height on the
        // searcher's own map, and a bot still on another continent (or not yet
        // in the world) gets that continent's ground under this one's sky. Two
        // strangers were put seventy yards under Northshire that way. Without a
        // searcher the ground is read from the anchor's map, the one that matters.
        anchor->GetClosePoint(x, y, z, bot->GetObjectBoundingRadius(), distance, angle);
        map = anchor->GetMapId();
        if (z <= INVALID_HEIGHT || std::abs(z - anchor->GetPositionZ()) > 15.0f)
            z = anchor->GetPositionZ();   // a bad lookup is worse than the anchor's own height
        o = MapManager::NormalizeOrientation(std::atan2(anchor->GetPositionY() - y, anchor->GetPositionX() - x));   // facing the anchor
    }
    else
    {
        map = OptionalUnsigned(args, "map", bot->GetMapId());
        x = float(RequireNumber(args, "x"));
        y = float(RequireNumber(args, "y"));
        z = float(RequireNumber(args, "z"));
        o = float(OptionalNumber(args, "o", bot->GetOrientation()));
        if (!MapManager::IsValidMapCoord(map, x, y, z, o))
            throw BridgeCommandError("not a valid position");
    }

    if (!bot->TeleportTo(map, x, y, z, o))
        throw BridgeCommandError("the server refused the teleport");

    Json pos;
    pos["map"] = map;
    pos["x"] = x;
    pos["y"] = y;
    pos["z"] = z;
    pos["o"] = o;
    Json result;
    result["unit"] = PlayerRef(bot);
    result["map"] = map;
    result["pos"] = pos;
    return result;
}

std::optional<Json> Bridge::CmdBotStrategy(const BridgeInbound&, const Json& args)
{
    Player* bot = RequireBot(RequireString(args, "bot"));
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    const std::string change = OptionalString(args, "change");
    if (!change.empty())
        ai->ChangeStrategy(change, StateFor(OptionalString(args, "state", "non combat")));   // "+stay,-rpg" and so on

    static const std::pair<const char*, BotState> states[] = {
        {"combat", BotState::BOT_STATE_COMBAT},
        {"non combat", BotState::BOT_STATE_NON_COMBAT},
        {"dead", BotState::BOT_STATE_DEAD},
        {"reaction", BotState::BOT_STATE_REACTION},
    };
    Json strategies;
    for (auto const& entry : states)
    {
        Json list = Json::array();
        for (std::string_view name : ai->GetStrategies(entry.second))
            list.push_back(std::string(name));
        strategies[entry.first] = list;
    }
    Json result;
    result["unit"] = PlayerRef(bot);
    result["strategies"] = strategies;
    return result;
}

std::optional<Json> Bridge::CmdPlayerSay(const BridgeInbound&, const Json& args)
{
    Player* player = RequireOnlinePlayer(RequireString(args, "player"), "player");
    const std::string text = RequireString(args, "text");
    RequireSpeech(text);
    const std::string channel = OptionalString(args, "channel", "say");
    uint32 type;
    if (channel == "say")          type = CHAT_MSG_SAY;
    else if (channel == "yell")    type = CHAT_MSG_YELL;
    else if (channel == "emote")   type = CHAT_MSG_EMOTE;
    else if (channel == "party")   type = CHAT_MSG_PARTY;
    else if (channel == "raid")    type = CHAT_MSG_RAID;
    else if (channel == "guild")   type = CHAT_MSG_GUILD;
    else if (channel == "whisper") type = CHAT_MSG_WHISPER;
    else
        throw BridgeCommandError("unknown channel '" + channel + "' (say, yell, emote, party, raid, guild, whisper)");

    // The line enters the player's session as if typed in the client: the
    // chat handler, the bots' command parsing and the bridge's own chat event
    // all follow on the session's next update.
    WorldPacket packet(CMSG_MESSAGECHAT);
    packet << uint32(type) << uint32(LANG_UNIVERSAL);
    if (type == CHAT_MSG_WHISPER)
        packet << RequireString(args, "to");
    packet << text;
    player->GetSession()->QueuePacket(std::unique_ptr<WorldPacket>(new WorldPacket(packet)));

    Json result;
    result["queued"] = true;
    return result;
}

std::optional<Json> Bridge::CmdWeather(const BridgeInbound&, const Json& args)
{
    const std::string typeName = OptionalString(args, "type", "fine");
    WeatherType type;
    if (typeName == "fine")       type = WEATHER_TYPE_FINE;
    else if (typeName == "rain")  type = WEATHER_TYPE_RAIN;
    else if (typeName == "snow")  type = WEATHER_TYPE_SNOW;
    else if (typeName == "storm") type = WEATHER_TYPE_STORM;
    else
        throw BridgeCommandError("unknown weather '" + typeName + "' (fine, rain, snow, storm)");
    const float grade = float(std::min(std::max(OptionalNumber(args, "grade", type == WEATHER_TYPE_FINE ? 0.0 : 0.5), 0.0), 1.0));
    const bool permanent = OptionalBool(args, "permanent", false);

    Map* map = nullptr;
    uint32 zone = 0;
    if (args.contains("zone") && !args["zone"].is_null())
    {
        zone = OptionalUnsigned(args, "zone", 0);
        map = sMapMgr.FindMap(OptionalUnsigned(args, "map", 0), OptionalUnsigned(args, "instance", 0));
        if (!map)
            throw BridgeCommandError("that map is not loaded");
    }
    else
    {
        const std::string playerName = OptionalString(args, "player");
        Player* player = nullptr;
        if (!playerName.empty())
            player = RequireOnlinePlayer(playerName, "player");
        else
        {
            std::vector<Player*> reals = RealPlayersOnline();
            if (reals.empty())
                throw BridgeCommandError("no real player is online; pass 'player', or 'map' and 'zone'");
            player = reals.front();
        }
        map = player->GetMap();
        zone = player->GetZoneId();
    }

    // The core's own zone weather: everyone in that zone on this map sees the
    // change at once. Unless permanent, the regular weather cycle resumes later.
    map->SetWeather(zone, type, grade, permanent);

    Json result;
    result["map"] = map->GetId();
    result["zone"] = zone;
    if (AreaTableEntry const* entry = GetAreaEntryByAreaID(zone))
        result["zone_name"] = entry->area_name[0];
    result["type"] = typeName;
    result["grade"] = grade;
    result["permanent"] = permanent;
    return result;
}

// Cast characters ----------------------------------------------------------------
//
// bot.create makes a character to order: a name, a race, a class, a gender, a
// level, and optionally a talent path, a gear quality and a look. It goes on a
// cast account (AiPlayerbot.Bridge.CastAccountPrefix), which counts as a
// random-bot account for the module's ownership rules - a real player may add
// it, bot.login may log it in - but which the random-bot manager leaves alone:
// no re-rolled level, no wandering teleport, no timed logout, no login of its
// own accord. The character is created offline the way the module's own
// `.bot create` does it, with the core's name rules enforced first; its spells
// and gear for the level are finished the first time it stands in the world,
// before the bot.login event announces it.
//
// This is still not a console. The only thing it can make is a player
// character on an account the module owns, at or below the realm's level cap.

#include "playerbot/ChatHelper.h"
#include "playerbot/PlayerbotFactory.h"
#include "playerbot/Talentspec.h"
#include "playerbot/strategy/actions/ChangeTalentsAction.h"

#include "Accounts/AccountMgr.h"
#include "World/World.h"

#include <cctype>
#include <random>

using namespace ai;   // TalentPath, ChatHelper, ChangeTalentsAction, BotRoles

namespace
{
    uint32 MaxCharactersPerAccount()
    {
#ifdef MANGOSBOT_TWO
        return 10;
#else
        return 9;
#endif
    }

    std::string Lower(std::string text)
    {
        for (char& c : text)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    bool AllDigits(const std::string& text)
    {
        if (text.empty())
            return false;
        for (char c : text)
            if (!std::isdigit(static_cast<unsigned char>(c)))
                return false;
        return true;
    }

    std::string RandomPassword()
    {
        static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
        std::string password;
        for (int i = 0; i < 16; ++i)
            password += alphabet[urand(0, sizeof(alphabet) - 2)];
        return password;
    }

    // The module's premade talent paths for a class, by name. "affliction"
    // finds "pve dps affli" the way a person would: an exact name first, then
    // a path whose name contains the query, then a path with a word the query
    // begins with. PvE paths are listed first in the config, so they win ties.
    TalentPath* FindTalentPath(uint8 cls, const std::string& query)
    {
        std::vector<TalentPath>& paths = sPlayerbotAIConfig.classSpecs[cls].talentPath;
        const std::string q = Lower(query);
        for (TalentPath& path : paths)
            if (Lower(path.name) == q)
                return &path;
        for (TalentPath& path : paths)
            if (Lower(path.name).find(q) != std::string::npos)
                return &path;
        for (TalentPath& path : paths)
        {
            std::istringstream words(Lower(path.name));
            std::string word;
            while (words >> word)
                if (word.size() >= 4 && q.rfind(word, 0) == 0)
                    return &path;
        }
        return nullptr;
    }

    std::string TalentPathNames(uint8 cls)
    {
        std::string names;
        for (TalentPath& path : sPlayerbotAIConfig.classSpecs[cls].talentPath)
            names += (names.empty() ? "" : ", ") + path.name;
        return names.empty() ? std::string("none configured") : names;
    }

    uint32 GearQuality(const std::string& gear)
    {
        if (gear == "green" || gear == "uncommon") return ITEM_QUALITY_UNCOMMON;
        if (gear == "blue" || gear == "rare")      return ITEM_QUALITY_RARE;
        if (gear == "purple" || gear == "epic")    return ITEM_QUALITY_EPIC;
        throw BridgeCommandError("argument 'gear' must be green, blue or purple");
    }

    struct Look
    {
        uint8 skin = 0, face = 0, hairStyle = 0, hairColor = 0, facialHair = 0;
    };

    // The same choice the module makes for its random pool, from the client's
    // own appearance tables, but from a seed: the same seed gives the same
    // face, so a character that is ever remade looks like itself.
    Look PickLook(uint8 race, uint8 gender, uint32 seed)
    {
        std::vector<uint8> facialHairs;
        std::vector<std::pair<uint8, uint8>> faces, hairs;   // variation, colour
        for (CharSectionsMap::const_iterator itr = sCharSectionMap.begin(); itr != sCharSectionMap.end(); ++itr)
        {
            CharSectionsEntry const* entry = itr->second;
            if (entry->Race != race || entry->Gender != gender)
                continue;
#ifndef MANGOSBOT_TWO
            const uint8 colour = uint8(entry->ColorIndex);
#else
            const uint8 colour = uint8(entry->Color);
#endif
            switch (entry->BaseSection)
            {
                case SECTION_TYPE_FACE:        faces.emplace_back(uint8(entry->VariationIndex), colour); break;
                case SECTION_TYPE_FACIAL_HAIR: facialHairs.push_back(colour); break;
                case SECTION_TYPE_HAIR:        hairs.emplace_back(uint8(entry->VariationIndex), colour); break;
                default: break;
            }
        }
        std::mt19937 rng(seed);
        auto pick = [&rng](size_t count) -> size_t
        {
            return count ? std::uniform_int_distribution<size_t>(0, count - 1)(rng) : 0;
        };
        Look look;
        if (!faces.empty())
        {
            const auto& face = faces[pick(faces.size())];
            look.face = face.first;
            look.skin = face.second;   // a face texture belongs to one skin colour; the module pairs them the same way
        }
        if (!hairs.empty())
        {
            const auto& hair = hairs[pick(hairs.size())];
            look.hairStyle = hair.first;
            look.hairColor = hair.second;
        }
        const bool noFacialHair = race == RACE_TAUREN || (gender == GENDER_FEMALE && race != RACE_NIGHTELF && race != RACE_UNDEAD);
#ifndef MANGOSBOT_TWO
        if (!noFacialHair && !facialHairs.empty())
            look.facialHair = facialHairs[pick(facialHairs.size())];
#endif
        return look;
    }
}

void Bridge::RegisterCastAccount(uint32 accountId)
{
    PlayerbotAIConfig& config = sPlayerbotAIConfig;
    if (!config.IsInCastAccountList(accountId))
        config.castBotAccounts.push_back(accountId);
    if (!config.IsInRandomAccountList(accountId))
        config.randomBotAccounts.push_back(accountId);
}

void Bridge::LoadCastAccounts()
{
    PlayerbotAIConfig& config = sPlayerbotAIConfig;
    config.castBotAccounts.clear();

    std::string prefix = config.bridgeCastAccountPrefix;
    std::string randomPrefix = config.randomBotAccountPrefix;
    AccountMgr::normalizeString(prefix);
    AccountMgr::normalizeString(randomPrefix);
    if (prefix.empty() || prefix.rfind(randomPrefix, 0) == 0 || randomPrefix.rfind(prefix, 0) == 0)
    {
        sLog.outError("Bridge: AiPlayerbot.Bridge.CastAccountPrefix ('%s') must differ from AiPlayerbot.RandomBotAccountPrefix ('%s'); bot.create is off",
                      config.bridgeCastAccountPrefix.c_str(), config.randomBotAccountPrefix.c_str());
        return;
    }

    std::string pattern = prefix;
    LoginDatabase.escape_string(pattern);
    auto rows = LoginDatabase.PQuery("SELECT id, username FROM account WHERE username LIKE '%s%%'", pattern.c_str());
    if (!rows)
        return;
    do
    {
        Field* fields = rows->Fetch();
        const uint32 accountId = fields[0].GetUInt32();
        std::string username = fields[1].GetCppString();
        AccountMgr::normalizeString(username);
        if (username.size() <= prefix.size() || !AllDigits(username.substr(prefix.size())))
            continue;   // some other account that happens to start the same way
        RegisterCastAccount(accountId);
    }
    while (rows->NextRow());
    if (!config.castBotAccounts.empty())
        sLog.outString("Bridge: %zu cast account(s) loaded", config.castBotAccounts.size());
}

uint32 Bridge::GetOrCreateCastAccount(std::string& error)
{
    PlayerbotAIConfig& config = sPlayerbotAIConfig;
    std::string prefix = config.bridgeCastAccountPrefix;
    std::string randomPrefix = config.randomBotAccountPrefix;
    AccountMgr::normalizeString(prefix);
    AccountMgr::normalizeString(randomPrefix);
    if (prefix.empty() || prefix.rfind(randomPrefix, 0) == 0 || randomPrefix.rfind(prefix, 0) == 0)
    {
        error = "AiPlayerbot.Bridge.CastAccountPrefix must differ from AiPlayerbot.RandomBotAccountPrefix";
        return 0;
    }

    for (uint32 number = 0; number < 1000; ++number)
    {
        const std::string accountName = prefix + std::to_string(number);
        uint32 accountId = sAccountMgr.GetId(accountName);
        if (!accountId)
        {
            LoginDatabase.BeginTransaction();
#ifndef MANGOSBOT_ZERO
            AccountOpResult result = sAccountMgr.CreateAccount(accountName, RandomPassword(), MAX_EXPANSION);
#else
            AccountOpResult result = sAccountMgr.CreateAccount(accountName, RandomPassword());
#endif
            LoginDatabase.CommitTransactionDirect();
            accountId = result == AOR_OK ? sAccountMgr.GetId(accountName) : 0;
            if (!accountId)
            {
                error = "could not create cast account " + accountName;
                return 0;
            }
            sLog.outString("Bridge: created cast account %s", accountName.c_str());
        }
        RegisterCastAccount(accountId);
        if (sAccountMgr.GetCharactersCount(accountId) < MaxCharactersPerAccount())
            return accountId;
    }
    error = "every cast account is full";
    return 0;
}

std::optional<Json> Bridge::CmdBotCreate(const BridgeInbound&, const Json& args)
{
    // The name, by the core's rules, before anything is touched.
    std::string name = RequireString(args, "name");
    if (!normalizePlayerName(name) || ObjectMgr::CheckPlayerName(name, true) != CHAR_NAME_SUCCESS)
        throw BridgeCommandError("'" + name + "' is not a valid character name (2 to 12 letters, no spaces or digits)");
    if (sObjectMgr.IsReservedName(name))
        throw BridgeCommandError("'" + name + "' is a reserved name");
    if (!sObjectMgr.GetPlayerGuidByName(name).IsEmpty())
        throw BridgeCommandError("a character named '" + name + "' already exists");

    if (!args.contains("race") || args["race"].is_null() || !args.contains("class") || args["class"].is_null())
        throw BridgeCommandError("arguments 'race' and 'class' are required");
    const uint32 race = RaceId(args["race"]);
    const uint32 cls = ClassId(args["class"]);
    if (race == 0 || race >= MAX_RACES || !((1 << (race - 1)) & RACEMASK_ALL_PLAYABLE))
        throw BridgeCommandError("not a playable race");
    if (cls == 0 || cls >= MAX_CLASSES || !((1 << (cls - 1)) & CLASSMASK_ALL_PLAYABLE))
        throw BridgeCommandError("not a playable class");
    ChrRacesEntry const* raceEntry = sChrRacesStore.LookupEntry(race);
    ChrClassesEntry const* classEntry = sChrClassesStore.LookupEntry(cls);
    if (!raceEntry || !classEntry || !sObjectMgr.GetPlayerInfo(race, cls))
        throw BridgeCommandError(std::string("a ") + (raceEntry ? raceEntry->name[0] : "?") + " cannot be a " + (classEntry ? classEntry->name[0] : "?"));

    const std::string genderText = Lower(OptionalString(args, "gender"));
    uint8 gender;
    if (genderText == "male") gender = GENDER_MALE;
    else if (genderText == "female") gender = GENDER_FEMALE;
    else throw BridgeCommandError("argument 'gender' must be male or female");

    const uint32 maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);
    const uint32 level = OptionalUnsigned(args, "level", 1);
    if (level < 1 || level > maxLevel)
        throw BridgeCommandError("argument 'level' must be between 1 and " + std::to_string(maxLevel));

    TalentPath* spec = nullptr;
    const std::string specText = OptionalString(args, "spec");
    if (!specText.empty())
    {
        spec = FindTalentPath(uint8(cls), specText);
        if (!spec)
            throw BridgeCommandError("no talent path like '" + specText + "' for " + classEntry->name[0] + " (" + TalentPathNames(uint8(cls)) + ")");
    }
    const std::string roleText = OptionalString(args, "role");
    BotRoles role = BotRoles::BOT_ROLE_NONE;
    if (!roleText.empty())
    {
        role = ChatHelper::parseRole(roleText);
        if (role == BotRoles::BOT_ROLE_NONE)
            throw BridgeCommandError("argument 'role' must be tank, healer or dps");
    }
    const std::string gear = Lower(OptionalString(args, "gear"));
    if (!gear.empty())
        GearQuality(gear);   // validated now, applied on arrival

    uint32 seed = OptionalUnsigned(args, "look", 0);
    if (seed == 0)
        seed = urand(1, 0x7fffffff);
    const Look look = PickLook(uint8(race), gender, seed);

    std::string error;
    const uint32 accountId = GetOrCreateCastAccount(error);
    if (!accountId)
        throw BridgeCommandError(error);

    // From here on, the module's own `.bot create` sequence.
    WorldSession* session = new WorldSession(accountId, nullptr, SEC_PLAYER,
#ifdef MANGOSBOT_TWO
        2, 0, LOCALE_enUS, "", 0, 0, false);
#endif
#ifdef MANGOSBOT_ONE
        2, 0, LOCALE_enUS, "", 0, 0, false);
#endif
#ifdef MANGOSBOT_ZERO
        0, LOCALE_enUS, "", 0);
#endif
    session->SetNoAnticheat();

    Player* character = new Player(session);
    if (!character->Create(sObjectMgr.GeneratePlayerLowGuid(), name, uint8(race), uint8(cls), gender,
                           look.skin, look.face, look.hairStyle, look.hairColor, look.facialHair, 0))
    {
        delete character;
        delete session;
        throw BridgeCommandError("the core refused to create '" + name + "'");
    }

    character->setCinematic(2);
    character->SetAtLoginFlag(AT_LOGIN_NONE);
    sObjectAccessor.AddObject(character);
    const ObjectGuid guid = character->GetObjectGuid();
    const uint32 counter = character->GetGUIDLow();

    if (spec)
        sRandomPlayerbotMgr.SetValue(counter, "specNo", uint32(spec->id + 1));

    if (level > 1)
    {
        character->SetLevel(level);
        character->SetUInt32Value(PLAYER_XP, 0);
        character->InitStatsForLevel(true);
#ifdef MANGOSBOT_ZERO
        character->InitTaxiNodes();
#else
        character->InitTaxiNodesForLevel();
#endif
        character->InitTalentForLevel();
        character->InitPrimaryProfessions();
        character->learnDefaultSpells();

        std::ostringstream talentLog;
        if (spec || role != BotRoles::BOT_ROLE_NONE)
            ChangeTalentsAction::AutoSelectTalents(character, &talentLog, role);

        sRandomPlayerbotMgr.SetValue(counter, "create levelup", 1);   // spells and gear for this level, on arrival
    }
    else
        character->SetLevel(1);
    if (!gear.empty())
        sRandomPlayerbotMgr.SetValue(counter, "create gear", 1, gear);

    character->SaveToDB();

    // The session never took the player, so the logout is bookkeeping and the
    // object is ours to remove and free.
    session->LogoutPlayer();
    sObjectAccessor.RemoveObject(character);
    delete character;
    delete session;

    sLog.outString("Bridge: created %s, %s %s %s, level %u, on cast account %u",
                   name.c_str(), genderText.c_str(), raceEntry->name[0], classEntry->name[0], level, accountId);

    Json result;
    result["guid"] = GuidString(guid);
    result["name"] = name;
    result["race"] = raceEntry->name[0];
    result["class"] = classEntry->name[0];
    result["race_id"] = race;
    result["class_id"] = cls;
    result["gender"] = genderText;
    result["level"] = level;
    result["look"] = seed;
    result["spec"] = spec ? spec->name : "";
    result["account"] = accountId;
    return result;
}

std::optional<Json> Bridge::CmdBotDelete(const BridgeInbound&, const Json& args)
{
    std::string name = RequireString(args, "name");
    if (!normalizePlayerName(name))
        throw BridgeCommandError("'" + name + "' is not a valid character name");
    const ObjectGuid guid = sObjectMgr.GetPlayerGuidByName(name);
    if (guid.IsEmpty())
        throw BridgeCommandError("no character named '" + name + "'");
    const uint32 accountId = sObjectMgr.GetPlayerAccountIdByGUID(guid);
    if (!sPlayerbotAIConfig.IsInCastAccountList(accountId))
        throw BridgeCommandError("'" + name + "' is not a cast character (only characters made by bot.create can be deleted here)");
    if (FindOnlinePlayer(name))
        throw BridgeCommandError("'" + name + "' is in the world; bot.logout it first");

    Player::DeleteFromDB(guid, accountId, true, true);
    CharacterDatabase.PExecute("DELETE FROM ai_playerbot_random_bots WHERE bot = '%u'", guid.GetCounter());
    sLog.outString("Bridge: deleted cast character %s", name.c_str());

    Json result;
    result["guid"] = GuidString(guid);
    result["name"] = name;
    result["deleted"] = true;
    return result;
}

std::optional<Json> Bridge::CmdBotLevel(const BridgeInbound&, const Json& args)
{
    // Re-level a made-to-order character in place: a chapter needs it a little
    // older, or an early random pass left it wrong. Cast characters only, and
    // it must be in the world (add it, or bot.login it, first).
    std::string name = RequireString(args, "name");
    if (!normalizePlayerName(name))
        throw BridgeCommandError("'" + name + "' is not a valid character name");
    const ObjectGuid guid = sObjectMgr.GetPlayerGuidByName(name);
    if (guid.IsEmpty())
        throw BridgeCommandError("no character named '" + name + "'");
    if (!sPlayerbotAIConfig.IsInCastAccountList(sObjectMgr.GetPlayerAccountIdByGUID(guid)))
        throw BridgeCommandError("'" + name + "' is not a cast character (only characters made by bot.create can be re-levelled here)");

    const uint32 maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);
    const uint32 level = OptionalUnsigned(args, "level", 0);
    if (level < 1 || level > maxLevel)
        throw BridgeCommandError("argument 'level' must be between 1 and " + std::to_string(maxLevel));

    Player* bot = FindOnlinePlayer(name);
    if (!bot)
        throw BridgeCommandError("'" + name + "' is not in the world; add it (/add) or bot.login it first, then set its level");
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    if (!ai || ai->IsRealPlayer())
        throw BridgeCommandError("'" + name + "' is not a bot");

    const uint32 was = bot->GetLevel();
    bot->SetLevel(level);
    bot->SetUInt32Value(PLAYER_XP, 0);
    bot->InitStatsForLevel(true);
#ifdef MANGOSBOT_ZERO
    bot->InitTaxiNodes();
#else
    bot->InitTaxiNodesForLevel();
#endif
    bot->InitTalentForLevel();
    bot->InitPrimaryProfessions();
    bot->learnDefaultSpells();

    // Spells and gear the level knows, without touching the level again.
    PlayerbotFactory factory(bot, level);
    factory.Randomize(true, false);
    ai->ResetStrategies();

    sLog.outString("Bridge: re-levelled %s from %u to %u", name.c_str(), was, level);
    Json result;
    result["unit"] = PlayerRef(bot);
    result["name"] = name;
    result["level"] = level;
    result["was"] = was;
    return result;
}

// Scene pieces ---------------------------------------------------------------------
//
// A stranger who teleports in, speaks, and teleports out is a proof of
// concept. These let the control center have one walk up, gesture, sit,
// and walk off, using the module's own movement rather than the map's.

std::optional<Json> Bridge::CmdBotMaster(const BridgeInbound&, const Json& args)
{
    // Give a bot a master for a while (a cast stranger follows the player it
    // is approaching), or take it away. No group is joined, so the party and
    // the companions' minds never see it; only the module's follow does.
    Player* bot = RequireBot(RequireString(args, "bot"));
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    Player* master = nullptr;
    if (args.contains("master") && !args["master"].is_null())
    {
        master = RequireOnlinePlayer(RequireString(args, "master"), "master");
        PlayerbotAI* masterAi = master->GetPlayerbotAI();
        if (masterAi && !masterAi->IsRealPlayer())
            throw BridgeCommandError("'" + std::string(master->GetName()) + "' is a bot; a master must be a real player");
    }
    ai->SetMaster(master);
    ai->ResetStrategies();
    Json result;
    result["unit"] = PlayerRef(bot);
    result["master"] = master ? PlayerRef(master) : Json(nullptr);
    return result;
}

std::optional<Json> Bridge::CmdBotEmote(const BridgeInbound&, const Json& args)
{
    // An animation emote (wave, bow, point...) by the client's id. The text
    // emote that prints a line is bot.say on the emote channel; this is the
    // body moving.
    Player* bot = RequireBot(RequireString(args, "bot"));
    const uint32 emote = OptionalUnsigned(args, "emote", 0);
    if (emote == 0)
        throw BridgeCommandError("argument 'emote' must be an emote id");
    bot->HandleEmoteCommand(emote);
    Json result;
    result["unit"] = PlayerRef(bot);
    result["emote"] = emote;
    return result;
}

std::optional<Json> Bridge::CmdBotStance(const BridgeInbound&, const Json& args)
{
    Player* bot = RequireBot(RequireString(args, "bot"));
    const std::string state = OptionalString(args, "state", "stand");
    uint8 stand;
    if (state == "sit")        stand = UNIT_STAND_STATE_SIT;
    else if (state == "kneel") stand = UNIT_STAND_STATE_KNEEL;
    else if (state == "stand") stand = UNIT_STAND_STATE_STAND;
    else throw BridgeCommandError("argument 'state' must be sit, kneel or stand");
    bot->SetStandState(stand);
    Json result;
    result["unit"] = PlayerRef(bot);
    result["state"] = state;
    return result;
}

bool Bridge::NeedsOutfit(Player* bot)
{
    if (!bot || !sPlayerbotAIConfig.IsCastBot(bot->GetGUIDLow()))
        return false;
    const uint32 counter = bot->GetGUIDLow();
    return sRandomPlayerbotMgr.GetValue(counter, "create levelup") || sRandomPlayerbotMgr.GetValue(counter, "create gear");
}

void Bridge::OutfitOnArrival(Player* bot)
{
    const uint32 counter = bot->GetGUIDLow();
    if (sRandomPlayerbotMgr.GetValue(counter, "create levelup"))
    {
        // The module's incremental pass: keeps the level and the talents it
        // was made with, learns the spells a character of that level knows,
        // and equips it accordingly.
        PlayerbotFactory factory(bot, bot->GetLevel());
        factory.Randomize(true, false);
        sRandomPlayerbotMgr.SetValue(counter, "create levelup", 0);
    }
    if (sRandomPlayerbotMgr.GetValue(counter, "create gear"))
    {
        const std::string gear = sRandomPlayerbotMgr.GetData(counter, "create gear");
        try
        {
            PlayerbotFactory factory(bot, bot->GetLevel(), GearQuality(gear));
            factory.EquipGear();
        }
        catch (const BridgeCommandError&)
        {
            // an unknown word could only have got there by hand; nothing to do
        }
        sRandomPlayerbotMgr.SetValue(counter, "create gear", 0);
    }
    sLog.outString("Bridge: %s outfitted for level %u", bot->GetName(), bot->GetLevel());
}

void Bridge::FlushPendingOutfits()
{
    for (auto it = pendingOutfits_.begin(); it != pendingOutfits_.end();)
    {
        Player* bot = sObjectMgr.GetPlayer(*it);
        if (!bot)
        {
            it = pendingOutfits_.erase(it);
            continue;
        }
        if (!bot->IsInWorld() || bot->IsBeingTeleported() || !bot->GetPlayerbotAI())
        {
            ++it;
            continue;
        }
        OutfitOnArrival(bot);
        it = pendingOutfits_.erase(it);
    }
}

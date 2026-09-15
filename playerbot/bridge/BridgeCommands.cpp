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
#include "playerbot/PlayerbotFactory.h"
#include "playerbot/PlayerbotMgr.h"
#include "playerbot/RandomPlayerbotMgr.h"
#include "playerbot/playerbot.h"

#include "Accounts/AccountMgr.h"
#include "World/World.h"

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
    std::list<std::string> messages = mgr->HandlePlayerbotCommand("add " + RequireString(args, "bot"), master);
    Json result;
    result["messages"] = messages;
    return result;
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
    std::list<uint32> const& accounts = sPlayerbotAIConfig.randomBotAccounts;
    if (accounts.empty())
        throw BridgeCommandError("no random bot accounts exist; the module creates them on first start");

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
        anchor->GetClosePoint(x, y, z, bot->GetObjectBoundingRadius(), distance, angle, bot);
        map = anchor->GetMapId();
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

// Characters made to order ------------------------------------------------------
//
// The module already makes characters: RandomPlayerbotFactory rolls a name,
// a race, a class and a face onto a random-bot account at startup. The
// control center sometimes needs one made to a description instead: the
// body an external player (Isaac) chose for himself, or a recurring cast
// member a story bible named. bot.create is that, and nothing more: it
// writes the character to the database the way the core's own character
// screen does, and returns. Levels and gear are bot.init, which needs the
// character standing in the world, because the module's factory works on a
// Player, not a row.
//
// Where a character lives decides who may use it:
//   account "master"   the named real player's own account. .bot add works
//                      for its owner; the random-bot manager never touches it,
//                      so it keeps its level and its name for as long as the
//                      account does. The right home for a player's body.
//   account "random"   the module's random-bot pool, like the roster.
//                      bot.login (the cast machinery) works; the manager may
//                      log it in on its own and re-roll its level over the
//                      days. The right home for a stranger who comes back.

namespace
{
    uint32 GenderId(const Json& args)
    {
        if (!args.contains("gender") || args["gender"].is_null())
            return urand(0, 1) ? GENDER_MALE : GENDER_FEMALE;
        if (!args["gender"].is_string())
            throw BridgeCommandError("argument 'gender' must be male or female");
        const std::string gender = args["gender"].get<std::string>();
        if (gender == "male")
            return GENDER_MALE;
        if (gender == "female")
            return GENDER_FEMALE;
        throw BridgeCommandError("argument 'gender' must be male or female");
    }

    // A random account with a free character slot, the way the module's own
    // factory fills them: the first one that is not full.
    uint32 RandomAccountWithRoom()
    {
        const uint32 perRealm = sWorld.getConfig(CONFIG_UINT32_CHARACTERS_PER_REALM);
        for (uint32 accountId : sPlayerbotAIConfig.randomBotAccounts)
            if (sAccountMgr.GetCharactersCount(accountId) < perRealm)
                return accountId;
        throw BridgeCommandError("every random bot account is full; raise AiPlayerbot.RandomBotAccountCount");
    }

    Json CharacterRow(ObjectGuid guid)
    {
        Json out;
        auto rows = CharacterDatabase.PQuery("SELECT name, race, class, level, gender, account FROM characters WHERE guid = %u",
                                             guid.GetCounter());
        if (!rows)
            throw BridgeCommandError("the character vanished between two reads");
        Field* fields = rows->Fetch();
        const uint32 race = fields[1].GetUInt8();
        const uint32 cls = fields[2].GetUInt8();
        out["guid"] = Bridge::GuidString(guid);
        out["name"] = fields[0].GetCppString();
        out["race_id"] = race;
        out["class_id"] = cls;
        out["level"] = fields[3].GetUInt32();
        out["gender"] = fields[4].GetUInt8() == 0 ? "male" : "female";
        const uint32 accountId = fields[5].GetUInt32();
        out["account"] = sPlayerbotAIConfig.IsInRandomAccountList(accountId) ? "random" : "player";
        if (ChrRacesEntry const* entry = sChrRacesStore.LookupEntry(race))
            out["race"] = entry->name[0];
        if (ChrClassesEntry const* entry = sChrClassesStore.LookupEntry(cls))
            out["class"] = entry->name[0];
        return out;
    }
}

std::optional<Json> Bridge::CmdBotCreate(const BridgeInbound&, const Json& args)
{
    std::string name = RequireString(args, "name");
    if (!normalizePlayerName(name))
        throw BridgeCommandError("'" + name + "' is not a valid character name");
    if (ObjectMgr::CheckPlayerName(name, true) != CHAR_NAME_SUCCESS)
        throw BridgeCommandError("'" + name + "' does not pass the server's name rules (length, letters, profanity)");
    if (sObjectMgr.IsReservedName(name))
        throw BridgeCommandError("'" + name + "' is a reserved name");

    const std::string kind = OptionalString(args, "account", args.contains("master") ? "master" : "random");
    uint32 accountId = 0;
    if (kind == "master")
    {
        Player* master = RequireOnlinePlayer(RequireString(args, "master"), "master");
        accountId = master->GetSession()->GetAccountId();
    }
    else if (kind == "random")
        accountId = RandomAccountWithRoom();
    else
        throw BridgeCommandError("argument 'account' must be master or random");

    // Made already? Then this is a lookup, so a control center can ask for
    // the same body every start without minding whether it exists yet.
    if (ObjectGuid existing = sObjectMgr.GetPlayerGuidByName(name))
    {
        const uint32 owner = sObjectMgr.GetPlayerAccountIdByGUID(existing);
        const bool ours = kind == "master" ? owner == accountId : sPlayerbotAIConfig.IsInRandomAccountList(owner);
        if (!ours)
            throw BridgeCommandError("'" + name + "' exists on another account; pick a different name");
        Json result = CharacterRow(existing);
        result["created"] = false;
        return result;
    }

    if (!args.contains("race") || !args.contains("class"))
        throw BridgeCommandError("a new character needs 'race' and 'class' (id or name)");
    const uint32 race = RaceId(args["race"]);
    const uint32 cls = ClassId(args["class"]);
    if (!sObjectMgr.GetPlayerInfo(race, cls))
        throw BridgeCommandError("that race cannot be that class on this server");
    const uint32 gender = GenderId(args);
    if (sAccountMgr.GetCharactersCount(accountId) >= sWorld.getConfig(CONFIG_UINT32_CHARACTERS_PER_REALM))
        throw BridgeCommandError("that account has no free character slot");

    // A face, the way the module rolls one for its own bots.
    std::vector<uint8> skinColors, facialHairTypes;
    std::vector<std::pair<uint8, uint8>> faces, hairs;
    for (CharSectionsMap::const_iterator itr = sCharSectionMap.begin(); itr != sCharSectionMap.end(); ++itr)
    {
        CharSectionsEntry const* entry = itr->second;
        if (entry->Race != race || entry->Gender != gender)
            continue;
#ifndef MANGOSBOT_TWO
        switch (entry->BaseSection)
        {
            case SECTION_TYPE_SKIN: skinColors.push_back(entry->ColorIndex); break;
            case SECTION_TYPE_FACE: faces.push_back(std::pair<uint8, uint8>(entry->VariationIndex, entry->ColorIndex)); break;
            case SECTION_TYPE_FACIAL_HAIR: facialHairTypes.push_back(entry->ColorIndex); break;
            case SECTION_TYPE_HAIR: hairs.push_back(std::pair<uint8, uint8>(entry->VariationIndex, entry->ColorIndex)); break;
        }
#else
        switch (entry->BaseSection)
        {
            case SECTION_TYPE_SKIN: skinColors.push_back(entry->Color); break;
            case SECTION_TYPE_FACE: faces.push_back(std::pair<uint8, uint8>(entry->VariationIndex, entry->Color)); break;
            case SECTION_TYPE_FACIAL_HAIR: facialHairTypes.push_back(entry->Color); break;
            case SECTION_TYPE_HAIR: hairs.push_back(std::pair<uint8, uint8>(entry->VariationIndex, entry->Color)); break;
        }
#endif
    }
    if (skinColors.empty() || faces.empty() || hairs.empty())
        throw BridgeCommandError("no appearance data for that race and gender");
    const uint8 skinColor = skinColors[urand(0, skinColors.size() - 1)];
    const std::pair<uint8, uint8> face = faces[urand(0, faces.size() - 1)];
    const std::pair<uint8, uint8> hair = hairs[urand(0, hairs.size() - 1)];
    const bool noFacialHair = race == RACE_TAUREN || (gender == GENDER_FEMALE && race != RACE_NIGHTELF && race != RACE_UNDEAD);
#ifndef MANGOSBOT_TWO
    const uint8 facialHair = noFacialHair || facialHairTypes.empty() ? 0 : facialHairTypes[urand(0, facialHairTypes.size() - 1)];
#else
    const uint8 facialHair = 0;
#endif
    (void)skinColor;

    // The same session-less creation the module's factory does, then the
    // same save the core's character screen does. The character is a row
    // afterwards, not a Player: bot.add or bot.login brings it into the world.
    WorldSession* session = new WorldSession(accountId, NULL, SEC_PLAYER,
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
    Player* player = new Player(session);
    if (!player->Create(sObjectMgr.GeneratePlayerLowGuid(), name, uint8(race), uint8(cls), uint8(gender),
                        face.second, face.first, hair.first, hair.second, facialHair, 0))
    {
        delete player;
        delete session;
        throw BridgeCommandError("the core refused to create '" + name + "' (race/class problem?)");
    }
    player->setCinematic(2);
    player->SetAtLoginFlag(AT_LOGIN_NONE);
    player->SaveToDB();
    const ObjectGuid guid = player->GetObjectGuid();
    delete player;                       // created only to call SaveToDB(), as the core does
    delete session;
    sWorld.UpdateRealmCharCount(accountId);
    sLog.outBasic("BRIDGE: created character '%s' (guid %u, race %u, class %u) on account %u for the control center",
                  name.c_str(), guid.GetCounter(), race, cls, accountId);

    Json result = CharacterRow(guid);
    result["created"] = true;
    return result;
}

std::optional<Json> Bridge::CmdBotInit(const BridgeInbound&, const Json& args)
{
    Player* bot = RequireBot(RequireString(args, "bot"));
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    const uint32 maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);
    uint32 level = OptionalUnsigned(args, "level", 0);
    if (level == 0)
    {
        Player* master = ai ? ai->GetMaster() : nullptr;
        level = master && ai->HasRealPlayerMaster() ? master->GetLevel() : bot->GetLevel();
    }
    if (level < 1 || level > maxLevel)
        throw BridgeCommandError("argument 'level' must be between 1 and " + std::to_string(maxLevel));

    const std::string quality = OptionalString(args, "quality", "green");
    uint32 itemQuality = ITEM_QUALITY_UNCOMMON;
    if (quality == "white" || quality == "common") itemQuality = ITEM_QUALITY_NORMAL;
    else if (quality == "green" || quality == "uncommon") itemQuality = ITEM_QUALITY_UNCOMMON;
    else if (quality == "blue" || quality == "rare") itemQuality = ITEM_QUALITY_RARE;
    else if (quality == "epic" || quality == "purple") itemQuality = ITEM_QUALITY_EPIC;
    else throw BridgeCommandError("argument 'quality' must be white, green, blue or epic");

    // The module's own ".bot init <quality>": level, spells, skills, gear,
    // consumables, from scratch at that level. Works on a bot in a group.
    PlayerbotFactory factory(bot, level, itemQuality);
    factory.Randomize(false, OptionalBool(args, "sync", false));

    Json result;
    result["unit"] = PlayerRef(bot);
    result["level"] = bot->GetLevel();
    result["quality"] = quality;
    return result;
}


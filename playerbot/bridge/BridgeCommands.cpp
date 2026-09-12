// Command side of the bridge. Every handler runs on the world thread, before
// the maps update, so it may touch any player or creature directly. A handler
// returns its result, or std::nullopt when it will reply later itself.
//
// Deliberately absent: a way to run server console commands from the socket.
// Those levers (weather, spawning, mail) stay behind SOAP, which requires a
// GM account and password; the bridge only exposes scoped game actions.

#include "playerbot/bridge/Bridge.h"

#include "playerbot/PlayerbotAI.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/PlayerbotMgr.h"
#include "playerbot/playerbot.h"

#include "Entities/Creature.h"
#include "Entities/Player.h"
#include "Globals/ObjectAccessor.h"
#include "Globals/ObjectMgr.h"
#include "Maps/Map.h"
#include "Maps/MapManager.h"

#include <memory>

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

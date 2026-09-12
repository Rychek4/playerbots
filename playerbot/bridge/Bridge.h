#pragma once

// The narrator bridge: the module's door to an external control center.
//
//   events out   game happenings, serialised as JSON lines (see PROTOCOL.md)
//   commands in  JSON lines, queued by the transport and executed here on the
//                world thread, inside RandomPlayerbotMgr::UpdateAI, before the
//                maps update, so touching any Player is safe
//
// Everything lives in this folder plus four one-line hooks:
//   PlayerbotAIConfig::Initialize          -> sBridge.Start()
//   RandomPlayerbotMgr::UpdateAI           -> sBridge.Update(diff)
//   PlayerbotAI::HandleBotOutgoingPacket   -> sBridge.OnBotPacket(bot, packet)
//   PlayerbotMgr::HandleMasterIncomingPacket -> sBridge.OnMasterPacket(master, packet)

#include "Common.h"
#include "Entities/ObjectGuid.h"
#include "playerbot/bridge/BridgeJson.h"
#include "playerbot/bridge/BridgeServer.h"

#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>

class Creature;
class Group;
class Player;
class Unit;
class WorldObject;
class WorldPacket;

namespace BridgeProtocol
{
    constexpr int BRIDGE_PROTOCOL_VERSION = 1;
    constexpr char SERVER_NAME[] = "playerbots-bridge";

    constexpr char EV_BOT_LOGIN[] = "bot.login";
    constexpr char EV_BOT_LOGOUT[] = "bot.logout";
    constexpr char EV_CHAT[] = "chat";
    constexpr char EV_EMOTE[] = "emote";
    constexpr char EV_GROUP_CHANGED[] = "group.changed";
    constexpr char EV_ZONE_CHANGED[] = "zone.changed";
    constexpr char EV_COMBAT_STARTED[] = "combat.started";
    constexpr char EV_COMBAT_ENDED[] = "combat.ended";
    constexpr char EV_LEVEL_UP[] = "level.up";
    constexpr char EV_QUEST_UPDATE[] = "quest.update";
    constexpr char EV_ITEM_PUSHED[] = "item.pushed";
    constexpr char EV_DEATH[] = "death";
    constexpr char EV_RESURRECT[] = "resurrect";
    constexpr char EV_SCENE[] = "scene";

    constexpr uint32 DUPLICATE_WINDOW_MS = 2000;
}

// Thrown by command handlers; becomes an ok:false reply.
class BridgeCommandError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

class Bridge
{
public:
    static Bridge& instance()
    {
        static Bridge instance;
        return instance;
    }

    // Reads AiPlayerbot.Bridge.* from the config and starts listening. Safe to
    // call again (config reload): a running bridge is left alone.
    void Start();
    void Stop();
    bool IsRunning() const { return server_.IsRunning(); }

    // World thread, once per world tick.
    void Update(uint32 diff);

    // Hooks. Run on whichever thread owns the player; they only read the
    // player and the packet, and Emit is thread-safe.
    void OnBotPacket(Player* bot, const WorldPacket& packet);
    void OnMasterPacket(Player* master, const WorldPacket& packet);

    // Thread-safe. Wraps data in the event envelope and broadcasts it.
    void Emit(const char* name, Json data);

    // JSON builders. Call on the thread that owns the object.
    static std::string GuidString(ObjectGuid guid);
    static Json UnitRef(Unit* unit);
    static Json PlayerRef(Player* player);
    static Json PlayerRefByGuid(ObjectGuid guid);
    static Json Position(WorldObject* object);
    static void AddZone(WorldObject* object, Json& out);
    static Json PartyMember(Player* player);
    static Json NearbyUnit(Player* center, Unit* unit);
    Json BuildScene(Player* center);

private:
    Bridge();
    ~Bridge();

    using Handler = std::optional<Json> (Bridge::*)(const BridgeInbound& in, const Json& args);

    struct UnitState
    {
        uint32 map = 0;
        uint32 zone = 0;
        uint32 area = 0;
        uint32 level = 0;
        bool alive = true;
        bool inCombat = false;
    };

    // Dispatch and replies
    void DrainCommands();
    void Dispatch(const BridgeInbound& in);
    void Reply(const BridgeInbound& in, const Json& result);
    void ReplyError(const BridgeInbound& in, const std::string& error);
    void SendReply(const std::weak_ptr<BridgeConnection>& to, const Json& id, const Json& result, const std::string& error);

    // Events
    void Snapshot();
    void TrackTransitions(Player* player);
    void ParseBotChat(Player* bot, const WorldPacket& packet);
    bool IsDuplicate(const std::string& key);
    void ExpireDuplicates(uint32 now);

    // Commands (BridgeCommands.cpp)
    std::optional<Json> CmdBotList(const BridgeInbound& in, const Json& args);
    std::optional<Json> CmdBotAdd(const BridgeInbound& in, const Json& args);
    std::optional<Json> CmdBotRemove(const BridgeInbound& in, const Json& args);
    std::optional<Json> CmdBotCommand(const BridgeInbound& in, const Json& args);
    std::optional<Json> CmdBotSay(const BridgeInbound& in, const Json& args);
    std::optional<Json> CmdNpcSay(const BridgeInbound& in, const Json& args);
    std::optional<Json> CmdNpcEmote(const BridgeInbound& in, const Json& args);
    std::optional<Json> CmdSceneGet(const BridgeInbound& in, const Json& args);

    // Command helpers
    static Player* FindOnlinePlayer(const std::string& name);
    static Player* RequireOnlinePlayer(const std::string& name, const char* what);
    static Player* RequireBot(const std::string& name);
    static Creature* FindCreature(const Json& args);
    static std::string RequireString(const Json& args, const char* key);
    static std::string OptionalString(const Json& args, const char* key, const std::string& fallback = "");
    static std::vector<Player*> RealPlayersOnline();

    BridgeServer server_;
    std::map<std::string, Handler> handlers_;
    std::atomic<uint64> seq_{0};
    uint32 snapshotTimer_ = 0;

    std::mutex duplicatesMutex_;
    std::unordered_map<std::string, uint32> duplicates_;

    // World thread only
    std::map<ObjectGuid, UnitState> tracked_;
    std::map<ObjectGuid, std::string> botsOnline_;      // guid -> name
    std::map<ObjectGuid, std::string> groupSignature_;  // real player -> leader + members
};

#define sBridge Bridge::instance()

#pragma once

// The narrator bridge: the module's door to an external control center.
//
//   events out   game happenings, serialised as JSON lines (see PROTOCOL.md)
//   commands in  JSON lines, queued by the transport and executed here on the
//                world thread, inside RandomPlayerbotMgr::UpdateAI, before the
//                maps update, so touching any Player is safe
//
// Everything lives in this folder plus seven one-line hooks:
//   PlayerbotAIConfig::Initialize            -> sBridge.Start()
//   RandomPlayerbotMgr::UpdateAI             -> sBridge.Update(diff)
//   PlayerbotAI::HandleBotOutgoingPacket     -> sBridge.OnBotPacket(bot, packet)
//   PlayerbotMgr::HandleMasterIncomingPacket -> sBridge.OnMasterPacket(master, packet)
//   PlayerbotMgr::HandleMasterOutgoingPacket -> sBridge.OnMasterOutgoingPacket(master, packet)
//   PlayerbotHolder::OnBotLogin              -> sBridge.OnBotLogin(bot)
//   PlayerbotHolder::LogoutPlayerBot         -> sBridge.OnBotLogout(bot)
//
// Events drive the world. Chat, emotes, items and quests come from packets.
// Zone, level, death, combat and group changes are detected on the world tick
// and emitted once, with whatever cause the game can name. The bubble around
// each real player is reported as units entering and leaving. The full scene
// is sent only as a slow reconciliation and on request.

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
    constexpr char EV_UNIT_ENTERED[] = "unit.entered";
    constexpr char EV_UNIT_LEFT[] = "unit.left";
    constexpr char EV_MOVEMENT[] = "movement";
    constexpr char EV_BOT_ACTIVITY[] = "bot.activity";

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
    void OnBotPacket(Player* bot, const WorldPacket& packet) { OnOutgoingPacket(bot, packet); }
    void OnMasterOutgoingPacket(Player* master, const WorldPacket& packet) { OnOutgoingPacket(master, packet); }
    void OnMasterPacket(Player* master, const WorldPacket& packet);
    void OnBotLogin(Player* bot);
    void OnBotLogout(Player* bot);

    // Cast accounts (bot.create). Called from Start(); safe to call again.
    void LoadCastAccounts();

    // A character a client logged in through bot.login. The random-bot manager
    // leaves such a character's level and place alone while the request stands.
    bool IsRequestedLogin(uint32 counter) const { return requestedLogins_.count(ObjectGuid(HIGHGUID_PLAYER, counter)) > 0; }

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
        bool moving = false;       // real players only: the last movement state announced
        uint32 stillSince = 0;     // ms clock when the player last came to rest, 0 while moving
        std::string activity;      // companions only: the module's last executed action, as announced
        ObjectGuid rpgTarget;      // the NPC the companion was heading for, as announced
        std::string travel;        // where the companion was travelling, as announced
        uint32 activityAt = 0;     // ms clock of the last bot.activity, for the throttle
    };

    // Dispatch and replies
    void DrainCommands();
    void Dispatch(const BridgeInbound& in);
    void Reply(const BridgeInbound& in, const Json& result);
    void ReplyError(const BridgeInbound& in, const std::string& error);
    void SendReply(const std::weak_ptr<BridgeConnection>& to, const Json& id, const Json& result, const std::string& error);

    // Events
    void OnOutgoingPacket(Player* receiver, const WorldPacket& packet);
    void ParseChat(Player* receiver, const WorldPacket& packet);
    void Watch();                       // every world tick: transitions of everyone in a real player's party
    void WatchMember(Player* player);
    void WatchZone(Player* player);
    void WatchGroup(Player* real);
    bool Announces(Player* bot) const;  // is this bot's coming and going any business of a client's?
    void EmitBotLogin(Player* bot);     // the bot.login event proper, once the bot is really in the world
    void FlushPendingLogins();          // every world tick: emit for bots that have arrived since
    void BubbleScan();                  // every bubble interval: who entered or left each real player's bubble
    void Snapshot();                    // every snapshot interval: reconciliation scenes
    void ForgetTracking();
    std::vector<Player*> TrackedParty(std::vector<Player*>* reals = nullptr);
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
    // Cast (BridgeCommands.cpp): bots the control center logs in, places and dismisses
    std::optional<Json> CmdBotRoster(const BridgeInbound& in, const Json& args);
    std::optional<Json> CmdBotLogin(const BridgeInbound& in, const Json& args);
    std::optional<Json> CmdBotLogout(const BridgeInbound& in, const Json& args);
    std::optional<Json> CmdBotPlace(const BridgeInbound& in, const Json& args);
    std::optional<Json> CmdBotStrategy(const BridgeInbound& in, const Json& args);
    std::optional<Json> CmdPlayerSay(const BridgeInbound& in, const Json& args);
    std::optional<Json> CmdWeather(const BridgeInbound& in, const Json& args);
    // Characters made to order (BridgeCommands.cpp, "Cast characters")
    std::optional<Json> CmdBotCreate(const BridgeInbound& in, const Json& args);
    std::optional<Json> CmdBotDelete(const BridgeInbound& in, const Json& args);
    std::optional<Json> CmdBotLevel(const BridgeInbound& in, const Json& args);
    // Scene pieces (BridgeCommands.cpp): what a cast stranger can do besides speak
    std::optional<Json> CmdBotMaster(const BridgeInbound& in, const Json& args);
    std::optional<Json> CmdBotEmote(const BridgeInbound& in, const Json& args);
    std::optional<Json> CmdBotStance(const BridgeInbound& in, const Json& args);
    std::optional<Json> CmdBotFace(const BridgeInbound& in, const Json& args);
    std::optional<Json> CmdQuestNearby(const BridgeInbound& in, const Json& args);
    std::optional<Json> CmdNpcAbout(const BridgeInbound& in, const Json& args);
    std::optional<Json> CmdNpcFace(const BridgeInbound& in, const Json& args);
    static void QuestsAt(Player* player, Creature* giver, Json& offered, Json& turnIn, size_t most);
    static uint32 GetOrCreateCastAccount(std::string& error);
    static void RegisterCastAccount(uint32 accountId);
    static bool NeedsOutfit(Player* bot);
    void OutfitOnArrival(Player* bot);  // spells and gear for the level it was made at, once it stands in the world with its AI
    void FlushPendingOutfits();         // every world tick, before pending logins are announced
    void FlushPendingAttach();          // every world tick: give bot.add's characters their master once they stand in the world

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
    uint32 bubbleTimer_ = 0;
    bool hadClients_ = false;

    std::mutex duplicatesMutex_;
    std::unordered_map<std::string, uint32> duplicates_;

    // World thread only
    std::map<ObjectGuid, UnitState> tracked_;                              // party members of real players
    std::map<ObjectGuid, uint64> groupSignature_;                          // real player -> leader and members
    std::map<ObjectGuid, std::map<ObjectGuid, std::string>> bubble_;       // real player -> units around them
    std::set<ObjectGuid> bubbleReady_;                                     // real players whose bubble has a baseline
    std::set<ObjectGuid> pendingLogins_;                                   // bots the module has that the core has not put in the world yet
    std::set<ObjectGuid> requestedLogins_;                                 // bots a client logged in through bot.login; announced even without a master
    std::set<ObjectGuid> pendingOutfits_;                                  // made-to-order bots that still need their level's spells and gear; announced after
    std::map<ObjectGuid, std::pair<ObjectGuid, uint32>> pendingAttach_;    // bot.add on a pool or cast character: bot -> (master, deadline ms)
};

#define sBridge Bridge::instance()

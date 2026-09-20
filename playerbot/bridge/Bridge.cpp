#include "playerbot/bridge/Bridge.h"

#include "playerbot/PlayerbotAIConfig.h"

#include "Log/Log.h"
#include "Util/Timer.h"
#include "World/World.h"

#include <chrono>

using namespace BridgeProtocol;

namespace
{
    int64 NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
}

Bridge::Bridge()
{
    handlers_["bot.list"] = &Bridge::CmdBotList;
    handlers_["bot.add"] = &Bridge::CmdBotAdd;
    handlers_["bot.remove"] = &Bridge::CmdBotRemove;
    handlers_["bot.command"] = &Bridge::CmdBotCommand;
    handlers_["bot.say"] = &Bridge::CmdBotSay;
    handlers_["npc.say"] = &Bridge::CmdNpcSay;
    handlers_["npc.emote"] = &Bridge::CmdNpcEmote;
    handlers_["scene.get"] = &Bridge::CmdSceneGet;
    handlers_["bot.roster"] = &Bridge::CmdBotRoster;
    handlers_["bot.login"] = &Bridge::CmdBotLogin;
    handlers_["bot.logout"] = &Bridge::CmdBotLogout;
    handlers_["bot.place"] = &Bridge::CmdBotPlace;
    handlers_["bot.strategy"] = &Bridge::CmdBotStrategy;
    handlers_["player.say"] = &Bridge::CmdPlayerSay;
    handlers_["weather"] = &Bridge::CmdWeather;
    handlers_["bot.create"] = &Bridge::CmdBotCreate;
    handlers_["bot.delete"] = &Bridge::CmdBotDelete;
    handlers_["bot.level"] = &Bridge::CmdBotLevel;
    handlers_["bot.master"] = &Bridge::CmdBotMaster;
    handlers_["bot.emote"] = &Bridge::CmdBotEmote;
    handlers_["bot.stance"] = &Bridge::CmdBotStance;
    handlers_["bot.face"] = &Bridge::CmdBotFace;
    handlers_["quest.nearby"] = &Bridge::CmdQuestNearby;
    handlers_["npc.about"] = &Bridge::CmdNpcAbout;
    handlers_["npc.face"] = &Bridge::CmdNpcFace;
}

Bridge::~Bridge()
{
    Stop();
}

void Bridge::Start()
{
    LoadCastAccounts();   // whether or not the socket opens: the module's ownership rules need the list
    if (!sPlayerbotAIConfig.enabled || sPlayerbotAIConfig.bridgePort <= 0)
    {
        if (server_.IsRunning())
            Stop();
        return;
    }
    if (server_.IsRunning())
        return;

    Json hello;
    hello["type"] = "hello";
    hello["protocol"] = BRIDGE_PROTOCOL_VERSION;
    hello["server"] = SERVER_NAME;
#if defined(MANGOSBOT_ZERO)
    hello["build"] = "cmangos-classic";
#elif defined(MANGOSBOT_ONE)
    hello["build"] = "cmangos-tbc";
#elif defined(MANGOSBOT_TWO)
    hello["build"] = "cmangos-wotlk";
#else
    hello["build"] = "cmangos";
#endif
    hello["realm"] = std::to_string(realmID);

    const std::string bindIp = sPlayerbotAIConfig.bridgeBindIp.empty() ? std::string("127.0.0.1") : sPlayerbotAIConfig.bridgeBindIp;
    const uint32 maxClients = sPlayerbotAIConfig.bridgeMaxClients > 0 ? uint32(sPlayerbotAIConfig.bridgeMaxClients) : 1u;
    if (server_.Start(bindIp, uint16(sPlayerbotAIConfig.bridgePort), maxClients, JsonLine(hello)))
        sLog.outString("Bridge: listening on %s:%d (protocol %d)", bindIp.c_str(), sPlayerbotAIConfig.bridgePort, BRIDGE_PROTOCOL_VERSION);
}

void Bridge::Stop()
{
    server_.Stop();
    ForgetTracking();
}

void Bridge::ForgetTracking()
{
    tracked_.clear();
    groupSignature_.clear();
    bubble_.clear();
}

// Tick -----------------------------------------------------------------------

void Bridge::Update(uint32 diff)
{
    if (!server_.IsRunning())
        return;

    DrainCommands();
    FlushPendingOutfits();
    FlushPendingAttach();
    FlushPendingLogins();

    const bool haveClients = server_.ClientCount() > 0;
    if (!haveClients)
    {
        if (hadClients_)
            ForgetTracking();   // a client that connects later starts from a clean baseline
        hadClients_ = false;
        snapshotTimer_ += diff;
        if (snapshotTimer_ >= 30000)
        {
            snapshotTimer_ = 0;
            ExpireDuplicates(WorldTimer::getMSTime());
        }
        return;
    }
    const bool justConnected = !hadClients_;
    hadClients_ = true;

    Watch();   // cheap field reads for a handful of players; emits on the tick a change happens

    bubbleTimer_ += diff;
    const uint32 bubbleInterval = sPlayerbotAIConfig.bridgeBubbleInterval > 0 ? uint32(sPlayerbotAIConfig.bridgeBubbleInterval) : 500u;
    if (bubbleTimer_ >= bubbleInterval)
    {
        bubbleTimer_ = 0;
        BubbleScan();
    }

    snapshotTimer_ += diff;
    const uint32 snapshotInterval = sPlayerbotAIConfig.bridgeSnapshotInterval > 0 ? uint32(sPlayerbotAIConfig.bridgeSnapshotInterval) : 30000u;
    if (justConnected)
        snapshotTimer_ = snapshotInterval;   // the first client gets a full picture at once, not in 30 s
    if (snapshotTimer_ >= snapshotInterval)
    {
        snapshotTimer_ = 0;
        ExpireDuplicates(WorldTimer::getMSTime());
        Snapshot();
    }
}

void Bridge::DrainCommands()
{
    std::deque<BridgeInbound> batch;
    server_.Drain(batch);
    for (BridgeInbound const& in : batch)
        Dispatch(in);
}

void Bridge::Dispatch(const BridgeInbound& in)
{
    const std::string name = in.message["name"];
    auto it = handlers_.find(name);
    if (it == handlers_.end())
    {
        ReplyError(in, "unknown command: " + name);
        return;
    }

    try
    {
        if (std::optional<Json> result = (this->*it->second)(in, in.message["args"]))
            Reply(in, *result);
        // nullopt: the handler took ownership of the reply and sends it later
    }
    catch (BridgeCommandError const& e)
    {
        ReplyError(in, e.what());
    }
    catch (std::exception const& e)
    {
        sLog.outError("Bridge: command %s failed: %s", name.c_str(), e.what());
        ReplyError(in, std::string("internal error: ") + e.what());
    }
}

void Bridge::Reply(const BridgeInbound& in, const Json& result)
{
    SendReply(in.from, in.message["id"], result, "");
}

void Bridge::ReplyError(const BridgeInbound& in, const std::string& error)
{
    SendReply(in.from, in.message["id"], Json::object(), error);
}

void Bridge::SendReply(const std::weak_ptr<BridgeConnection>& to, const Json& id, const Json& result, const std::string& error)
{
    Json reply;
    reply["type"] = "reply";
    reply["id"] = id;
    if (error.empty())
    {
        reply["ok"] = true;
        reply["result"] = result;
    }
    else
    {
        reply["ok"] = false;
        reply["error"] = error;
    }
    server_.Send(to, JsonLine(reply));
}

// Events ---------------------------------------------------------------------

void Bridge::Emit(const char* name, Json data)
{
    if (!server_.IsRunning() || server_.ClientCount() == 0)
        return;

    Json event;
    event["type"] = "event";
    event["name"] = name;
    event["seq"] = ++seq_;
    event["t"] = NowMs();
    event["data"] = std::move(data);
    server_.Broadcast(JsonLine(event));
}

bool Bridge::IsDuplicate(const std::string& key)
{
    const uint32 now = WorldTimer::getMSTime();
    std::lock_guard<std::mutex> guard(duplicatesMutex_);
    auto it = duplicates_.find(key);
    if (it != duplicates_.end() && now - it->second < DUPLICATE_WINDOW_MS)
        return true;
    duplicates_[key] = now;
    return false;
}

void Bridge::ExpireDuplicates(uint32 now)
{
    std::lock_guard<std::mutex> guard(duplicatesMutex_);
    for (auto it = duplicates_.begin(); it != duplicates_.end();)
    {
        if (now - it->second >= DUPLICATE_WINDOW_MS)
            it = duplicates_.erase(it);
        else
            ++it;
    }
}

// Event side of the bridge: packet hooks, periodic scene snapshots, state
// transitions, and the JSON builders shared with the command side.

#include "playerbot/bridge/Bridge.h"

#include "playerbot/PlayerbotAI.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/playerbot.h"

#include "Entities/Creature.h"
#include "Entities/Player.h"
#include "Entities/Unit.h"
#include "Globals/ObjectAccessor.h"
#include "Globals/ObjectMgr.h"
#include "Grids/CellImpl.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Groups/Group.h"
#include "Log/Log.h"
#include "Server/DBCStores.h"
#include "Server/DBCStructure.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Util/ByteBuffer.h"

#include <algorithm>

using namespace BridgeProtocol;

namespace
{
    // Wire name of a chat message type; nullptr for types the bridge ignores.
    const char* ChatChannelName(uint32 type)
    {
        switch (type)
        {
            case CHAT_MSG_SAY:               return "say";
            case CHAT_MSG_YELL:              return "yell";
            case CHAT_MSG_PARTY:             return "party";
            case CHAT_MSG_RAID:
            case CHAT_MSG_RAID_LEADER:
            case CHAT_MSG_RAID_WARNING:      return "raid";
            case CHAT_MSG_WHISPER:
            case CHAT_MSG_WHISPER_INFORM:    return "whisper";
            case CHAT_MSG_GUILD:             return "guild";
            case CHAT_MSG_OFFICER:           return "officer";
            case CHAT_MSG_EMOTE:             return "emote";
            case CHAT_MSG_CHANNEL:           return "channel";
            case CHAT_MSG_MONSTER_SAY:       return "monster_say";
            case CHAT_MSG_MONSTER_YELL:      return "monster_yell";
            case CHAT_MSG_MONSTER_EMOTE:
            case CHAT_MSG_RAID_BOSS_EMOTE:   return "monster_emote";
            case CHAT_MSG_MONSTER_WHISPER:
            case CHAT_MSG_RAID_BOSS_WHISPER: return "monster_whisper";
            default:                         return nullptr;
        }
    }

    bool IsMonsterChat(uint32 type)
    {
        switch (type)
        {
            case CHAT_MSG_MONSTER_SAY:
            case CHAT_MSG_MONSTER_YELL:
            case CHAT_MSG_MONSTER_EMOTE:
            case CHAT_MSG_MONSTER_WHISPER:
            case CHAT_MSG_RAID_BOSS_EMOTE:
            case CHAT_MSG_RAID_BOSS_WHISPER:
                return true;
            default:
                return false;
        }
    }

    void AddQuestName(Json& out, uint32 questId)
    {
        if (Quest const* quest = sObjectMgr.GetQuestTemplate(questId))
            out["quest_name"] = quest->GetTitle();
    }

    Json UnitOrGuid(Unit* unit, ObjectGuid guid)
    {
        if (unit)
            return Bridge::UnitRef(unit);
        Json ref;
        ref["guid"] = Bridge::GuidString(guid);
        ref["kind"] = guid.IsPlayer() ? "player" : guid.IsCreatureOrPet() ? "creature" : "other";
        return ref;
    }
}

// Builders -------------------------------------------------------------------

std::string Bridge::GuidString(ObjectGuid guid)
{
    return std::to_string(guid.GetRawValue());
}

Json Bridge::PlayerRef(Player* player)
{
    if (!player)
        return nullptr;

    Json ref;
    ref["guid"] = GuidString(player->GetObjectGuid());
    ref["name"] = player->GetName();
    ref["kind"] = "player";
    ref["level"] = player->GetLevel();
    ref["class_id"] = uint32(player->getClass());
    ref["race_id"] = uint32(player->getRace());
    ref["gender"] = uint32(player->getGender());
    if (ChrClassesEntry const* cls = sChrClassesStore.LookupEntry(player->getClass()))
        ref["class"] = cls->name[0];
    if (ChrRacesEntry const* race = sChrRacesStore.LookupEntry(player->getRace()))
        ref["race"] = race->name[0];

    PlayerbotAI* ai = player->GetPlayerbotAI();
    const bool isBot = ai && !ai->IsRealPlayer();
    ref["is_bot"] = isBot;
    if (isBot)
    {
        Player* master = ai->GetMaster();
        ref["master"] = master ? Json(master->GetName()) : Json(nullptr);
    }
    return ref;
}

Json Bridge::PlayerRefByGuid(ObjectGuid guid)
{
    if (Player* player = ObjectAccessor::FindPlayer(guid))
        return PlayerRef(player);

    Json ref;
    ref["guid"] = GuidString(guid);
    ref["kind"] = "player";
    std::string name;
    if (sObjectMgr.GetPlayerNameByGUID(guid, name))
        ref["name"] = name;
    return ref;
}

Json Bridge::UnitRef(Unit* unit)
{
    if (!unit)
        return nullptr;
    if (unit->GetTypeId() == TYPEID_PLAYER)
        return PlayerRef(static_cast<Player*>(unit));

    Json ref;
    ref["guid"] = GuidString(unit->GetObjectGuid());
    ref["name"] = unit->GetName();
    ref["kind"] = unit->GetObjectGuid().IsPet() ? "pet" : "creature";
    ref["level"] = unit->GetLevel();
    ref["entry"] = unit->GetEntry();
    if (unit->GetTypeId() == TYPEID_UNIT)
    {
        if (CreatureInfo const* info = static_cast<Creature*>(unit)->GetCreatureInfo())
        {
            if (info->SubName && *info->SubName)
                ref["sub_name"] = info->SubName;
            ref["creature_type"] = info->CreatureType;
        }
    }
    return ref;
}

Json Bridge::Position(WorldObject* object)
{
    Json pos;
    pos["map"] = object->GetMapId();
    pos["instance"] = object->GetInstanceId();
    pos["x"] = object->GetPositionX();
    pos["y"] = object->GetPositionY();
    pos["z"] = object->GetPositionZ();
    pos["o"] = object->GetOrientation();
    return pos;
}

void Bridge::AddZone(WorldObject* object, Json& out)
{
    uint32 zone = 0, area = 0;
    object->GetZoneAndAreaId(zone, area);
    out["map"] = object->GetMapId();
    out["zone"] = zone;
    out["area"] = area;
    if (AreaTableEntry const* zoneEntry = GetAreaEntryByAreaID(zone))
        out["zone_name"] = zoneEntry->area_name[0];
    if (AreaTableEntry const* areaEntry = GetAreaEntryByAreaID(area))
        out["area_name"] = areaEntry->area_name[0];
}

Json Bridge::PartyMember(Player* player)
{
    Json member;
    member["unit"] = PlayerRef(player);
    member["pos"] = Position(player);
    AddZone(player, member);
    member["hp_pct"] = int32(player->GetHealthPercent());
    const Powers power = player->GetPowerType();
    const uint32 maxPower = player->GetMaxPower(power);
    member["power_type"] = int32(power);
    member["power_pct"] = maxPower ? int32(player->GetPower(power) * 100 / maxPower) : 0;
    member["alive"] = player->IsAlive();
    member["in_combat"] = player->IsInCombat();
    const ObjectGuid target = player->GetSelectionGuid();
    member["target"] = target.IsEmpty() ? Json(nullptr) : Json(GuidString(target));
    return member;
}

Json Bridge::NearbyUnit(Player* center, Unit* unit)
{
    Json entry;
    entry["unit"] = UnitRef(unit);
    entry["pos"] = Position(unit);
    entry["dist"] = center->GetDistance(unit);
    entry["hostile"] = center->IsEnemy(unit);
    entry["alive"] = unit->IsAlive();
    entry["in_combat"] = unit->IsInCombat();
    if (unit->GetTypeId() == TYPEID_UNIT)
    {
        const uint32 flags = unit->GetUInt32Value(UNIT_NPC_FLAGS);
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
        entry["npc_flags"] = services;
    }
    return entry;
}

Json Bridge::BuildScene(Player* center)
{
    Json scene;
    scene["center"] = GuidString(center->GetObjectGuid());
    AddZone(center, scene);

    Json party = Json::array();
    if (Group* group = center->GetGroup())
    {
        scene["leader"] = GuidString(group->GetLeaderGuid());
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->getSource())
                if (member->IsInWorld())
                    party.push_back(PartyMember(member));
    }
    else
    {
        scene["leader"] = nullptr;
        party.push_back(PartyMember(center));
    }
    scene["party"] = party;

    const float radius = sPlayerbotAIConfig.bridgeSceneRadius > 0.0f ? sPlayerbotAIConfig.bridgeSceneRadius : 40.0f;
    std::list<Unit*> units;
    MaNGOS::AnyUnitInObjectRangeCheck check(center, radius);
    MaNGOS::UnitListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(units, check);
    Cell::VisitAllObjects(center, searcher, radius);

    Json nearby = Json::array();
    for (Unit* unit : units)
    {
        if (unit == center)
            continue;
        if (unit->GetTypeId() == TYPEID_PLAYER && center->GetGroup() && static_cast<Player*>(unit)->GetGroup() == center->GetGroup())
            continue;   // already listed under party
        nearby.push_back(NearbyUnit(center, unit));
    }
    scene["nearby"] = nearby;
    return scene;
}

// Transitions, bubble and reconciliation ------------------------------------------

std::vector<Player*> Bridge::TrackedParty(std::vector<Player*>* reals)
{
    std::vector<Player*> members;
    std::set<ObjectGuid> seen;
    for (auto const& entry : sObjectAccessor.GetPlayers())
    {
        Player* player = entry.second;
        if (!player || !player->IsInWorld())
            continue;
        PlayerbotAI* ai = player->GetPlayerbotAI();
        if (ai && !ai->IsRealPlayer())
            continue;
        if (reals)
            reals->push_back(player);
        if (Group* group = player->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->getSource())
                    if (member->IsInWorld() && seen.insert(member->GetObjectGuid()).second)
                        members.push_back(member);
        }
        else if (seen.insert(player->GetObjectGuid()).second)
            members.push_back(player);
    }
    return members;
}

// Every world tick: cached field reads for everyone in a real player's party.
// A change is emitted on the tick it is seen, with what the game can say about why.
void Bridge::Watch()
{
    std::vector<Player*> reals;
    std::vector<Player*> members = TrackedParty(&reals);

    std::set<ObjectGuid> seen;
    for (Player* member : members)
    {
        seen.insert(member->GetObjectGuid());
        WatchMember(member);
    }
    for (auto it = tracked_.begin(); it != tracked_.end();)
        it = seen.count(it->first) ? std::next(it) : tracked_.erase(it);

    std::set<ObjectGuid> realsSeen;
    for (Player* real : reals)
    {
        realsSeen.insert(real->GetObjectGuid());
        WatchGroup(real);
    }
    for (auto it = groupSignature_.begin(); it != groupSignature_.end();)
        it = realsSeen.count(it->first) ? std::next(it) : groupSignature_.erase(it);
    for (auto it = bubble_.begin(); it != bubble_.end();)
        it = realsSeen.count(it->first) ? std::next(it) : bubble_.erase(it);
    for (auto it = bubbleReady_.begin(); it != bubbleReady_.end();)
        it = realsSeen.count(*it) ? std::next(it) : bubbleReady_.erase(it);
}

void Bridge::WatchMember(Player* player)
{
    auto it = tracked_.find(player->GetObjectGuid());
    if (it == tracked_.end())
    {
        UnitState first;                                   // first sight is a baseline, not an event
        first.map = player->GetMapId();
        player->GetZoneAndAreaId(first.zone, first.area);
        first.level = player->GetLevel();
        first.alive = player->IsAlive();
        first.inCombat = player->IsInCombat();
        tracked_[player->GetObjectGuid()] = first;
        return;
    }
    UnitState& prev = it->second;

    const uint32 level = player->GetLevel();
    if (level > prev.level)
    {
        Json data;
        data["unit"] = PlayerRef(player);
        data["level"] = level;
        data["from"] = prev.level;
        Emit(EV_LEVEL_UP, data);
    }
    prev.level = level;

    const bool alive = player->IsAlive();
    if (alive != prev.alive)
    {
        Json data;
        data["unit"] = PlayerRef(player);
        data["pos"] = Position(player);
        AddZone(player, data);
        Emit(alive ? EV_RESURRECT : EV_DEATH, data);
        prev.alive = alive;
    }

    const bool inCombat = player->IsInCombat();
    if (inCombat != prev.inCombat)
    {
        Json data;
        data["unit"] = PlayerRef(player);
        data["target"] = UnitRef(player->GetVictim());
        if (inCombat)
        {
            Json attackers = Json::array();
            for (Unit* attacker : player->getAttackers())
            {
                if (attackers.size() >= 3)
                    break;
                attackers.push_back(UnitRef(attacker));
            }
            data["attackers"] = attackers;
        }
        Emit(inCombat ? EV_COMBAT_STARTED : EV_COMBAT_ENDED, data);
        prev.inCombat = inCombat;
    }
}

// At the bubble interval: zone and area need a terrain lookup, so they are not read every tick.
void Bridge::WatchZone(Player* player)
{
    auto it = tracked_.find(player->GetObjectGuid());
    if (it == tracked_.end())
        return;                                            // WatchMember lays the baseline first
    UnitState& prev = it->second;
    uint32 zone = 0, area = 0;
    player->GetZoneAndAreaId(zone, area);
    const uint32 map = player->GetMapId();
    if (zone == prev.zone && area == prev.area && map == prev.map)
        return;

    Json data;
    data["unit"] = PlayerRef(player);
    AddZone(player, data);
    Json from;
    from["map"] = prev.map;
    from["zone"] = prev.zone;
    from["area"] = prev.area;
    if (AreaTableEntry const* zoneEntry = GetAreaEntryByAreaID(prev.zone))
        from["zone_name"] = zoneEntry->area_name[0];
    if (AreaTableEntry const* areaEntry = GetAreaEntryByAreaID(prev.area))
        from["area_name"] = areaEntry->area_name[0];
    data["from"] = from;
    Emit(EV_ZONE_CHANGED, data);
    prev.map = map;
    prev.zone = zone;
    prev.area = area;
}

void Bridge::WatchGroup(Player* real)
{
    uint64 signature = 0;
    std::vector<Player*> members;
    Group* group = real->GetGroup();
    if (group)
    {
        signature = group->GetLeaderGuid().GetRawValue() ^ (uint64(group->GetMembersCount()) << 56);
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->getSource())
            {
                signature += uint64(member->GetObjectGuid().GetCounter()) * 0x9E3779B97F4A7C15ULL;   // order-independent
                if (member->IsInWorld())
                    members.push_back(member);
            }
    }
    auto it = groupSignature_.find(real->GetObjectGuid());
    if (it == groupSignature_.end())
    {
        groupSignature_[real->GetObjectGuid()] = signature;   // baseline
        return;
    }
    if (it->second == signature)
        return;
    it->second = signature;

    Json data;
    data["player"] = PlayerRef(real);
    data["leader"] = group ? Json(GuidString(group->GetLeaderGuid())) : Json(nullptr);
    Json list = Json::array();
    if (group)
        for (Player* member : members)
            list.push_back(PlayerRef(member));
    else
        list.push_back(PlayerRef(real));
    data["members"] = list;
    Emit(EV_GROUP_CHANGED, data);
}

// At the bubble interval: who entered or left each real player's bubble.
void Bridge::BubbleScan()
{
    std::vector<Player*> reals;
    std::vector<Player*> members = TrackedParty(&reals);
    for (Player* member : members)
        WatchZone(member);

    const float radius = sPlayerbotAIConfig.bridgeSceneRadius > 0.0f ? sPlayerbotAIConfig.bridgeSceneRadius : 40.0f;
    for (Player* real : reals)
    {
        std::list<Unit*> units;
        MaNGOS::AnyUnitInObjectRangeCheck check(real, radius);
        MaNGOS::UnitListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(units, check);
        Cell::VisitAllObjects(real, searcher, radius);

        const ObjectGuid center = real->GetObjectGuid();
        std::map<ObjectGuid, std::string>& last = bubble_[center];
        const bool ready = bubbleReady_.count(center) > 0;
        std::map<ObjectGuid, std::string> now;
        for (Unit* unit : units)
        {
            if (unit == real)
                continue;
            if (unit->GetTypeId() == TYPEID_PLAYER && real->GetGroup() && static_cast<Player*>(unit)->GetGroup() == real->GetGroup())
                continue;   // party members are watched, not bubbled
            const ObjectGuid guid = unit->GetObjectGuid();
            now[guid] = unit->GetName();
            if (ready && !last.count(guid))
            {
                Json data = NearbyUnit(real, unit);
                data["center"] = GuidString(center);
                Emit(EV_UNIT_ENTERED, data);
            }
        }
        if (ready)
        {
            for (auto const& entry : last)
            {
                if (now.count(entry.first))
                    continue;
                Json data;
                data["center"] = GuidString(center);
                data["guid"] = GuidString(entry.first);
                data["name"] = entry.second;
                Emit(EV_UNIT_LEFT, data);
            }
        }
        last.swap(now);
        bubbleReady_.insert(center);
    }
}

// At the snapshot interval: one full scene per real player, so a client can reconcile drift.
void Bridge::Snapshot()
{
    std::vector<Player*> reals;
    TrackedParty(&reals);
    for (Player* real : reals)
        Emit(EV_SCENE, BuildScene(real));
}

// Login hooks -------------------------------------------------------------------

// PlayerbotHolder::OnBotLogin runs when the module has the bot, which can be
// a tick before the core has added it to the world. A client that acts on
// bot.login the moment it arrives then gets "is not online" from bot.place,
// as happened on the first live run. So the event waits for IsInWorld and is
// sent from the world tick if it has to.
void Bridge::OnBotLogin(Player* bot)
{
    if (!bot || !server_.IsRunning())
        return;
    // A character made to order arrives with the level it was given and the
    // spells and gear of a level 1. Its outfit is finished on the world tick,
    // once the module has given it an AI, and the bot.login event waits for
    // that: by the time a client hears of it, the character is complete and
    // nothing the client sets on it afterwards is reset by the outfitting.
    const bool outfitting = NeedsOutfit(bot);
    if (outfitting)
        pendingOutfits_.insert(bot->GetObjectGuid());
    if (server_.ClientCount() == 0)
        return;
    if (!bot->IsInWorld() || outfitting)
    {
        pendingLogins_.insert(bot->GetObjectGuid());
        return;
    }
    EmitBotLogin(bot);
}

// A thousand random bots log in and out all day; a client cares about the
// ones that are its business: a companion with a master, a member of a
// tracked party, or a character it asked for through bot.login. The first
// live session showed the rest as a scrolling wall of "logged in (masterless)".
bool Bridge::Announces(Player* bot) const
{
    if (!bot)
        return false;
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    if (ai && ai->GetMaster())
        return true;
    const ObjectGuid guid = bot->GetObjectGuid();
    return tracked_.count(guid) > 0 || requestedLogins_.count(guid) > 0;
}

void Bridge::EmitBotLogin(Player* bot)
{
    if (!Announces(bot))
        return;
    Json data;
    data["bot"] = PlayerRef(bot);
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    Player* master = ai ? ai->GetMaster() : nullptr;
    data["master"] = master ? PlayerRef(master) : Json(nullptr);
    Emit(EV_BOT_LOGIN, data);
}

void Bridge::FlushPendingLogins()
{
    if (pendingLogins_.empty())
        return;
    if (server_.ClientCount() == 0)
    {
        pendingLogins_.clear();
        return;
    }
    for (auto it = pendingLogins_.begin(); it != pendingLogins_.end();)
    {
        Player* bot = sObjectMgr.GetPlayer(*it);
        if (!bot)                       // gone again before it ever arrived
            it = pendingLogins_.erase(it);
        else if (bot->IsInWorld() && pendingOutfits_.count(*it) == 0)
        {
            EmitBotLogin(bot);
            it = pendingLogins_.erase(it);
        }
        else
            ++it;
    }
}

void Bridge::OnBotLogout(Player* bot)
{
    if (bot)
    {
        pendingLogins_.erase(bot->GetObjectGuid());   // never announce a login that ended first
        pendingOutfits_.erase(bot->GetObjectGuid());  // it will be outfitted next time it arrives
    }
    if (!bot || !server_.IsRunning() || server_.ClientCount() == 0)
        return;
    const bool announce = Announces(bot);
    requestedLogins_.erase(bot->GetObjectGuid());     // a request is spent once the character has gone
    if (!announce)
        return;
    Json data;
    data["guid"] = GuidString(bot->GetObjectGuid());
    data["name"] = bot->GetName();
    Emit(EV_BOT_LOGOUT, data);
}

// Packet hooks -----------------------------------------------------------------

// Packets sent to a bot or to the human: the same parse, the receiver differs.
void Bridge::OnOutgoingPacket(Player* receiver, const WorldPacket& packet)
{
    // IsInWorld also guarantees a map; GetMap() asserts on a player still logging in.
    if (!receiver || !receiver->IsInWorld() || !server_.IsRunning() || server_.ClientCount() == 0)
        return;

    // Listen only through ears that belong to a tracked party. Every bot's
    // session receives the chat and emotes around it, and with a thousand
    // random bots roaming, some bot is always standing next to an Ironforge
    // vendor; the first live session put that vendor's line in front of a
    // player in Northshire. Real players always count. Bots count when they
    // are in a real player's party, which is when what they hear is ours.
    if (receiver->GetPlayerbotAI() && tracked_.find(receiver->GetObjectGuid()) == tracked_.end())
        return;

    try
    {
        switch (packet.GetOpcode())
        {
            case SMSG_MESSAGECHAT:
                ParseChat(receiver, packet);
                break;

            case SMSG_EMOTE:
            {
                WorldPacket p(packet);
                p.rpos(0);
                uint32 emoteId;
                ObjectGuid guid;
                p >> emoteId >> guid;
                if (IsDuplicate("emote|" + GuidString(guid) + "|" + std::to_string(emoteId)))
                    break;
                Json data;
                data["kind"] = "emote";
                data["sender"] = UnitOrGuid(receiver->GetMap()->GetUnit(guid), guid);
                data["emote_id"] = emoteId;
                Emit(EV_EMOTE, data);
                break;
            }

            case SMSG_TEXT_EMOTE:
            {
                WorldPacket p(packet);
                p.rpos(0);
                ObjectGuid guid;
                uint32 textEmote, emoteNum, nameLen;
                std::string targetName;
                p >> guid >> textEmote >> emoteNum >> nameLen >> targetName;
                if (IsDuplicate("textemote|" + GuidString(guid) + "|" + std::to_string(textEmote) + "|" + std::to_string(emoteNum)))
                    break;
                Json data;
                data["kind"] = "text_emote";
                data["sender"] = UnitOrGuid(receiver->GetMap()->GetUnit(guid), guid);
                data["text_emote"] = textEmote;
                data["emote_num"] = emoteNum;
                data["target_name"] = targetName;
                Emit(EV_EMOTE, data);
                break;
            }

            case SMSG_ITEM_PUSH_RESULT:
            {
                WorldPacket p(packet);
                p.rpos(0);
                ObjectGuid guid;
                uint32 received, created, showInChat, slot, entry, suffix, count;
                int32 randomProperty;
                uint8 bag;
                p >> guid >> received >> created >> showInChat >> bag >> slot >> entry >> suffix >> randomProperty >> count;
                if (IsDuplicate("item|" + GuidString(guid) + "|" + std::to_string(entry) + "|" + std::to_string(count) + "|" + std::to_string(slot)))
                    break;
                Json data;
                data["unit"] = PlayerRefByGuid(guid);
                data["item_entry"] = entry;
                data["count"] = count;
                data["received"] = received != 0;
                data["created"] = created != 0;
                if (ItemPrototype const* proto = ObjectMgr::GetItemPrototype(entry))
                {
                    data["item_name"] = proto->Name1;
                    data["quality"] = proto->Quality;
                }
                Emit(EV_ITEM_PUSHED, data);
                break;
            }

            case SMSG_QUESTUPDATE_COMPLETE:
            {
                WorldPacket p(packet);
                p.rpos(0);
                uint32 questId;
                p >> questId;
                Json data;
                data["unit"] = PlayerRef(receiver);
                data["kind"] = "objectives_complete";
                data["quest_id"] = questId;
                AddQuestName(data, questId);
                Emit(EV_QUEST_UPDATE, data);
                break;
            }

            default:
                break;
        }
    }
    catch (ByteBufferException const&)
    {
        sLog.outDetail("Bridge: could not parse opcode %u for %s", packet.GetOpcode(), receiver->GetName());
    }
}

void Bridge::ParseChat(Player* receiver, const WorldPacket& packet)
{
#ifdef MANGOSBOT_ZERO
    WorldPacket p(packet);
    p.rpos(0);
    uint8 type;
    uint32 lang;
    p >> type >> lang;
    if (lang == LANG_ADDON)
        return;

    const char* channel = ChatChannelName(type);
    if (!channel)
        return;

    ObjectGuid sender, target;
    std::string senderName, channelName;
    switch (type)
    {
        case CHAT_MSG_MONSTER_WHISPER:
        case CHAT_MSG_RAID_BOSS_WHISPER:
        case CHAT_MSG_RAID_BOSS_EMOTE:
        case CHAT_MSG_MONSTER_EMOTE:
        {
            uint32 nameLen;
            p >> nameLen >> senderName >> target;
            break;
        }
        case CHAT_MSG_SAY:
        case CHAT_MSG_PARTY:
        case CHAT_MSG_YELL:
            p >> sender >> sender;
            break;
        case CHAT_MSG_MONSTER_SAY:
        case CHAT_MSG_MONSTER_YELL:
        {
            uint32 nameLen;
            p >> sender >> nameLen >> senderName >> target;
            break;
        }
        case CHAT_MSG_CHANNEL:
        {
            uint32 rank;
            p >> channelName >> rank >> sender;
            break;
        }
        default:
            p >> sender;
            break;
    }
    uint32 textLen;
    std::string text;
    p >> textLen >> text;

    const bool monster = IsMonsterChat(type);
    const std::string senderKey = monster ? senderName : GuidString(sender);
    if (IsDuplicate(std::string("chat|") + channel + "|" + senderKey + "|" + text))
        return;

    Json data;
    data["channel"] = channel;
    if (type == CHAT_MSG_CHANNEL)
        data["channel_name"] = channelName;
    data["lang"] = lang;
    data["text"] = text;
    if (monster)
    {
        Json ref;
        ref["guid"] = sender.IsEmpty() ? Json(nullptr) : Json(GuidString(sender));
        ref["name"] = senderName;
        ref["kind"] = "creature";
        data["sender"] = ref;
        data["receiver"] = target.IsEmpty() ? Json(nullptr) : PlayerRefByGuid(target);
    }
    else if (type == CHAT_MSG_WHISPER_INFORM)
    {
        data["sender"] = PlayerRef(receiver);            // the receiver whispered someone
        data["receiver"] = PlayerRefByGuid(sender);  // the packet carries the recipient
    }
    else
    {
        data["sender"] = PlayerRefByGuid(sender);
        data["receiver"] = type == CHAT_MSG_WHISPER ? PlayerRef(receiver) : Json(nullptr);
    }
    Emit(EV_CHAT, data);
#else
    // SMSG_MESSAGECHAT layouts differ per expansion; only the classic one is
    // wired. Chat typed by real players still arrives through OnMasterPacket.
    (void)receiver;
    (void)packet;
#endif
}

void Bridge::OnMasterPacket(Player* master, const WorldPacket& packet)
{
    if (!master || !master->IsInWorld() || !server_.IsRunning() || server_.ClientCount() == 0)
        return;

    try
    {
        switch (packet.GetOpcode())
        {
            case CMSG_MESSAGECHAT:
            {
                WorldPacket p(packet);
                p.rpos(0);
                uint32 type, lang;
                p >> type >> lang;
                if (lang == LANG_ADDON)
                    break;
                const char* channel = ChatChannelName(type);
                if (!channel || type == CHAT_MSG_WHISPER_INFORM)
                    break;
                std::string to, channelName, text;
                if (type == CHAT_MSG_WHISPER)
                    p >> to;
                else if (type == CHAT_MSG_CHANNEL)
                    p >> channelName;
                p >> text;
                if (IsDuplicate(std::string("chat|") + channel + "|" + GuidString(master->GetObjectGuid()) + "|" + text))
                    break;
                Json data;
                data["channel"] = channel;
                if (type == CHAT_MSG_CHANNEL)
                    data["channel_name"] = channelName;
                data["lang"] = lang;
                data["text"] = text;
                data["sender"] = PlayerRef(master);
                if (to.empty())
                    data["receiver"] = nullptr;
                else if (Player* receiver = FindOnlinePlayer(to))
                    data["receiver"] = PlayerRef(receiver);
                else
                {
                    Json ref;
                    ref["name"] = to;
                    ref["kind"] = "player";
                    data["receiver"] = ref;
                }
                Emit(EV_CHAT, data);
                break;
            }

            case CMSG_TEXT_EMOTE:
            {
                WorldPacket p(packet);
                p.rpos(0);
                uint32 textEmote, emoteNum;
                ObjectGuid guid;
                p >> textEmote >> emoteNum >> guid;
                if (IsDuplicate("textemote|" + GuidString(master->GetObjectGuid()) + "|" + std::to_string(textEmote) + "|" + std::to_string(emoteNum)))
                    break;
                Json data;
                data["kind"] = "text_emote";
                data["sender"] = PlayerRef(master);
                data["text_emote"] = textEmote;
                data["emote_num"] = emoteNum;
                data["target"] = guid.IsEmpty() ? Json(nullptr) : UnitOrGuid(master->GetMap()->GetUnit(guid), guid);
                Emit(EV_EMOTE, data);
                break;
            }

            case CMSG_QUESTGIVER_ACCEPT_QUEST:
            case CMSG_QUESTGIVER_CHOOSE_REWARD:
            {
                WorldPacket p(packet);
                p.rpos(0);
                ObjectGuid giver;
                uint32 questId;
                p >> giver >> questId;
                Json data;
                data["unit"] = PlayerRef(master);
                data["kind"] = packet.GetOpcode() == CMSG_QUESTGIVER_ACCEPT_QUEST ? "accepted" : "turned_in";
                data["quest_id"] = questId;
                data["giver"] = GuidString(giver);
                AddQuestName(data, questId);
                Emit(EV_QUEST_UPDATE, data);
                break;
            }

            default:
                break;
        }
    }
    catch (ByteBufferException const&)
    {
        sLog.outDetail("Bridge: could not parse opcode %u from %s", packet.GetOpcode(), master->GetName());
    }
}

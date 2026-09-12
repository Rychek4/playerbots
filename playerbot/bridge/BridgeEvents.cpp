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

// Snapshot and transitions ----------------------------------------------------

void Bridge::Snapshot()
{
    std::vector<Player*> reals;
    std::map<ObjectGuid, Player*> bots;
    for (auto const& entry : sObjectAccessor.GetPlayers())
    {
        Player* player = entry.second;
        if (!player || !player->IsInWorld())
            continue;
        PlayerbotAI* ai = player->GetPlayerbotAI();
        if (ai && !ai->IsRealPlayer())
            bots[player->GetObjectGuid()] = player;
        else
            reals.push_back(player);
    }

    // Bot logins and logouts, by comparing with the previous snapshot.
    for (auto const& entry : bots)
    {
        if (botsOnline_.count(entry.first))
            continue;
        Json data;
        data["bot"] = PlayerRef(entry.second);
        Player* master = entry.second->GetPlayerbotAI()->GetMaster();
        data["master"] = master ? PlayerRef(master) : Json(nullptr);
        Emit(EV_BOT_LOGIN, data);
    }
    for (auto const& entry : botsOnline_)
    {
        if (bots.count(entry.first))
            continue;
        Json data;
        data["guid"] = GuidString(entry.first);
        data["name"] = entry.second;
        Emit(EV_BOT_LOGOUT, data);
    }
    botsOnline_.clear();
    for (auto const& entry : bots)
        botsOnline_[entry.first] = entry.second->GetName();

    // One scene per real player, plus transitions for everyone in their party.
    std::set<ObjectGuid> seen;
    std::set<ObjectGuid> realsSeen;
    for (Player* real : reals)
    {
        realsSeen.insert(real->GetObjectGuid());

        std::vector<Player*> members;
        std::string signature;
        Group* group = real->GetGroup();
        if (group)
        {
            signature = GuidString(group->GetLeaderGuid());
            std::vector<std::string> ids;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                if (Player* member = ref->getSource())
                {
                    ids.push_back(GuidString(member->GetObjectGuid()));
                    if (member->IsInWorld())
                        members.push_back(member);
                }
            }
            std::sort(ids.begin(), ids.end());
            for (std::string const& id : ids)
                signature += "," + id;
        }
        else
            members.push_back(real);

        std::string& last = groupSignature_[real->GetObjectGuid()];
        if (last != signature)
        {
            const bool first = last.empty();
            last = signature;
            if (!first || group)   // a lone player's first snapshot is not a change
            {
                Json data;
                data["player"] = PlayerRef(real);
                data["leader"] = group ? Json(GuidString(group->GetLeaderGuid())) : Json(nullptr);
                Json list = Json::array();
                for (Player* member : members)
                    list.push_back(PlayerRef(member));
                data["members"] = list;
                Emit(EV_GROUP_CHANGED, data);
            }
        }

        for (Player* member : members)
            if (seen.insert(member->GetObjectGuid()).second)
                TrackTransitions(member);

        Emit(EV_SCENE, BuildScene(real));
    }

    for (auto it = tracked_.begin(); it != tracked_.end();)
        it = seen.count(it->first) ? std::next(it) : tracked_.erase(it);
    for (auto it = groupSignature_.begin(); it != groupSignature_.end();)
        it = realsSeen.count(it->first) ? std::next(it) : groupSignature_.erase(it);
}

void Bridge::TrackTransitions(Player* player)
{
    UnitState now;
    player->GetZoneAndAreaId(now.zone, now.area);
    now.map = player->GetMapId();
    now.level = player->GetLevel();
    now.alive = player->IsAlive();
    now.inCombat = player->IsInCombat();

    auto it = tracked_.find(player->GetObjectGuid());
    if (it == tracked_.end())
    {
        tracked_[player->GetObjectGuid()] = now;   // first sight: nothing to compare with
        return;
    }
    UnitState& prev = it->second;

    if (now.map != prev.map || now.zone != prev.zone || now.area != prev.area)
    {
        Json data;
        data["unit"] = PlayerRef(player);
        AddZone(player, data);
        Json from;
        from["map"] = prev.map;
        from["zone"] = prev.zone;
        from["area"] = prev.area;
        data["from"] = from;
        Emit(EV_ZONE_CHANGED, data);
    }
    if (now.level > prev.level)
    {
        Json data;
        data["unit"] = PlayerRef(player);
        data["level"] = now.level;
        data["from"] = prev.level;
        Emit(EV_LEVEL_UP, data);
    }
    if (now.alive != prev.alive)
    {
        Json data;
        data["unit"] = PlayerRef(player);
        data["pos"] = Position(player);
        AddZone(player, data);
        Emit(now.alive ? EV_RESURRECT : EV_DEATH, data);
    }
    if (now.inCombat != prev.inCombat)
    {
        Json data;
        data["unit"] = PlayerRef(player);
        data["target"] = UnitRef(player->GetVictim());
        Emit(now.inCombat ? EV_COMBAT_STARTED : EV_COMBAT_ENDED, data);
    }
    prev = now;
}

// Packet hooks -----------------------------------------------------------------

void Bridge::OnBotPacket(Player* bot, const WorldPacket& packet)
{
    // IsInWorld also guarantees a map; GetMap() asserts on a player still logging in.
    if (!bot || !bot->IsInWorld() || !server_.IsRunning() || server_.ClientCount() == 0)
        return;

    try
    {
        switch (packet.GetOpcode())
        {
            case SMSG_MESSAGECHAT:
                ParseBotChat(bot, packet);
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
                data["sender"] = UnitOrGuid(bot->GetMap()->GetUnit(guid), guid);
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
                data["sender"] = UnitOrGuid(bot->GetMap()->GetUnit(guid), guid);
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
                data["unit"] = PlayerRef(bot);
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
        sLog.outDetail("Bridge: could not parse opcode %u for bot %s", packet.GetOpcode(), bot->GetName());
    }
}

void Bridge::ParseBotChat(Player* bot, const WorldPacket& packet)
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
        data["sender"] = PlayerRef(bot);            // the bot whispered someone
        data["receiver"] = PlayerRefByGuid(sender);  // the packet carries the recipient
    }
    else
    {
        data["sender"] = PlayerRefByGuid(sender);
        data["receiver"] = type == CHAT_MSG_WHISPER ? PlayerRef(bot) : Json(nullptr);
    }
    Emit(EV_CHAT, data);
#else
    // SMSG_MESSAGECHAT layouts differ per expansion; only the classic one is
    // wired. Chat from real players still arrives through OnMasterPacket.
    (void)bot;
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

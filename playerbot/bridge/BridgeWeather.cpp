#include "playerbot/bridge/Bridge.h"

#include "playerbot/PlayerbotAI.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/playerbot.h"

#include "Entities/Object.h"
#include "Entities/Player.h"
#include "Globals/ObjectAccessor.h"
#include "Log/Log.h"
#include "Maps/Map.h"
#include "Server/DBCStores.h"
#include "Weather/Weather.h"
#include "World/World.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <set>
#include <sstream>

// The weather (the owner, 19 October): the core's own cycle rerolls each
// zone every ten minutes against `game_weather`, and four times in five
// what it rolls is so light it does not show (grade under 0.33; the client
// draws nothing under about 0.27). In some twenty sessions it turned the
// sky once on its own, to rain at 0.02. So the bridge keeps the weather, by
// rule and nothing else; the narrator never decides it.
//
// The unit is a front, not a moment's weather (the owner: "rain start with
// a low intensity and slowly build up to a storm", and "that pre-storm
// wind and those first rain drops, when you know something bad is coming,
// but it's not here yet. When you hear the far off thunder"). A front has
// a shape:
//
//   gathering   a storm only: WeatherGather minutes of clear sky and far
//               thunder (WeatherThunderFar), before anything falls
//   first drops the lightest grade that shows (WeatherGrade)
//   building    up to the front's peak over the first half of it
//   peak        held through the third quarter; thunder overhead when it
//               is a storm (WeatherThunder)
//   easing      back down to the lightest grade over the last quarter
//   clear       and fine, until a front rolls again
//
// While the sky is clear, each zone with a real player in it rolls for a
// front once every WeatherMinutes (a spell): its own `game_weather`
// chances for the season, times WeatherOdds, and times WeatherNight as well
// while the bridge's sky says night (TurnTheSky). A front lasts 1 to
// WeatherLength spells and peaks somewhere from WeatherGrade to 1, lighter
// more often than heavier; rain peaking at a storm's grade (0.7, heavy rain
// in the core and "storm" on the narrator's page) gathers first. Nothing
// is rolled with a die: each roll, and each thunderclap's timing, is a hash
// of the zone and the time, so the same zone at the same time, under the
// same settings, always gets the same weather, and the log can be worked
// out again from the config. The weather is set as permanent, so the core's
// own cycle never turns it. A zone with no `game_weather` row is left as
// the core leaves it (always fine). WeatherMinutes 0: the core's cycle, as
// before.
namespace
{
    const float STORM_GRADE = 0.7f;             // heavy rain: the core's WEATHER_STATE_HEAVY_RAIN, the page's "storm"

    // splitmix64: a well-spread 64-bit hash of a 64-bit input.
    uint64 Mix(uint64 x)
    {
        x += 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
    }

    uint64 Hash(uint32 zone, uint64 at, uint64 salt = 0)
    {
        return Mix(uint64(zone) << 40 ^ at ^ salt << 56);
    }

    // The core's season (Weather::ReGenerate): spring from 20 March, 91 days each.
    uint32 Season(time_t at)
    {
        std::tm local {};
#ifdef _WIN32
        localtime_s(&local, &at);
#else
        localtime_r(&at, &local);
#endif
        return uint32((local.tm_yday - 78 + 365) / 91) % 4;
    }

    const char* WeatherWord(uint32 type)
    {
        switch (type)
        {
            case WEATHER_TYPE_RAIN:  return "rain";
            case WEATHER_TYPE_SNOW:  return "snow";
            case WEATHER_TYPE_STORM: return "storm";
            default:                 return "fine";
        }
    }

    std::vector<uint32> SoundIds(const std::string& list)
    {
        std::vector<uint32> ids;
        std::stringstream in(list);
        std::string item;
        while (std::getline(in, item, ','))
            if (uint32 id = uint32(std::strtoul(item.c_str(), nullptr, 10)))
                ids.push_back(id);
        return ids;
    }

    float Least()
    {
        return std::min(std::max(sPlayerbotAIConfig.bridgeWeatherGrade, 0.0f), 1.0f);
    }

    const char* ZoneName(uint32 zone)
    {
        AreaTableEntry const* entry = GetAreaEntryByAreaID(zone);
        return entry ? entry->area_name[0] : "a zone";
    }
}

bool Bridge::WeatherOn() const
{
    return sPlayerbotAIConfig.bridgeWeatherMinutes > 0;
}

bool Bridge::WeatherNight() const
{
    if (SkyCycleOn())
        return SkyRegime() == 2;
    const time_t now = sWorld.GetGameTime();
    std::tm local {};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return local.tm_hour >= 22 || local.tm_hour < 5;          // the night window the sky uses
}

Bridge::WeatherFront Bridge::FrontFor(uint32 zone, uint64 spell, bool night) const
{
    WeatherFront front;
    WeatherZoneChances const* chances = sWeatherMgr.GetWeatherChances(zone);
    if (!chances)
        return front;
    front.known = true;
    const uint64 spellSeconds = uint64(sPlayerbotAIConfig.bridgeWeatherMinutes) * 60;
    const uint64 now = spell * spellSeconds;
    WeatherSeasonChances const& season = chances->data[Season(time_t(now))];
    const double odds = sPlayerbotAIConfig.bridgeWeatherOdds * (night ? sPlayerbotAIConfig.bridgeWeatherNight : 1.0);

    const uint64 hash = Hash(zone, spell);
    const double roll = double(hash % 10000) / 100.0;                        // 0 to 99.99
    const double rain = season.rainChance * odds;
    const double snow = rain + season.snowChance * odds;
    const double storm = snow + season.stormChance * odds;
    front.type = uint32(roll < rain ? WEATHER_TYPE_RAIN : roll < snow ? WEATHER_TYPE_SNOW : roll < storm ? WEATHER_TYPE_STORM : WEATHER_TYPE_FINE);
    if (front.type == WEATHER_TYPE_FINE)
        return front;

    const double u = double((hash >> 16) % 1000) / 1000.0;
    front.peak = float(Least() + (1.0f - Least()) * u * u);                  // lighter more often than heavier
    const uint64 spells = 1 + (hash >> 32) % uint64(std::max(1, sPlayerbotAIConfig.bridgeWeatherLength));
    const bool gathers = front.type == WEATHER_TYPE_RAIN && front.peak >= STORM_GRADE;
    front.gather = now;
    front.start = now + (gathers ? uint64(std::max(0, sPlayerbotAIConfig.bridgeWeatherGather)) * 60 : 0);
    front.end = front.start + spells * spellSeconds;
    front.thunder = front.gather;
    return front;
}

Bridge::WeatherNow Bridge::WeatherAt(const WeatherFront& front, uint64 now) const
{
    WeatherNow at;
    if (front.type == WEATHER_TYPE_FINE || now < front.start || now >= front.end)
        return at;                                                          // clear, gathering, or over
    const double f = double(now - front.start) / double(front.end - front.start);
    const double least = Least(), peak = std::max(double(front.peak), least);
    at.type = front.type;
    at.grade = float(f < 0.5 ? least + (peak - least) * (f / 0.5)          // first drops, building
                   : f < 0.75 ? peak                                       // the peak
                   : peak + (least - peak) * ((f - 0.75) / 0.25));         // easing
    return at;
}

void Bridge::Thunder(const WeatherZone& key, WeatherFront& front, uint64 now, const std::vector<Player*>& here)
{
    // Thunder belongs to a storm: far off while it gathers, overhead at the
    // peak. Each clap's timing is a hash of the zone and the last clap.
    if (front.type != WEATHER_TYPE_RAIN || front.peak < STORM_GRADE || now < front.thunder || now >= front.end)
        return;
    const bool gathering = now < front.start;
    const WeatherNow at = WeatherAt(front, now);
    // Not `far` and `near`: windows.h defines both as macros.
    const std::vector<uint32> distant = SoundIds(sPlayerbotAIConfig.bridgeWeatherThunderFar);
    const std::vector<uint32> overhead = SoundIds(sPlayerbotAIConfig.bridgeWeatherThunder);
    const std::vector<uint32>& sounds = gathering ? distant : overhead;
    const uint64 hash = Hash(key.zone, front.thunder, 1);
    front.thunder = now + (gathering ? 40 + hash % 80 : 20 + hash % 40);   // far: every 40 to 120 s; overhead: 20 to 60 s
    if (sounds.empty() || (!gathering && at.grade < STORM_GRADE))
        return;
    const uint32 sound = sounds[(hash >> 32) % sounds.size()];
    for (Player* player : here)
        player->PlayDirectSound(sound, PlayPacketParameters(PlayPacketSettings::TARGET, player));
}

void Bridge::HoldWeather(uint32 map, uint32 instance, uint32 zone)
{
    // The weather command (the overlay's buttons): the player chose the sky.
    // Any front there is over, and the next roll waits for the next spell.
    if (!WeatherOn())
        return;
    const WeatherZone key {map, instance, zone};
    const uint64 spellSeconds = uint64(sPlayerbotAIConfig.bridgeWeatherMinutes) * 60;
    weatherFront_.erase(key);
    weatherHold_[key] = (uint64(time(nullptr)) / spellSeconds + 1) * spellSeconds;
}

void Bridge::TurnTheWeather(uint32 diff)
{
    if (!WeatherOn())
    {
        weatherFront_.clear();
        weatherHold_.clear();
        weatherHere_.clear();
        return;
    }
    weatherTick_ += diff;
    if (weatherTick_ < 1000)
        return;
    weatherTick_ = 0;

    // Once a second: every zone with a real player in it follows its front.
    const uint64 now = uint64(time(nullptr));
    const uint64 spell = now / (uint64(sPlayerbotAIConfig.bridgeWeatherMinutes) * 60);
    std::map<WeatherZone, std::pair<Map*, std::vector<Player*>>> zones;
    for (Player* player : RealPlayersOnline())
        if (Map* map = player->GetMap())
        {
            auto& entry = zones[WeatherZone {map->GetId(), map->GetInstanceId(), player->GetZoneId()}];
            entry.first = map;
            entry.second.push_back(player);
        }

    for (auto& [key, entry] : zones)
    {
        auto hold = weatherHold_.find(key);
        if (hold != weatherHold_.end())
        {
            if (now < hold->second)
                continue;                                                   // the player's own sky, until the next spell
            weatherHold_.erase(hold);
        }
        WeatherFront& front = weatherFront_[key];
        if (front.rolled != spell && (front.type == WEATHER_TYPE_FINE || now >= front.end))
        {
            // The sky is clear and a new spell has begun: roll for a front.
            const bool night = WeatherNight();
            WeatherFront next = FrontFor(key.zone, spell, night);
            next.rolled = spell;
            next.sent = front.sent;
            next.applied = front.applied;
            const bool was = front.type != WEATHER_TYPE_FINE;
            front = next;
            if (front.type != WEATHER_TYPE_FINE)
                sLog.outString("Bridge: weather in %s: %s%s, first drops in %u min, peak %.2f, clear in %u min%s", ZoneName(key.zone),
                               front.start > front.gather ? "a storm gathers, " : "", WeatherWord(front.type),
                               uint32((front.start - now) / 60), front.peak, uint32((front.end - now) / 60), night ? " (night)" : "");
            else if (was)
                sLog.outString("Bridge: weather in %s: clear", ZoneName(key.zone));
        }
        if (!front.known)
            continue;                                                       // no game_weather row: the core's, always fine
        // Sent when it changes, and once on arriving in the zone, so the
        // core's own weather there never stands in for the front's.
        const WeatherNow at = WeatherAt(front, now);
        if (!front.applied || !weatherHere_.count(key) || at.type != front.sent.type || std::abs(at.grade - front.sent.grade) >= 0.02f)
        {
            entry.first->SetWeather(key.zone, WeatherType(at.type), at.grade, true);
            front.sent = at;
            front.applied = true;
        }
        Thunder(key, front, now, entry.second);
    }

    weatherHere_.clear();
    for (auto const& entry : zones)
        weatherHere_.insert(entry.first);

    // Fronts outlive a visit (the storm is still there when the party comes
    // back), and go when they are over.
    for (auto it = weatherFront_.begin(); it != weatherFront_.end();)
        it = !zones.count(it->first) && (it->second.type == WEATHER_TYPE_FINE || now >= it->second.end)
             ? weatherFront_.erase(it) : std::next(it);
}

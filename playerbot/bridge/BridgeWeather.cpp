#include "playerbot/bridge/Bridge.h"

#include "playerbot/PlayerbotAI.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/playerbot.h"

#include "Entities/Player.h"
#include "Globals/ObjectAccessor.h"
#include "Log/Log.h"
#include "Maps/Map.h"
#include "Server/DBCStores.h"
#include "Weather/Weather.h"
#include "World/World.h"

#include <algorithm>
#include <ctime>
#include <set>

// The weather (the owner, 19 October): the core's own cycle rerolls each
// zone every ten minutes against `game_weather`, and four times in five
// what it rolls is so light it does not show (grade under 0.33; the client
// draws nothing under about 0.27). In some twenty sessions it turned the
// sky once on its own, to rain at 0.02. So the bridge keeps the weather,
// by rule and nothing else; the narrator never decides it.
//
// The rule: the day is cut into spells of AiPlayerbot.Bridge.WeatherMinutes
// real minutes. Each zone with a real player in it gets, for each spell,
// the weather its own `game_weather` row gives the season, its chances
// times WeatherOdds, and times WeatherNight as well while the bridge's sky
// says night (TurnTheSky). Rain, snow or storm come at a grade that shows,
// from WeatherGrade up to 1, lighter more often than heavier. The roll is
// not random: it is a hash of the zone and the spell, so the same zone in
// the same spell, by the same rule, always has the same weather, and a
// change in the log can be worked out again from the config. The weather
// is set as permanent, so the core's own cycle never turns it in between;
// the next spell does. A zone with no `game_weather` row is left alone, as
// the core leaves it (always fine). WeatherMinutes 0: the core's cycle, as
// before.
namespace
{
    // splitmix64: a well-spread 64-bit hash of a 64-bit input.
    uint64 Mix(uint64 x)
    {
        x += 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
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

Bridge::WeatherCall Bridge::WeatherFor(uint32 zone, uint64 spell, bool night) const
{
    WeatherCall call;
    WeatherZoneChances const* chances = sWeatherMgr.GetWeatherChances(zone);
    if (!chances)
        return call;
    call.known = true;
    const time_t start = time_t(spell * uint64(sPlayerbotAIConfig.bridgeWeatherMinutes) * 60);
    WeatherSeasonChances const& season = chances->data[Season(start)];
    const double odds = sPlayerbotAIConfig.bridgeWeatherOdds * (night ? sPlayerbotAIConfig.bridgeWeatherNight : 1.0);

    const uint64 hash = Mix(uint64(zone) << 40 ^ spell);
    const double roll = double(hash % 10000) / 100.0;                       // 0 to 99.99
    const double rain = season.rainChance * odds;
    const double snow = rain + season.snowChance * odds;
    const double storm = snow + season.stormChance * odds;
    call.type = uint32(roll < rain ? WEATHER_TYPE_RAIN : roll < snow ? WEATHER_TYPE_SNOW : roll < storm ? WEATHER_TYPE_STORM : WEATHER_TYPE_FINE);
    if (call.type != WEATHER_TYPE_FINE)
    {
        const double least = std::min(std::max(double(sPlayerbotAIConfig.bridgeWeatherGrade), 0.0), 1.0);
        const double u = double((hash >> 32) % 1000) / 1000.0;
        call.grade = float(least + (1.0 - least) * u * u);                // lighter more often than heavier
    }
    return call;
}

void Bridge::TurnTheWeather(uint32 diff)
{
    if (!WeatherOn())
    {
        weatherSpell_.clear();
        return;
    }
    weatherTick_ += diff;
    if (weatherTick_ < 1000)
        return;
    weatherTick_ = 0;

    // Once a second: every zone with a real player in it has this spell's
    // weather, set once when the spell begins or the player arrives.
    const uint64 spell = uint64(time(nullptr)) / (uint64(sPlayerbotAIConfig.bridgeWeatherMinutes) * 60);
    const bool night = WeatherNight();
    std::set<WeatherZone> seen;
    for (Player* player : RealPlayersOnline())
    {
        Map* map = player->GetMap();
        if (!map)
            continue;
        const WeatherZone key {map->GetId(), map->GetInstanceId(), player->GetZoneId()};
        if (!seen.insert(key).second)
            continue;
        auto found = weatherSpell_.find(key);
        if (found != weatherSpell_.end() && found->second == spell)
            continue;
        weatherSpell_[key] = spell;
        const WeatherCall call = WeatherFor(key.zone, spell, night);
        if (!call.known)
            continue;
        map->SetWeather(key.zone, WeatherType(call.type), call.grade, true);
        AreaTableEntry const* entry = GetAreaEntryByAreaID(key.zone);
        sLog.outString("Bridge: weather in %s for the next %d min: %s %.2f%s", entry ? entry->area_name[0] : "zone",
                       sPlayerbotAIConfig.bridgeWeatherMinutes, WeatherWord(call.type), call.grade, night ? " (night)" : "");
    }
    for (auto it = weatherSpell_.begin(); it != weatherSpell_.end();)
        it = seen.count(it->first) ? std::next(it) : weatherSpell_.erase(it);
}

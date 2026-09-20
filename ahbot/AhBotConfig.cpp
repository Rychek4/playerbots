
#include "AhBotConfig.h"
#include "SystemConfig.h"
std::vector<std::string> split(const std::string &s, char delim);

INSTANTIATE_SINGLETON_1(AhBotConfig);

// Every field has a value before ahbot.conf is read, so a missing file means
// an auction bot that is off, not one running on whatever the heap held.
AhBotConfig::AhBotConfig() : enabled(false), guid(0), updateInterval(900), historyDays(30), maxSellInterval(3600 * 8),
    itemBuyMinInterval(600), itemBuyMaxInterval(7200), itemSellMinInterval(600), itemSellMaxInterval(7200),
    alwaysAvailableMoney(200000), priceMultiplier(1.0f), priceQualityMultiplier(1.0f), defaultMinPrice(20),
    stackReducePrice(1000000), maxItemLevel(199), maxRequiredLevel(80), underPriceProbability(0.05f), sendmail(true)
{
}

template <class T>
void LoadSet(std::string value, T &res)
{
    std::vector<std::string> ids = split(value, ',');
    for (std::vector<std::string>::iterator i = ids.begin(); i != ids.end(); i++)
    {
        uint32 id = atoi((*i).c_str());
        if (!id)
            continue;

        res.insert(id);
    }
}

bool AhBotConfig::Initialize()
{
    if (!config.SetSource(SYSCONFDIR"ahbot.conf", "AHBot_"))
    {
        sLog.outString("AhBot is Disabled. Unable to open configuration file ahbot.conf");
        return false;
    }

    enabled = config.GetBoolDefault("AhBot.Enabled", true);

    if (!enabled)
        sLog.outString("AhBot is Disabled in ahbot.conf");

    guid = (uint64)config.GetIntDefault("AhBot.GUID", 0);
    updateInterval = config.GetIntDefault("AhBot.UpdateIntervalInSeconds", 900);
    historyDays = config.GetIntDefault("AhBot.History.Days", 30);
    itemBuyMinInterval = config.GetIntDefault("AhBot.ItemBuyMinInterval", 600);
    itemBuyMaxInterval = config.GetIntDefault("AhBot.ItemBuyMaxInterval", 7200);
    itemSellMinInterval = config.GetIntDefault("AhBot.ItemSellMinInterval", 600);
    itemSellMaxInterval = config.GetIntDefault("AhBot.ItemSellMaxInterval", 7200);
    maxSellInterval = config.GetIntDefault("AhBot.MaxSellInterval", 3600 * 8);
    alwaysAvailableMoney = config.GetIntDefault("AhBot.AlwaysAvailableMoney", 200000);
    priceMultiplier = config.GetFloatDefault("AhBot.PriceMultiplier", 1.0f);
    defaultMinPrice = config.GetIntDefault("AhBot.DefaultMinPrice", 20);
    maxItemLevel = config.GetIntDefault("AhBot.MaxItemLevel", 199);
    maxRequiredLevel = config.GetIntDefault("AhBot.MaxRequiredLevel", 80);
    stackReducePrice = config.GetIntDefault("AhBot.StackReducePrice", 1000000);
    priceQualityMultiplier = config.GetFloatDefault("AhBot.PriceQualityMultiplier", 1.0f);
    underPriceProbability = config.GetFloatDefault("AhBot.UnderPriceProbability", 0.05f);
    LoadSet<std::set<uint32> >(config.GetStringDefault("AhBot.IgnoreItemIds", "49283,52200,8494,6345,6891,2460,37164,34835"), ignoreItemIds);
    LoadSet<std::set<uint32> >(config.GetStringDefault("AhBot.IgnoreVendorItemIds", "755,858,4592,4593,1710,3827,2455,3385"), ignoreVendorItemIds);
    sendmail = config.GetBoolDefault("AhBot.SendMail", true);

    // A zero interval would run the check on every world tick and flood the
    // console; nobody means that. Say what was read, and what will be used.
    if (enabled && updateInterval < 60)
    {
        sLog.outError("AhBot.UpdateIntervalInSeconds = %u in ahbot.conf is too small; using 900", updateInterval);
        updateInterval = 900;
    }
    if (enabled)
        sLog.outString("AhBot enabled (ahbot.conf); checking auctions every %u seconds", updateInterval);

    return enabled;
}

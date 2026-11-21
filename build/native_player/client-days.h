// Bunker's Android ClientDays.KEYS. The game caches these across calendar days.
namespace client_days {
static constexpr const char16_t *keys[] = {
    u"login_bonus_spanning_day_key", u"roulette_badge_spanning_day_key",
    u"mom_promotion_spanning_day_key", u"function_promotion_spanning_day_key",
    u"mom_promotion_day_play_count_key", u"function_promotion_day_play_count_key",
    u"gacha_day_first_draw_unix_time_key", u"gacha_draw_count_key",
    u"last_opened_shop_unix_time_key", u"last_display_tip_unix_time_key",
    u"last_display_tip_background_unix_time_key", u"daily_quest_message_lottery_unix_time_key",
    u"world_map_notification_view_unix_time_key", u"unknown_location_entry_unix_time_key",
    u"last_limited_open_notification_unix_time"
};
static auto marker() { return privateClient()/"server"/"client-days.reset"; }

static void beforePreference(State &state,void *fn) {
    struct Binding { Call call; void *address; };
    static const bool snapshot = _wgetenv(L"REINCOMP_CHECKPOINT_PREFS") != nullptr;
    static const auto calls = [] {
        std::unordered_map<std::string,Binding> result;
        auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"UnityPlayer.dll"));
        for (const auto &method : methods) {
            if (std::string_view(method.name).find("UnityEngine.PlayerPrefs::") != 0) continue;
            auto call = snapshot ? checkpoint_prefs::bind(method.name) : method.call;
            result.emplace(method.name,Binding{call,reinterpret_cast<void *>(base+method.rva)});
        }
        return result;
    }();
    // Do this on the first actual preference access, after Unity and IL2CPP
    // have initialized and before the game can cache an old day in memory.
    static const bool reset = [&] {
        if (snapshot) {
            {
                std::lock_guard guard(checkpoint_prefs::mutex);
                for (auto key : keys) checkpoint_prefs::values().erase(key);
            }
            checkpoint_prefs::save(state,nullptr);
        } else {
            auto erase = reinterpret_cast<void(*)(P)>(calls.at("UnityEngine.PlayerPrefs::DeleteKey").address);
            for (auto key : keys) {
                std::u16string_view name(key);
                erase(managedPath(std::wstring(name.begin(),name.end())));
            }
            reinterpret_cast<void(*)()>(calls.at("UnityEngine.PlayerPrefs::Save").address)();
        }
        std::error_code error;
        const bool removed = std::filesystem::remove(marker(),error);
        if(error) std::fprintf(stderr,"Could not remove Bunker day reset marker: %s\n",error.message().c_str());
        return removed;
    }();
    (void)reset;
    for (const auto &entry : calls) {
        if (entry.second.address == fn) {
            entry.second.call(state,fn);
            return;
        }
    }
    throw std::runtime_error("Unregistered Unity PlayerPrefs call");
}
}

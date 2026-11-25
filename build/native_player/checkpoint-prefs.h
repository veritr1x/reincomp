// The original emulator importer and the native checkpoint share one wire format.
namespace checkpoint_prefs {
using namespace bunker::windows;
static std::mutex mutex;
static bool dirty = false;
static auto file() { return std::filesystem::path(_wgetenv(L"REINCOMP_CHECKPOINT_PREFS")); }
static SharedPreferencesSnapshot load() {
    auto path = file();
    auto size = std::filesystem::file_size(path);
    if (size > 4 * 1024 * 1024) throw std::runtime_error("Oversized checkpoint preferences");
    std::vector<uint8_t> bytes(size);
    std::ifstream input(path, std::ios::binary);
    if (!input.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
        throw std::runtime_error("Cannot read checkpoint preferences");
    return DecodeSharedPreferencesSnapshot(bytes);
}
static SharedPreferencesSnapshot &values() {
    static auto store = load();
    return store;
}
static std::u16string stringValue(uint64_t object) {
    auto value = pathText(reinterpret_cast<P>(object));
    return std::u16string(value.begin(), value.end());
}
static void saveLocked() {
    auto path = file();
    auto pending = path;
    pending += L".tmp";
    auto bytes = EncodeSharedPreferencesSnapshot(values());
    std::ofstream out(pending, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    out.close();
    if (!out || !MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot save checkpoint preferences");
    dirty = false;
}
static void save(State &, void *) {
    std::lock_guard guard(mutex);
    saveLocked();
}
template<SharedPreferenceValue::Kind kind>
static void read(State &s, void *) {
    auto key = stringValue(s.gpr.x0.qword);
    SharedPreferenceValue value;
    bool found;
    {
        std::lock_guard guard(mutex);
        auto &store = values();
        auto item = store.find(key);
        found = item != store.end() && item->second.kind == kind;
        if (found) value = item->second;
    }
    if constexpr (kind == SharedPreferenceValue::String) {
        s.gpr.x0.qword = found ? reinterpret_cast<uintptr_t>(managedPath(
            std::wstring(value.text.begin(), value.text.end()))) : s.gpr.x1.qword;
    } else if constexpr (kind == SharedPreferenceValue::Integer) {
        s.gpr.x0.qword = found ? uint32_t(value.bits) : s.gpr.x1.dword;
    } else if (found) {
        auto bits = uint32_t(value.bits);
        std::memcpy(&s.simd.v[0], &bits, sizeof(bits));
    }
}
template<SharedPreferenceValue::Kind kind>
static void set(State &s, void *) {
    auto key = stringValue(s.gpr.x0.qword);
    SharedPreferenceValue value;
    value.kind = kind;
    if constexpr (kind == SharedPreferenceValue::String) value.text = stringValue(s.gpr.x1.qword);
    else if constexpr (kind == SharedPreferenceValue::Integer) value.bits = s.gpr.x1.dword;
    else std::memcpy(&value.bits, &s.simd.v[0], sizeof(uint32_t));
    std::lock_guard guard(mutex);
    values()[key] = std::move(value);
    dirty = true;
    s.gpr.x0.qword = 1;
}
static void has(State &s, void *) {
    auto key = stringValue(s.gpr.x0.qword);
    std::lock_guard guard(mutex);
    s.gpr.x0.qword = values().count(key);
}
static void erase(State &s, void *) {
    auto key = stringValue(s.gpr.x0.qword);
    std::lock_guard guard(mutex);
    if (values().erase(key)) dirty = true;
}
static void clear(State &, void *) {
    std::lock_guard guard(mutex);
    if (!values().empty()) {
        values().clear();
        dirty = true;
    }
}
static Call bind(std::string_view key) {
    constexpr std::string_view prefix = "UnityEngine.PlayerPrefs::";
    if (key.substr(0, prefix.size()) != prefix) return nullptr;
    key.remove_prefix(prefix.size());
    if (key == "Save") return save;
    if (key == "HasKey") return has;
    if (key == "DeleteKey") return erase;
    if (key == "DeleteAll") return clear;
    if (key == "GetString") return read<SharedPreferenceValue::String>;
    if (key == "GetInt") return read<SharedPreferenceValue::Integer>;
    if (key == "GetFloat") return read<SharedPreferenceValue::Float>;
    if (key == "TrySetSetString") return set<SharedPreferenceValue::String>;
    if (key == "TrySetInt") return set<SharedPreferenceValue::Integer>;
    if (key == "TrySetFloat") return set<SharedPreferenceValue::Float>;
    throw std::runtime_error("Unknown Unity PlayerPrefs operation");
}
}

#pragma once
#include <chrono>
#include <vector>
#include "unity-object.h"

// The Subquests picker creates a fixed-height button for every event. Its single
// "contents" child holds the art, masks, labels and animated campaign badges.
namespace subquest_list {
static std::atomic<P> lateUpdate{}, onDisable{};
static P tabType, setupField;
static std::atomic<P> eventContentClass{};
static P scrollType, contentSetup;

static void prepare() {
    if (lateUpdate.load(std::memory_order_acquire)) return;
    auto scroll = desktop_skip::type("Dark.View.UserInterface.ScrollView", "DarkScrollRect");
    auto tab = desktop_skip::type("Dark.View.UserInterface.Outgame", "VerticalTab");
    if (!scroll || !tab) return;
    scrollType = il2cpp_type_get_object(il2cpp_class_get_type(scroll));
    auto eventContent = desktop_skip::type("Dark.View.UserInterface.Outgame", "EventQuestTabContent");
    if (eventContent) contentSetup = il2cpp_class_get_method_from_name(eventContent, "Setup", 1);
    eventContentClass.store(eventContent, std::memory_order_release);
    tabType = il2cpp_type_get_object(il2cpp_class_get_type(tab));
    setupField = il2cpp_class_get_field_from_name(tab, "_isEndSetup");
    auto disable = il2cpp_class_get_method_from_name(scroll, "OnDisable", 0);
    if (!disable || !tabType || !setupField) return;
    onDisable.store(disable, std::memory_order_release);
    lateUpdate.store(il2cpp_class_get_method_from_name(scroll, "LateUpdate", 0), std::memory_order_release);
}

static bool isEventPicker(P scroll) {
    static auto transform = reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.Component::get_transform"));
    static auto parent = reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.Transform::GetParent"));
    static auto name = reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.Object::GetName"));
    P node = transform(scroll);
    for (auto expected : {L"vertical_tab", L"left_parts", L"all", L"event_quest_top"}) {
        if (!node || playerString(name(node)) != expected) return false;
        node = parent(node);
    }
    return true;
}

struct HiddenVisual {
    uint32_t handle;
    bool outside;
    uint32_t setupTarget = 0, setupData = 0;
};
struct List {
    bool eventPicker = false;
    bool buildingDormantRows = false;
    std::vector<P> rows;
    float warmup = 0;
    std::map<P, HiddenVisual> hidden;
};
// Unity delivers both scroll callbacks on the main thread.
static std::map<P, List> lists;

static void setActive(P object, bool active) {
    static auto set = reinterpret_cast<void(*)(P, uint8_t)>(registry.at("UnityEngine.GameObject::SetActive"));
    set(object, active);
}

static P rowArtwork(P row) {
    static auto transform = reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.Component::get_transform"));
    static auto count = reinterpret_cast<int(*)(P)>(registry.at("UnityEngine.Transform::GetChildCount"));
    static auto child = reinterpret_cast<P(*)(P, int)>(registry.at("UnityEngine.Transform::GetChild"));
    static auto object = reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.Component::get_gameObject"));
    auto root = transform(row);
    return count(root) == 1 ? object(child(root, 0)) : nullptr;
}

static P eventScroll(P parent) {
    static auto getParent = reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.Transform::GetParent"));
    static auto object = reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.Component::get_gameObject"));
    static auto component = reinterpret_cast<P(*)(P, P)>(registry.at("UnityEngine.GameObject::GetComponent"));
    P viewport = parent ? getParent(parent) : nullptr, tab = viewport ? getParent(viewport) : nullptr;
    return tab && scrollType ? component(object(tab), scrollType) : nullptr;
}

// Each row keeps its original button and fixed-height RectTransform. Clone
// only the first screenful with active artwork. Unity can bind the remaining
// rows normally without registering their text, masks and animations on entry.
extern "C" __declspec(dllexport) P CloneWithSubquestArtwork(P prefab, P parent, uint8_t worldPositionStays) {
    static auto clone = reinterpret_cast<P(*)(P, P, uint8_t)>(registry.at("UnityEngine.Object::Internal_CloneSingleWithParent"));
    auto contentClass = eventContentClass.load(std::memory_order_acquire);
    if (!contentClass || !prefab || !parent || readField<sizeof(P), 0>(prefab) != uintptr_t(contentClass))
        return clone(prefab, parent, worldPositionStays);

    static auto count = reinterpret_cast<int(*)(P)>(registry.at("UnityEngine.Transform::GetChildCount"));
    static auto active = reinterpret_cast<uint8_t(*)(P)>(registry.at("UnityEngine.GameObject::get_activeSelf"));
    if (count(parent) < 10) return clone(prefab, parent, worldPositionStays);
    P scroll = eventScroll(parent);
    if (!scroll) return clone(prefab, parent, worldPositionStays);
    auto known = lists.find(scroll);
    if (known == lists.end() ? !isEventPicker(scroll) : !known->second.eventPicker)
        return clone(prefab, parent, worldPositionStays);
    P templateVisual = rowArtwork(prefab);
    if (!templateVisual || !active(templateVisual)) return clone(prefab, parent, worldPositionStays);

    P row;
    {
        struct Restore { P visual; ~Restore() { setActive(visual, true); } } restore{templateVisual};
        setActive(templateVisual, false);
        row = clone(prefab, parent, worldPositionStays);
    }
    if (row) {
        P visual = rowArtwork(row);
        if (!visual) return row;
        auto handle = uint32_t(invoke("il2cpp_gchandle_new", visual, false));
        if (handle) {
            auto &list = lists[scroll];
            list.eventPicker = list.buildingDormantRows = true;
            list.hidden.emplace(visual, HiddenVisual{handle, true});
        } else setActive(visual, true);
    }
    return row;
}

// VerticalTab has already registered the real button. Defer only the event
// label, maintenance notice and campaign artwork bound by TabContent.Setup.
extern "C" __declspec(dllexport) bool DeferSubquestArtwork(P row, P data) {
    static auto transform = reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.Component::get_transform"));
    static auto parent = reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.Transform::GetParent"));
    if (!row || !data || !contentSetup) return false;
    auto list = lists.find(eventScroll(parent(transform(row))));
    if (list == lists.end()) return false;
    auto hidden = list->second.hidden.find(rowArtwork(row));
    if (hidden == list->second.hidden.end()) return false;
    auto &entry = hidden->second;
    auto dataHandle = uint32_t(invoke("il2cpp_gchandle_new", data, false));
    if (!dataHandle) return false;
    if (!entry.setupTarget) entry.setupTarget = uint32_t(invoke("il2cpp_gchandle_new", row, false));
    if (!entry.setupTarget) {
        invoke("il2cpp_gchandle_free", dataHandle);
        return false;
    }
    if (entry.setupData) invoke("il2cpp_gchandle_free", entry.setupData);
    entry.setupData = dataHandle;
    return true;
}

static void bindArtwork(HiddenVisual &entry) {
    if (!entry.setupData) return;
    P target = reinterpret_cast<P>(invoke("il2cpp_gchandle_get_target", entry.setupTarget));
    P data = reinterpret_cast<P>(invoke("il2cpp_gchandle_get_target", entry.setupData));
    if (nativeUnityObjectAlive(target) && data) {
        P exception = nullptr;
        invoke("il2cpp_runtime_invoke", contentSetup, target, &data, &exception);
        if (exception) invoke("il2cpp_unhandled_exception", exception);
    }
    invoke("il2cpp_gchandle_free", entry.setupData);
    invoke("il2cpp_gchandle_free", entry.setupTarget);
    entry.setupData = entry.setupTarget = 0;
}

static void restore(List &list, bool all) {
    for (auto it = list.hidden.begin(); it != list.hidden.end();) {
        if (!all && it->second.outside) {
            ++it;
            continue;
        }
        if (nativeUnityObjectAlive(it->first)) setActive(it->first, true);
        bindArtwork(it->second);
        invoke("il2cpp_gchandle_free", it->second.handle);
        it = list.hidden.erase(it);
    }
}

static void disable(P scroll) {
    auto entry = lists.extract(scroll);
    if (!entry.empty()) restore(entry.mapped(), true);
}

static void update(P scroll) {
    if (!nativeUnityObjectAlive(scroll)) return;
    auto [it, added] = lists.try_emplace(scroll);
    List &list = it->second;
    if (added) list.eventPicker = isEventPicker(scroll);
    if (!list.eventPicker) return;

    static auto count = reinterpret_cast<int(*)(P)>(registry.at("UnityEngine.Transform::GetChildCount"));
    static auto child = reinterpret_cast<P(*)(P, int)>(registry.at("UnityEngine.Transform::GetChild"));
    static auto object = reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.Component::get_gameObject"));
    static auto component = reinterpret_cast<P(*)(P, P)>(registry.at("UnityEngine.GameObject::GetComponent"));
    static auto active = reinterpret_cast<uint8_t(*)(P)>(registry.at("UnityEngine.GameObject::get_activeSelf"));
    static auto rect = reinterpret_cast<void(*)(P, float*)>(registry.at("UnityEngine.RectTransform::get_rect_Injected"));
    static auto position = reinterpret_cast<void(*)(P, float*)>(registry.at("UnityEngine.Transform::get_position_Injected"));
    static auto toLocal = reinterpret_cast<void(*)(P, const float*, float*)>(registry.at("UnityEngine.Transform::InverseTransformPoint_Injected"));
    static auto delta = reinterpret_cast<float(*)()>(registry.at("UnityEngine.Time::get_deltaTime"));

    P content = screenObjectField(scroll, "_content"), viewport = screenObjectField(scroll, "_viewPort");
    int rows = nativeUnityObjectAlive(content) ? count(content) : 0;
    bool changed = list.rows.size() != size_t(rows);
    list.rows.resize(rows);
    for (int i = 0; i < rows; ++i) {
        P row = child(content, i);
        if (list.rows[i] != row) changed = true;
        list.rows[i] = row;
    }
    if (changed && !list.buildingDormantRows) {
        restore(list, true);
        list.warmup = 0;
    }
    if (!rows || !nativeUnityObjectAlive(viewport)) {
        restore(list, true);
        return;
    }
    // Badge entry animations bind their artwork after the buttons are created.
    // Give the populated list time to play them, including on slow frames.
    uint8_t ready = 0;
    if (P tab = component(object(scroll), tabType))
        invoke("il2cpp_field_get_value", tab, setupField, &ready);
    if (!ready) {
        if (!list.buildingDormantRows) restore(list, true);
        list.warmup = 0;
        return;
    }
    if (list.buildingDormantRows) {
        list.buildingDormantRows = false;
        list.warmup = 2.f;
    }
    if (list.warmup < 2.f) {
        list.warmup += std::min(delta(), .1f);
        return;
    }

    float view[4];
    rect(viewport, view);
    for (auto &entry : list.hidden) entry.second.outside = false;
    // SetActive dispatches OnDisable through each row's UI hierarchy. Spread
    // that cost over frames on entry. Rows coming into view always return now.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
    bool canHide = true;
    unsigned newlyHidden = 0;
    for (P row : list.rows) {
        if (view[3] <= 0) break;
        if (count(row) != 1) continue;
        float box[4], world[3], local[3];
        rect(row, box);
        position(row, world);
        toLocal(viewport, world, local);
        // Leave one extra row active at either edge for overhanging artwork.
        float height = box[3], bottom = local[1] + box[1];
        if (bottom + 2 * height >= view[1] && bottom - height <= view[1] + view[3]) continue;
        P visual = object(child(row, 0));
        auto hidden = list.hidden.find(visual);
        if (hidden != list.hidden.end()) {
            hidden->second.outside = true;
        } else if (canHide && active(visual)) {
            auto handle = uint32_t(invoke("il2cpp_gchandle_new", visual, false));
            if (!handle) continue;
            list.hidden.emplace(visual, HiddenVisual{handle, true});
            setActive(visual, false);
            canHide = ++newlyHidden < 8 && std::chrono::steady_clock::now() < deadline;
        }
    }
    // Also restore any rows removed or pooled since the previous layout.
    restore(list, false);
    // Give visible rows priority. Bind one more offscreen row per frame so
    // fast scrolling and leaving the picker do not accumulate deferred work.
    for (auto &[visual, entry] : list.hidden) {
        if (entry.setupData) {
            bindArtwork(entry);
            break;
        }
    }
}
}

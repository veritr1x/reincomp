#pragma once
#include <chrono>
#include <vector>
#include "unity-object.h"

// The Subquests picker creates a fixed-height button for every event. Its single
// "contents" child holds the art, masks, labels and animated campaign badges.
namespace subquest_list {
static std::atomic<P> lateUpdate{}, onDisable{};
static P tabType, setupField;

static void prepare() {
    if (lateUpdate.load(std::memory_order_acquire)) return;
    auto scroll = desktop_skip::type("Dark.View.UserInterface.ScrollView", "DarkScrollRect");
    auto tab = desktop_skip::type("Dark.View.UserInterface.Outgame", "VerticalTab");
    if (!scroll || !tab) return;
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
};
struct List {
    bool eventPicker = false;
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

static void restore(List &list, bool all) {
    for (auto it = list.hidden.begin(); it != list.hidden.end();) {
        if (!all && it->second.outside) {
            ++it;
            continue;
        }
        if (nativeUnityObjectAlive(it->first)) setActive(it->first, true);
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
    if (changed) {
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
        restore(list, true);
        list.warmup = 0;
        return;
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
}
}

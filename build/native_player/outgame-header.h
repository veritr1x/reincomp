#include <algorithm>
#include <cmath>
#include "unity-object.h"

// GetWorldCorners computes the four local rect corners and transforms them
// with localToWorldMatrix. Bunker's spacing calculation uses only these four
// coordinates. Read Unity's current rect and matrix directly on the UI thread.
extern "C" int desktop_header_corners(P transform, float out[4]) {
    if (!nativeUnityObjectAlive(transform)) return -1;
    using Read = void(*)(P, float *);
    static auto rect = reinterpret_cast<Read>(registry.at("UnityEngine.RectTransform::get_rect_Injected"));
    static auto matrix = reinterpret_cast<Read>(registry.at("UnityEngine.Transform::get_localToWorldMatrix_Injected"));
    float box[4], m[16];
    rect(transform, box);
    matrix(transform, m);
    const float left = box[0], bottom = box[1], right = left + box[2], top = bottom + box[3];
    out[0] = m[0]*left + m[4]*bottom + m[12];
    out[1] = m[1]*left + m[5]*bottom + m[13];
    out[2] = m[0]*right + m[4]*top + m[12];
    out[3] = m[1]*left + m[5]*top + m[13];
    return 0;
}

extern "C" P desktop_header_counters(P scope, P counterType) {
    using Components = P(*)(P, P, uint8_t, uint8_t, uint8_t, uint8_t, P);
    static auto components = reinterpret_cast<Components>(registry.at("UnityEngine.GameObject::GetComponentsInternal"));
    // The spacing loop consumes active controls only. Omit inactive subtrees
    // from this traversal while retaining the original per-counter check.
    return components(scope, counterType, true, true, false, false, nullptr);
}

static bool desktopHeaderComponentVisible(P component) {
    static auto object = reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.Component::get_gameObject"));
    static auto active = reinterpret_cast<uint8_t(*)(P)>(registry.at("UnityEngine.GameObject::get_activeInHierarchy"));
    return nativeUnityObjectAlive(component) && active(object(component));
}

extern "C" int desktop_header_row_visible(P row) { return desktopHeaderComponentVisible(row); }

extern "C" int desktop_header_counter_corners(P counter, float out[4]) {
    if (!desktopHeaderComponentVisible(counter)) return -1;
    static auto transform = reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.Component::get_transform"));
    return desktop_header_corners(transform(counter), out);
}

extern "C" int desktop_header_buttons_shown(P row) {
    static auto count = reinterpret_cast<int(*)(P)>(registry.at("UnityEngine.Transform::GetChildCount"));
    static auto child = reinterpret_cast<P(*)(P, int)>(registry.at("UnityEngine.Transform::GetChild"));
    static auto object = reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.Component::get_gameObject"));
    static auto active = reinterpret_cast<uint8_t(*)(P)>(registry.at("UnityEngine.GameObject::get_activeSelf"));
    int shown = 0;
    for (int i = 0, n = count(row); i < n; ++i) shown += active(object(child(row, i))) != 0;
    return shown;
}

// The nested button row drives its own width. After Bunker changes its
// spacing, the outer layout can retain the previous width until a tab change.
extern "C" void desktop_refresh_header_layout(P row) {
    static const auto rebuilder = [] {
        auto assembly = il2cpp_domain_assembly_open(il2cpp_domain_get(), "UnityEngine.UI");
        return assembly ? il2cpp_class_from_name(il2cpp_assembly_get_image(assembly),
            "UnityEngine.UI", "LayoutRebuilder") : nullptr;
    }();
    if (!rebuilder) return;
    static auto rebuild = il2cpp_class_get_method_from_name(rebuilder, "ForceRebuildLayoutImmediate", 1);
    static auto mark = il2cpp_class_get_method_from_name(rebuilder, "MarkLayoutForRebuild", 1);
    static auto getParent = reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.Transform::GetParent"));
    auto container = getParent(row);
    if (!rebuild || !mark || !container) return;
    P error = nullptr;
    invoke("il2cpp_runtime_invoke", rebuild, nullptr, &row, &error);
    if (!error) invoke("il2cpp_runtime_invoke", mark, nullptr, &container, &error);
}

// The extra Bunker button reduces the space between the localized title
// and the right-aligned counter group. Measure the original layout so tabs
// that already fit retain their normal sizes, including after a resize.
static void fitOutgameHeader() {
    static ULONGLONG next = 0;
    const auto now = GetTickCount64();
    if (now < next) return;
    next = now + 200;

    static const auto type = [] {
        auto assembly = il2cpp_domain_assembly_open(il2cpp_domain_get(), "Assembly-CSharp");
        return assembly ? il2cpp_class_from_name(il2cpp_assembly_get_image(assembly),
            "Dark.View.HeadUpDisplay.Header", "OutGameHeader") : nullptr;
    }();
    if (!type) return;
    static auto instance = il2cpp_class_get_method_from_name(type, "get_Instance", 0);
    static auto titlesField = il2cpp_class_get_field_from_name(type, "_titleTexts");
    static auto countersField = il2cpp_class_get_field_from_name(type, "_headerParameters");
    if (!instance || !titlesField || !countersField) return;
    P error = nullptr;
    auto header = reinterpret_cast<P>(invoke("il2cpp_runtime_invoke", instance, nullptr, nullptr, &error));
    if (error || !nativeUnityObjectAlive(header)) return;
    static auto gameObject = reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.Component::get_gameObject"));
    static auto active = reinterpret_cast<uint8_t(*)(P)>(registry.at("UnityEngine.GameObject::get_activeInHierarchy"));
    if (!active(gameObject(header))) return;

    P titles = nullptr, counters = nullptr;
    invoke("il2cpp_field_get_value", header, titlesField, &titles);
    invoke("il2cpp_field_get_value", header, countersField, &counters);
    if (!titles || !nativeUnityObjectAlive(counters) || !active(gameObject(counters))) return;
    const auto count = invoke("il2cpp_array_length", titles);
    if (!count) return;
    auto texts = reinterpret_cast<P const *>(static_cast<char *>(titles) + il2cpp_array_object_header_size());
    if (!nativeUnityObjectAlive(texts[0]) || !active(gameObject(texts[0]))) return;

    using Read = void(*)(P, float *);
    static auto transform = reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.Component::get_transform"));
    static auto position = reinterpret_cast<Read>(registry.at("UnityEngine.Transform::get_position_Injected"));
    static auto rect = reinterpret_cast<Read>(registry.at("UnityEngine.RectTransform::get_rect_Injected"));
    static auto parent = reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.Transform::GetParent"));
    static auto worldScale = reinterpret_cast<Read>(registry.at("UnityEngine.Transform::get_lossyScale_Injected"));
    static auto localScale = reinterpret_cast<Read>(registry.at("UnityEngine.Transform::get_localScale_Injected"));
    static auto setScale = reinterpret_cast<Read>(registry.at("UnityEngine.Transform::set_localScale_Injected"));
    P title = transform(texts[0]), parts = transform(counters);
    float left[3], right[3], titleRect[4], partsRect[4], titleParent[3], partsParent[3];
    position(title, left);
    position(parts, right);
    rect(title, titleRect);
    rect(parts, partsRect);
    auto titleContainer = parent(title), partsContainer = parent(parts);
    if (!titleContainer || !partsContainer) return;
    worldScale(titleContainer, titleParent);
    worldScale(partsContainer, partsParent);
    // Title layers pivot at the left and header_parts pivots at the right.
    const float titleWidth = titleRect[2] * titleParent[0];
    const float partsWidth = partsRect[2] * partsParent[0];
    const float space = right[0] - left[0] - 8.f * partsParent[0];
    if (titleWidth <= 0 || partsWidth <= 0 || space <= 0) return;
    const float fit = std::min(1.f, space / (titleWidth + partsWidth));
    auto resize = [&](P element) {
        float scale[3];
        localScale(element, scale);
        if (std::fabs(scale[0] - fit) < .001f && std::fabs(scale[1] - fit) < .001f) return;
        scale[0] = scale[1] = fit;
        setScale(element, scale);
    };
    resize(parts);
    for (uint32_t i = 0; i < count; ++i) if (nativeUnityObjectAlive(texts[i])) resize(transform(texts[i]));
}

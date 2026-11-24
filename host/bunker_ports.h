#pragma once

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bunker::ports {

inline unsigned windowsOffset() {
    static const unsigned offset = [] {
        const char *value = std::getenv("REINCOMP_BUNKER_PORT_OFFSET");
        if (!value)
            return std::getenv("REINCOMP_ISOLATED_SERVER") ? 31000u : 30000u;
        std::string_view text(value);
        unsigned number = 0;
        const char *end = value + text.size();
        auto result = std::from_chars(value, end, number);
        if (result.ec != std::errc{} || result.ptr != end || number > 65535 - 8080)
            throw std::invalid_argument("Invalid Bunker port offset");
        return number;
    }();
    return offset;
}

// Unity's packaged base URL uses 38080. Octo can also reuse a resource
// catalog saved while a different asset port was active.
template<class Char>
bool routeAssetUrl(std::basic_string<Char> &url, unsigned port, bool fromOctoCatalog = false) {
    constexpr std::string_view prefix = "http://127.0.0.1:";
    if (url.size() <= prefix.size() || !std::equal(prefix.begin(), prefix.end(), url.begin()))
        return false;
    auto end = prefix.size();
    unsigned old = 0;
    while (end < url.size() && url[end] >= '0' && url[end] <= '9') {
        old = old * 10 + (url[end++] - '0');
        if (old > 65535) return false;
    }
    if (!old || (end < url.size() && url[end] != '/' && url[end] != '?' && url[end] != '#'))
        return false;
    if (old == port || (old != 38080 && !fromOctoCatalog)) return false;
    auto digits = std::to_string(port);
    url.replace(url.begin() + prefix.size(), url.begin() + end, digits.begin(), digits.end());
    return true;
}
} // namespace bunker::ports

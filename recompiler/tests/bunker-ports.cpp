#include "bunker_ports.h"
#include <cassert>

int main() {
    using bunker::ports::routeAssetUrl;
    std::wstring web = L"http://127.0.0.1:38080/web/anecdote?id=3#panel";
    assert(routeAssetUrl(web, 11945));
    assert(web == L"http://127.0.0.1:11945/web/anecdote?id=3#panel");
    assert(!routeAssetUrl(web, 11945));

    std::string cached = "http://127.0.0.1:11945/catalog/file";
    assert(!routeAssetUrl(cached, 12080));
    assert(routeAssetUrl(cached, 12080, true));
    assert(cached == "http://127.0.0.1:12080/catalog/file");

    for (std::string other : {"http://example.com:38080/", "http://127.0.0.1:38080@example.com/",
                            "http://127.0.0.1:65536/x", "http://127.0.0.1:/x"}) {
        auto original = other;
        assert(!routeAssetUrl(other, 11945, true));
        assert(other == original);
    }
}

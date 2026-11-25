#include <windows.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

using addr_t = uintptr_t;
struct Memory {};
union Register { addr_t qword; int32_t dword; };
struct State { struct { Register x0, x8, x19, x30, pc; } gpr; };
namespace recomp {
using Entry = Memory *(*)(State &, addr_t, Memory *);
static Entry getAll;
static void AddGuestDirectEntry(addr_t address, Entry entry) {
    if (address == 0x847c006c) getAll = entry;
}
}
static DWORD owner;
struct Threads { void Run(addr_t, std::initializer_list<addr_t>) {} } *threads;
#include "../master-table-queries.h"

using namespace master_table_queries;
static Range original;
extern "C" Memory *sub_847c006c(State &state, addr_t, Memory *memory) {
    std::memcpy(reinterpret_cast<void *>(state.gpr.x8.qword), &original, sizeof(original));
    state.gpr.pc = state.gpr.x30;
    return memory;
}
extern "C" Memory *sub_827bf094(State &, addr_t, Memory *memory) { return memory; }

int main() {
    install();
    assert(recomp::getAll);
    struct Row { addr_t header[2]; int32_t quest; };
    struct Array { addr_t header[3], length; Row *items[4]; };
    Row a{{}, 7}, b{{}, 9}, c{{}, 7}, d{{}, 11};
    Array table{{}, 4, {&a, &b, &c, &d}};
    original = {reinterpret_cast<addr_t>(&table), 0, 3, 1, 1};
    auto query = [&](int32_t id, addr_t caller = eventQuestSchedule) {
        Row predicate{{}, id};
        Range result{};
        State state{};
        state.gpr.x8.qword = reinterpret_cast<addr_t>(&result);
        state.gpr.x19.qword = reinterpret_cast<addr_t>(&predicate);
        state.gpr.x30.qword = caller;
        recomp::getAll(state, 0x847c006c, nullptr);
        assert(state.gpr.pc.qword == caller && result.array == original.array);
        return result;
    };

    // The original Where retains both schedules even when another ID lies between them.
    auto duplicate = query(7);
    assert(duplicate.hasValue && duplicate.first == 0 && duplicate.last == 2);
    auto last = query(11);
    assert(last.hasValue && last.first == 3 && last.last == 3);
    assert(!query(13).hasValue);
    assert(query(7, 0x1234).last == 3);
    table.items[2] = nullptr;
    assert(query(11).first == 0 && query(11).last == 3);
}

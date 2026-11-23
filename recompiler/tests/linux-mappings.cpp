#include "../../build/linux_player/posix_files.h"
#include <cassert>
#include <cstdio>

int main() {
    linux_files::Mappings memory;
    const auto page=static_cast<size_t>(sysconf(_SC_PAGESIZE));
    constexpr int flags=MAP_PRIVATE|MAP_ANONYMOUS;
    auto heap=static_cast<char *>(memory.map(nullptr,3*page,PROT_READ|PROT_WRITE,flags,-1,0));
    assert(heap!=MAP_FAILED);
    heap[0]=1; heap[page]=2; heap[2*page]=3;
    assert(memory.map(heap+page,page,PROT_NONE,flags|MAP_FIXED,-1,0)==heap+page);
    assert(memory.map(heap+page,page,PROT_READ|PROT_WRITE,flags|MAP_FIXED,-1,0)==heap+page);
    assert(heap[0]==1 && heap[page]==0 && heap[2*page]==3);
    assert(memory.unmap(heap+page,page)==0);
    auto native=mmap(heap+page,page,PROT_READ|PROT_WRITE,flags|MAP_FIXED_NOREPLACE,-1,0);
    assert(native==heap+page);
    heap[page]=42;
    assert(memory.map(heap,3*page,PROT_NONE,flags|MAP_FIXED,-1,0)==MAP_FAILED);
    assert(heap[page]==42);
    assert(memory.unmap(heap,page)==0 && memory.unmap(heap+2*page,page)==0);
    assert(munmap(native,page)==0);
    std::puts("PASS collector pages can be discarded and restored without replacing neighbouring native memory");
}

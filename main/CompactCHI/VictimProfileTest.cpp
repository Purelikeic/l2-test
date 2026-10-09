#include "VictimProfile.hpp"
#include <cassert>
#include <cstdio>

using compactchi::VictimProfile;
using compactchi::EbRequest;
static EbRequest read(unsigned i) { return {1, 8, 0, i == 0 ? 7U : 0x26U, VictimProfile::requestAddress(i)}; }
static EbRequest release(unsigned i) { return {1, 8, 0, i == 0 ? 0x1bU : 0x42U, VictimProfile::address(i)}; }
static VictimProfile primed() {
    VictimProfile p;
    for (unsigned i = 0; i < 5; ++i) p.observe(read(i));
    return p;
}
template<class F> void rejects(F action) {
    bool rejected = false;
    try { action(); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
}
int main() {
    VictimProfile profile;
    assert(!profile.complete());
    for (unsigned i = 0; i < 4; ++i) profile.observe(read(i));
    profile.dirtyStored = true;
    // Independent explicit branch expectations: dirty, clean Comp, clean DBID,
    // clean Comp, clean DBID. Do not synthesize vectors from observed DUT output.
    const bool copyback[] = {false, false, true, false, true};
    for (unsigned i = 0; i < 5; ++i) {
        profile.observe(read(i + 4));
        const auto plan = profile.observe(release(i));
        assert(plan.cleanCopyback == copyback[i]);
    }
    assert(profile.complete());
    rejects([&] { profile.observe(read(9)); }); // F must be a resident hit.
    rejects([&] { profile.observe(release(5)); });
    rejects([] { VictimProfile p; p.observe(read(1)); });
    rejects([] { VictimProfile p; auto r = read(0); r.opcode = 0x26; p.observe(r); });
    rejects([] { auto p = primed(); p.observe(release(0)); }); // no planned dirty store
    rejects([] { auto p = primed(); p.dirtyStored = true; p.observe(release(1)); });
    rejects([] { auto p = primed(); p.dirtyStored = true; auto r = release(0); r.opcode = 0x42; p.observe(r); });
    rejects([] { VictimProfile p; auto r = read(0); r.address += 128; p.observe(r); });
    for (unsigned seed : {42U, 7U}) {
        const auto before = VictimProfile::initial(0x1000, seed);
        const auto after = VictimProfile::dirty(seed);
        assert(after[0] == (before[0] ^ 0xa5) && after[63] == (before[63] ^ 0x5a));
        for (unsigned i = 1; i < 63; ++i) assert(after[i] == before[i]);
    }
    std::puts("victim reference vectors passed; 8 invalid observations rejected");
}

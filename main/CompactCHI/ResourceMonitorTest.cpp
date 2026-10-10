#include "ResourceMonitor.hpp"
#include <cstdio>
#include <stdexcept>

static void require(bool value) { if (!value) throw std::runtime_error("resource monitor mutation passed unexpectedly"); }
template<class F> void rejects(F action) {
    bool rejected = false;
    try { action(); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected);
}
int main(int argc, char** argv) {
    require(argc == 2);
    for (unsigned field = 0; field < 10; ++field) {
        compactchi::ResourceMonitor monitor(std::string(argv[1]) + "/resource-monitor-" + std::to_string(field) + ".jsonl");
        compactchi::ResourceSnapshot sample{}; sample.ready = true; sample.counts[field] = 1;
        monitor.sample(8, 16, sample);
        rejects([&] { monitor.finish(); }); // A context/queue may be live while all ports are quiet.
        sample.counts[field] = 0;
        monitor.sample(9, 18, sample);
        monitor.finish();
    }
    compactchi::ResourceMonitor none(std::string(argv[1]) + "/resource-monitor-none.jsonl");
    rejects([&] { none.finish(); });
    compactchi::ResourceSnapshot zero{}; zero.ready = true;
    none.sample(8, 16, zero); rejects([&] { none.finish(); });
    rejects([&] { none.sample(10, 20, zero); });
    std::puts("resource monitor: ten live ownership layers, zero-activity and sampling gap rejected; terminal drain passed");
}

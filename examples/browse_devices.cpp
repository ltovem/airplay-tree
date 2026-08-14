/*!
 * @brief airplay2_browse - Scan the LAN for AirPlay devices via mDNS
 */
#include <airplay2/airplay2.h>
#include "../src/mdns/mdns_browser.h"
#include <iostream>
#include <atomic>
#include <csignal>
#include <chrono>
#include <thread>
#include <iomanip>

using namespace airplay2;

static std::atomic<bool> g_running{true};
static void on_sigint(int) { g_running.store(false); }

int main() {
    std::cout << "AirPlay device scanner (Ctrl+C to stop)\n";
    AirPlayServer::global_init();

    MdnsBrowser browser;
    std::mutex print_mu;
    browser.start([&](const DiscoveredAirPlayDevice& d, bool added) {
        std::lock_guard<std::mutex> lk(print_mu);
        const char* tag = added ? "[NEW]" : "[UPD]";
        std::cout << tag << " " << std::setw(32) << std::left << d.name
                  << "  " << std::setw(18) << (d.ip + ":" + std::to_string(d.port))
                  << "  vv=" << d.vv << "  model=" << d.model
                  << "  devid=" << d.device_id << "\n";
    });

    std::signal(SIGINT, on_sigint);
#ifdef _WIN32
    std::signal(SIGBREAK, on_sigint);
#endif

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto devs = browser.devices();
        std::lock_guard<std::mutex> lk(print_mu);
        std::cout << "\r  " << devs.size() << " devices discovered" << std::flush;
    }
    std::cout << "\n";

    browser.stop();
    AirPlayServer::global_cleanup();
    return 0;
}

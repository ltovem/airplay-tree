/*!
 * @file mdns_browser.h
 * @brief mDNS-based AirPlay service browser (discover other AirPlay devices)
 */
#ifndef AIRPLAY2_MDNS_BROWSER_H
#define AIRPLAY2_MDNS_BROWSER_H

#include "platform/platform_socket.h"
#include "platform/platform_thread.h"
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

namespace airplay2 {

struct DiscoveredAirPlayDevice {
    std::string name;
    std::string ip;
    uint16_t    port = 0;
    std::string device_id;
    std::string model;
    uint32_t    features = 0;
    uint64_t    last_seen_us = 0;
    int         vv = 0;   // AirPlay version (2=AP2, 1=legacy)
};

using DeviceDiscoveredCb = std::function<void(const DiscoveredAirPlayDevice&, bool added)>;

class MdnsBrowser {
public:
    MdnsBrowser();
    ~MdnsBrowser();

    bool start(DeviceDiscoveredCb cb);
    void stop();

    /// Snapshot of currently known devices
    std::vector<DiscoveredAirPlayDevice> devices() const;

private:
    void worker();
    void parse_response(const uint8_t* pkt, size_t len);

    std::atomic<bool> running_{false};
    platform::Thread worker_;
    platform::Socket sock4_;
    DeviceDiscoveredCb cb_;
    uint64_t last_query_us_ = 0;

    mutable std::mutex mu_;
    std::map<std::string, DiscoveredAirPlayDevice> devices_;
};

} // namespace airplay2

#endif // AIRPLAY2_MDNS_BROWSER_H

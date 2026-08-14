/*!
 * @file mdns_publisher.h
 * @brief mDNS / Bonjour service publisher for AirPlay advertisement
 */
#ifndef AIRPLAY2_MDNS_PUBLISHER_H
#define AIRPLAY2_MDNS_PUBLISHER_H

#include "../include/airplay2/airplay_config.h"
#include "platform/platform_socket.h"
#include "platform/platform_thread.h"
#include <map>
#include <string>
#include <vector>
#include <memory>

namespace airplay2 {

/*!
 * @brief Publishes AirPlay-related mDNS services.
 *
 * Publishes two services:
 *   _airplay._tcp  (control)
 *   _raop._tcp     (audio streaming, legacy + AP2)
 */
class MdnsPublisher {
public:
    MdnsPublisher();
    ~MdnsPublisher();

    /*!
     * @brief Start advertising AirPlay services
     * @param device Device info (name, device_id, port, features)
     * @param if_ipv4 IPv4 address to advertise (0.0.0.0 = auto-detect)
     * @return true on success
     */
    bool start(const DeviceInfo& device, const std::string& if_ipv4 = "0.0.0.0");

    /// Stop advertising
    void stop();

    bool is_running() const { return running_.load(); }

private:
    // ---- Minimal standalone mDNS responder (UDP 224.0.0.251:5353) ----
    void mdns_worker();
    void send_announcements(bool goodbye = false);
    void handle_query(const uint8_t* pkt, size_t len, const platform::SocketAddr& from);
    static std::vector<uint8_t> build_service_record(
        const std::string& name, const std::string& type, const std::string& domain,
        uint16_t port, const std::map<std::string,std::string>& txt,
        uint32_t ttl, bool goodbye, const std::string& a_record_ip);

    std::atomic<bool> running_{false};
    platform::Thread worker_;
    platform::Socket sock4_;
    DeviceInfo device_;
    std::string adv_ip_;
    std::string hostname_;
    uint64_t announce_timer_us_ = 0;
    int announce_count_ = 0;
};

} // namespace airplay2

#endif // AIRPLAY2_MDNS_PUBLISHER_H

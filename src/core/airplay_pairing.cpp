/*!
 * @file airplay_pairing.cpp
 */
#include "airplay_pairing.h"
#include "../platform/platform_log.h"
#include <cstring>
#include <string>     // std::stoi（PIN 码数值解析）

namespace airplay2 {

bool AirPlayPairing::is_paired(const std::string& client_ip) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (pin_.empty()) return true; // no auth required
    auto it = paired_ips_.find(client_ip);
    return it != paired_ips_.end() && it->second;
}

void AirPlayPairing::mark_paired(const std::string& client_ip) {
    std::lock_guard<std::mutex> lk(mu_);
    paired_ips_[client_ip] = true;
    AP2_LOGI("pairing: marked paired %s", client_ip.c_str());
}

void AirPlayPairing::unpair(const std::string& client_ip) {
    std::lock_guard<std::mutex> lk(mu_);
    paired_ips_.erase(client_ip);
}

std::vector<uint8_t> AirPlayPairing::handle_pair_setup(
    const std::string& client_ip, const uint8_t* body, size_t len, bool& needs_pin) {
    // In real AirPlay, this is MFi-SAP / FairPlay auth. We implement a
    // dummy PIN flow: clients either skip, or send the 4-digit PIN as text.
    (void)client_ip; (void)body; (void)len;
    needs_pin = false;
    std::lock_guard<std::mutex> lk(mu_);
    if (pin_.empty()) {
        // Accept immediately: return dummy "state=2" response (1 byte)
        return {0x02};
    }
    needs_pin = true;
    // Return "state=1, need pin" response (placeholder)
    return {0x01, 0x00};
}

std::vector<uint8_t> AirPlayPairing::handle_pair_verify(
    const std::string& client_ip, const uint8_t* body, size_t len, bool& pin_ok) {
    pin_ok = false;
    // If client sent PIN digits as ASCII: extract them
    std::string sent;
    for (size_t i = 0; i < len; ++i) {
        if (body[i] >= '0' && body[i] <= '9') sent.push_back((char)body[i]);
    }
    std::lock_guard<std::mutex> lk(mu_);
    if (pin_.empty()) {
        pin_ok = true;
    } else if (!sent.empty() && sent == pin_) {
        pin_ok = true;
        paired_ips_[client_ip] = true;
    } else if (pin_cb_) {
        if (pin_cb_(client_ip, sent)) {
            pin_ok = true;
            paired_ips_[client_ip] = true;
        }
    }
    if (pin_ok) return {0x00, 0x01}; // ok
    return {0x03, 0x00}; // fail (requires re-pair)
}

} // namespace airplay2

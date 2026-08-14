/*!
 * @file airplay_pairing.h
 * @brief Simple AirPlay PIN-code pairing (non-FairPlay fallback)
 *
 * The Apple AirPlay protocol supports two pairing modes:
 *   - PIN-based pairing (user types code shown on receiver into sender)
 *   - MFi chip authentication (requires Apple hardware)
 *
 * This module implements the PIN-based "legacy" pairing flow. Clients that
 * enforce FairPlay will fail; open-source and older clients work fine.
 */
#ifndef AIRPLAY2_AIRPLAY_PAIRING_H
#define AIRPLAY2_AIRPLAY_PAIRING_H

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace airplay2 {

class AirPlayPairing {
public:
    AirPlayPairing() = default;

    /// Set the PIN code clients must enter (4 digits, empty = no auth)
    void set_pin(const std::string& pin) {
        std::lock_guard<std::mutex> lk(mu_);
        pin_ = pin;
    }
    std::string pin() const {
        std::lock_guard<std::mutex> lk(mu_); return pin_;
    }

    /// Is this client IP currently paired / whitelisted?
    bool is_paired(const std::string& client_ip) const;

    /// Mark a client as paired (after successful PIN verification)
    void mark_paired(const std::string& client_ip);

    /// Revoke a client pairing (e.g., reset button)
    void unpair(const std::string& client_ip);

    /// Handler for /pair-setup and /pair-verify. Returns response body.
    std::vector<uint8_t> handle_pair_setup(const std::string& client_ip,
                                           const uint8_t* body, size_t len,
                                           bool& needs_pin_response);
    std::vector<uint8_t> handle_pair_verify(const std::string& client_ip,
                                            const uint8_t* body, size_t len,
                                            bool& pin_matched_out);

    /// PIN request callback: return true to accept this client/pin combo
    using PinCb = std::function<bool(const std::string& client_ip, const std::string& pin)>;
    void set_pin_callback(PinCb cb) {
        std::lock_guard<std::mutex> lk(mu_);
        pin_cb_ = std::move(cb);
    }

private:
    mutable std::mutex mu_;
    std::string pin_;
    PinCb pin_cb_;
    std::map<std::string, bool> paired_ips_;
};

} // namespace airplay2

#endif // AIRPLAY2_AIRPLAY_PAIRING_H

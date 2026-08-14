/*!
 * @file rtp_receiver.h
 * @brief RTP packet receiver and jitter buffer for AirPlay audio streams
 */
#ifndef AIRPLAY2_RTP_RECEIVER_H
#define AIRPLAY2_RTP_RECEIVER_H

#include "../platform/platform_socket.h"
#include "../platform/platform_thread.h"
#include <cstdint>
#include <functional>
#include <vector>
#include <map>
#include <mutex>
#include <memory>

namespace airplay2 {
namespace net {

/*!
 * @brief A single received RTP audio packet (after jitter buffer reordering)
 */
struct RtpAudioPacket {
    uint16_t seq;       ///< RTP sequence number
    uint32_t timestamp; ///< RTP timestamp (sample clock)
    uint32_t ssrc;
    uint8_t  pt;        ///< payload type
    bool     marker;
    std::vector<uint8_t> payload;
    uint64_t recv_us;   ///< Receive monotonic timestamp
};

using AudioPacketCb = std::function<void(const RtpAudioPacket&)>;

/*!
 * @brief RTP receiver: listens on 3 UDP ports (data/control/timing),
 *        reorders packets by sequence number, and emits them via callback.
 */
class RtpReceiver {
public:
    RtpReceiver();
    ~RtpReceiver();

    /*!
     * @brief Open and bind 3 UDP ports in the range [port_min, port_max]
     * @param[out] ports Bound ports (data, control, timing)
     * @return true if 3 consecutive ports were bound
     */
    bool open(uint16_t port_min, uint16_t port_max, int ports[3]);

    /// Register packet callback (invoked on receiver thread after reorder)
    void set_packet_callback(AudioPacketCb cb) { packet_cb_ = std::move(cb); }

    /// Start the receiver thread
    bool start();

    /// Stop and close ports
    void stop();

    /// Clear jitter buffer (e.g., on FLUSH)
    void flush();

    /// Get stats
    struct Stats {
        uint64_t packets = 0;
        uint64_t bytes = 0;
        uint64_t lost = 0;
        uint64_t reordered = 0;
    };
    Stats stats() const { return stats_; }

private:
    void receiver_worker();
    void emit_ready();

    platform::Socket data_sock_;
    platform::Socket ctrl_sock_;
    platform::Socket timing_sock_;
    platform::Thread worker_;
    std::atomic<bool> running_{false};
    AudioPacketCb packet_cb_;

    // Jitter buffer
    std::mutex jbuf_mu_;
    std::map<uint16_t, RtpAudioPacket> jbuffer_;
    uint16_t next_expected_seq_ = 0;
    bool     has_started_ = false;
    Stats    stats_;
    size_t   jbuf_max_ = 128;  // packets
};

} // namespace net
} // namespace airplay2

#endif // AIRPLAY2_RTP_RECEIVER_H

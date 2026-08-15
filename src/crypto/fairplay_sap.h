/*!
 * @file fairplay_sap.h
 * @brief FairPlay SAP 媒体密钥解密（PlayFair 白盒）
 *
 * iOS AirPlay 2 音频/镜像的媒体密钥流程：
 *   1. /fp-setup seq=3 时客户端发来 164 字节 keymsg（含白盒加密的会话密钥）；
 *   2. SETUP 请求体 bplist 里带 72 字节 ekey（FairPlay 加密的 AES 密钥）；
 *   3. fairplay_sap_decrypt(keymsg, ekey) → 16 字节原始 AES 密钥；
 *   4. 若 legacy 配对成功：final_key = SHA512(raw_key || ecdh_secret) 前 16B；
 *   5. 音频用 AES-CBC(final_key, eiv) 解密 RTP payload；
 *      视频用 AES-CTR，key/iv 由 "AirPlayStreamKey/IV{streamConnectionID}"
 *      与 final_key 再 SHA-512 派生。
 *
 * 实现来源：EstebanKubata/playfair（白盒 AES + SAP 哈希），UxPlay 同款。
 * 这些文件来自 Apple FairPlay 的逆向实现，含 Apple 专有密钥材料，
 * 仅用于 AirPlay 接收端互操作。
 */
#ifndef AIRPLAY2_FAIRPLAY_SAP_H
#define AIRPLAY2_FAIRPLAY_SAP_H

#include <cstdint>
#include <cstddef>

namespace airplay2 {
namespace crypto {

/*!
 * @brief 解密 FairPlay 加密的媒体 AES 密钥
 * @param keymsg fp-setup seq=3 收到的 164 字节 keymsg（必须 164B）
 * @param ekey   SETUP bplist 里的 72 字节 ekey
 * @param[out] out 16 字节解密后的 AES 密钥
 * @return true 成功
 */
bool fairplay_sap_decrypt(const uint8_t* keymsg, const uint8_t* ekey,
                          uint8_t out[16]);

} // namespace crypto
} // namespace airplay2

#endif // AIRPLAY2_FAIRPLAY_SAP_H

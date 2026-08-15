/*!
 * @file fairplay_sap.cpp
 * @brief PlayFair 白盒 AES 封装：fairplay_decrypt 等价实现
 *
 * 与 UxPlay fairplay_playfair.c 的 fairplay_decrypt() 完全一致：
 *   chunk1 = ekey[16:56]，chunk2 = ekey[56:72]
 *   sapKey = generate_session_key(default_sap, keymsg)
 *   key_schedule = generate_key_schedule(sapKey)
 *   block = cycle(z_xor(chunk2))
 *   key = x_xor(z_xor(block ^ chunk1))
 */
#include "fairplay_sap.h"

#include <cstring>

// PlayFair 白盒实现（C 文件），符号按 C 链接导出
extern "C" {
void generate_session_key(unsigned char* oldSap, unsigned char* messageIn,
                          unsigned char* sessionKey);
void generate_key_schedule(unsigned char* key_material,
                           uint32_t key_schedule[11][4]);
void cycle(unsigned char* block, uint32_t key_schedule[11][4]);
void z_xor(unsigned char* in, unsigned char* out, int blocks);
void x_xor(unsigned char* in, unsigned char* out, int blocks);
extern unsigned char default_sap[];
}

namespace airplay2 {
namespace crypto {

bool fairplay_sap_decrypt(const uint8_t* keymsg, const uint8_t* ekey,
                          uint8_t out[16]) {
    if (!keymsg || !ekey || !out) return false;

    // ekey 布局：0..15 = AES IV 区，16..55 = 加密密钥第 1 部分(40B)，
    //            56..71 = 加密密钥第 2 部分(16B)
    const uint8_t* chunk1 = ekey + 16;
    const uint8_t* chunk2 = ekey + 56;

    uint8_t sap_key[16];
    generate_session_key(default_sap, (unsigned char*)keymsg, sap_key);

    uint32_t key_schedule[11][4];
    generate_key_schedule(sap_key, key_schedule);

    uint8_t block[16];
    z_xor((unsigned char*)chunk2, block, 1);  // 第二段作为输入块
    cycle(block, key_schedule);               // 白盒 AES 一轮解密
    for (int i = 0; i < 16; ++i)
        out[i] = block[i] ^ chunk1[i];        // 与第一段异或得明文密钥

    // 额外的白盒混淆（与 UxPlay/PlayFair 一致）
    x_xor(out, out, 1);
    z_xor(out, out, 1);
    return true;
}

} // namespace crypto
} // namespace airplay2

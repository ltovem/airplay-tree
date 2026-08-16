/*!
 * @file modified_md5.c
 * @brief 修改版 MD5（vendored，FairPlay SAP 会话密钥派生用）
 *
 * 与标准 MD5 相似（F/G/H/I 轮函数、移位表、sin 常量），但有三处关键改动：
 *   - 中间（i==31 后）按 A/B/C/D 的值对块字做 5 次交换（swap），破坏标准结构；
 *   - 输入块以字节方式直接组装 32bit 字（小端）；
 *   - 输出为 4 个字与初始链值相加（mod 2^32），不再追加长度填充。
 * 由 generate_session_key() 调用（见 omg_hax.c），用于混合 SAP 会话密钥。
 * 逻辑不可变更，任何修改都会导致 FairPlay 密钥解密失败。
 */
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#define printf(...) (void)0;

/*! MD5 每轮循环左移位数表（64 项，与标准 MD5 相同） */
int shift[] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
               5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
               4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
               6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

/*! MD5 轮函数 F：B·C + ~B·D */
uint32_t F(uint32_t B, uint32_t C, uint32_t D)
{
   return (B & C) | (~B & D);
}

/*! MD5 轮函数 G：B·D + C·~D */
uint32_t G(uint32_t B, uint32_t C, uint32_t D)
{
   return (B & D) | (C & ~D);
}

/*! MD5 轮函数 H：B⊕C⊕D */
uint32_t H(uint32_t B, uint32_t C, uint32_t D)
{
   return B ^ C ^ D;
}

/*! MD5 轮函数 I：C⊕(B|~D) */
uint32_t I(uint32_t B, uint32_t C, uint32_t D)
{
   return C ^ (B | ~D);
}


/*! 32bit 循环左移（标准 MD5 的 rol） */
uint32_t rol(uint32_t input, int count)
{
   return ((input << count) & 0xffffffff) | (input & 0xffffffff) >> (32-count);
}

/*! 交换两个 32bit 字（modified_md5 中间的特殊扰动步骤，标准 MD5 没有） */
void swap(uint32_t* a, uint32_t* b)
{
   printf("%08x <-> %08x\n", *a, *b);
   uint32_t c = *a;
   *a = *b;
   *b = c;
}

/*! 修改版 MD5：对 64 字节输入块以 keyIn 为初始链值迭代 64 轮，
 *  输出 keyOut（4 字相加）。详见文件头说明。 */
void modified_md5(unsigned char* originalblockIn, unsigned char* keyIn, unsigned char* keyOut)
{
   unsigned char blockIn[64];
   uint32_t* block_words = (uint32_t*)blockIn;
   uint32_t* key_words = (uint32_t*)keyIn;
   uint32_t* out_words = (uint32_t*)keyOut;
   uint32_t A, B, C, D, Z, tmp;
   int i;
   
   memcpy(blockIn, originalblockIn, 64);

   // Each cycle does something like this:
   A = key_words[0];
   B = key_words[1];
   C = key_words[2];
   D = key_words[3];
   for (i = 0; i < 64; i++)
   {
      uint32_t input;
      int j;
      if (i < 16)
         j = i;
      else if (i < 32)
         j = (5*i + 1) % 16;
      else if (i < 48)
         j = (3*i + 5) % 16;
      else if (i < 64)
         j = 7*i % 16;

      input = blockIn[4*j] << 24 | blockIn[4*j+1] << 16 | blockIn[4*j+2] << 8 | blockIn[4*j+3];
      printf("Key = %08x\n", A);
      Z = A + input + (int)(long long)((1LL << 32) * fabs(sin(i + 1)));
      if (i < 16)
         Z = rol(Z + F(B,C,D), shift[i]);
      else if (i < 32)
         Z = rol(Z + G(B,C,D), shift[i]);
      else if (i < 48)
         Z = rol(Z + H(B,C,D), shift[i]);
      else if (i < 64)
         Z = rol(Z + I(B,C,D), shift[i]);
      if (i == 63)
         printf("Ror is %08x\n", Z);
      printf("Output of round %d: %08X + %08X = %08X (shift %d, constant %08X)\n", i, Z, B, Z+B, shift[i], (int)(long long)((1LL << 32) * fabs(sin(i + 1))));
      Z = Z + B;
      tmp = D;
      D = C;
      C = B;
      B = Z;
      A = tmp;
      if (i == 31)
      {
         // swapsies
         swap(&block_words[A & 15], &block_words[B & 15]);
         swap(&block_words[C & 15], &block_words[D & 15]);
         swap(&block_words[(A & (15<<4))>>4], &block_words[(B & (15<<4))>>4]);
         swap(&block_words[(A & (15<<8))>>8], &block_words[(B & (15<<8))>>8]);
         swap(&block_words[(A & (15<<12))>>12], &block_words[(B & (15<<12))>>12]);
      }
   }
   printf("%08X %08X %08X %08X\n", A, B, C, D);
   // Now we can actually compute the output
   printf("Out:\n");
   printf("%08x + %08x = %08x\n", key_words[0], A, key_words[0] + A);
   printf("%08x + %08x = %08x\n", key_words[1], B, key_words[1] + B);
   printf("%08x + %08x = %08x\n", key_words[2], C, key_words[2] + C);
   printf("%08x + %08x = %08x\n", key_words[3], D, key_words[3] + D);
   out_words[0] = key_words[0] + A;
   out_words[1] = key_words[1] + B;
   out_words[2] = key_words[2] + C;
   out_words[3] = key_words[3] + D;
   
}

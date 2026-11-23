#pragma once

// The launcher's miniz build is compiled once in miniz_impl.c. The upstream header also
// contains implementation bodies, so core consumers include only this small API declaration.
using wwe_mz_ulong = unsigned long;
extern "C" {
wwe_mz_ulong mz_compressBound(wwe_mz_ulong source_len);
int mz_compress2(unsigned char* destination, wwe_mz_ulong* destination_len,
                 const unsigned char* source, wwe_mz_ulong source_len, int level);
int mz_uncompress(unsigned char* destination, wwe_mz_ulong* destination_len,
                  const unsigned char* source, wwe_mz_ulong source_len);
}

inline constexpr int kWweMzOk = 0;
inline constexpr int kWweMzBestCompression = 10;

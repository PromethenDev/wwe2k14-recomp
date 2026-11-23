#include "creation_save.h"

#if defined(WWE2K14_CREATION_SAMPLE_REPRODUCTION)
#include <zlib.h>
#endif

namespace wwe2k14::launcher::internal {

bool CompressWithSystemZlibBest(const std::vector<uint8_t>& plain, std::vector<uint8_t>* compressed) {
#if defined(WWE2K14_CREATION_SAMPLE_REPRODUCTION)
  if (!compressed) return false;
  uLongf capacity = compressBound(static_cast<uLong>(plain.size()));
  compressed->resize(static_cast<size_t>(capacity));
  if (compress2(compressed->data(), &capacity, plain.data(), static_cast<uLong>(plain.size()),
                Z_BEST_COMPRESSION) != Z_OK) {
    compressed->clear();
    return false;
  }
  compressed->resize(static_cast<size_t>(capacity));
  return true;
#else
  (void)plain;
  (void)compressed;
  return false;
#endif
}

}  // namespace wwe2k14::launcher::internal

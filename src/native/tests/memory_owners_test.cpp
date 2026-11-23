#include "../nr/nr_backend.h"

#include <cassert>
#include <cstdint>

int main() {
  using nr::backend::MemoryOwnerResidual;
  assert(MemoryOwnerResidual(100, 40) == 60);
  assert(MemoryOwnerResidual(100, 100) == 0);
  assert(MemoryOwnerResidual(100, 101) == 0);
  assert(MemoryOwnerResidual(0, 0) == 0);
}

#include "name_registry.h"

#include <cassert>
#include <cstdio>

// Hand-rolled test harness: each case asserts, then prints PASS. A failed
// assert aborts with a non-zero exit, which ctest reports as a failure.
//
// TODO: this suite is a STUB. Expand it to cover the happy path for every
// public function, boundary inputs (empty / 1-byte / 32-byte / 33-byte names,
// missing lookup), and every failure case (NameTaken, NotOwner, NotFound),
// asserting the specific RegistryError values. See task.md.

static void register_happy() {
  NameRegistry r;
  assert(r.register_name("alice", "0xA") == RegistryError::Ok);
  assert(r.owner_of("alice") == Owner("0xA"));
  std::puts("  register_happy: PASS");
}

int main() {
  register_happy();
  std::puts("name_registry stub tests passed");
  return 0;
}

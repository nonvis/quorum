#include "registry.h"

#include <cassert>
#include <cstdio>

static void add_and_lookup() {
  Registry r;
  assert(r.add("alpha", 10) == STATUS_OK);
  uint64_t v = 0;
  assert(r.lookup("alpha", &v));
  assert(v == 10);
  std::puts("  add_and_lookup: PASS");
}

static void duplicate_rejected() {
  Registry r;
  r.add("a", 1);
  assert(r.add("a", 2) == STATUS_DUPLICATE);
  uint64_t v = 0;
  assert(r.lookup("a", &v));
  assert(v == 1);
  std::puts("  duplicate_rejected: PASS");
}

static void lookup_missing_false() {
  Registry r;
  uint64_t v = 123;
  assert(!r.lookup("nope", &v));
  std::puts("  lookup_missing_false: PASS");
}

static void remove_and_count() {
  Registry r;
  r.add("a", 1);
  r.add("b", 2);
  assert(r.get_count() == 2);
  assert(r.remove("a") == STATUS_OK);
  assert(r.get_count() == 1);
  assert(r.remove("a") == STATUS_NOT_FOUND);
  std::puts("  remove_and_count: PASS");
}

int main() {
  add_and_lookup();
  duplicate_rejected();
  lookup_missing_false();
  remove_and_count();
  std::puts("all registry tests passed");
  return 0;
}

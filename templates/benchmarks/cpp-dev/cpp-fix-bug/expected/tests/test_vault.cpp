#include "vault.h"

#include <cassert>
#include <cstdio>

// Hand-rolled test harness: each case asserts, then prints PASS. A failed
// assert aborts the process with a non-zero exit, which ctest reports as a
// failure. No external test framework.

static void deposit_then_balance() {
  Vault v;
  assert(!v.deposit("alice", 100));
  assert(v.balance_of("alice") == 100);
  std::puts("  deposit_then_balance: PASS");
}

static void withdraw_reduces() {
  Vault v;
  v.deposit("bob", 100);
  assert(!v.withdraw("bob", 40));
  assert(v.balance_of("bob") == 60);
  std::puts("  withdraw_reduces: PASS");
}

static void withdraw_insufficient_fails() {
  Vault v;
  v.deposit("carol", 10);
  auto err = v.withdraw("carol", 50);
  assert(err == VaultError::InsufficientFunds);
  assert(v.balance_of("carol") == 10);
  std::puts("  withdraw_insufficient_fails: PASS");
}

static void zero_amount_rejected() {
  Vault v;
  assert(v.deposit("dave", 0) == VaultError::ZeroAmount);
  std::puts("  zero_amount_rejected: PASS");
}

static void unknown_account_zero() {
  Vault v;
  assert(v.balance_of("nobody") == 0);
  std::puts("  unknown_account_zero: PASS");
}

int main() {
  deposit_then_balance();
  withdraw_reduces();
  withdraw_insufficient_fails();
  zero_amount_rejected();
  unknown_account_zero();
  std::puts("all vault tests passed");
  return 0;
}

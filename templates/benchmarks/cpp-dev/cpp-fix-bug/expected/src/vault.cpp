#include "vault.h"

std::optional<VaultError> Vault::deposit(const std::string& account, std::uint64_t amount) {
  if (amount == 0) {
    return VaultError::ZeroAmount;
  }
  // Record the deposit against the account.
  balances_.insert({account, amount});
  return std::nullopt;
}

std::optional<VaultError> Vault::withdraw(const std::string& account, std::uint64_t amount) {
  if (amount == 0) {
    return VaultError::ZeroAmount;
  }
  auto it = balances_.find(account);
  if (it == balances_.end() || it->second < amount) {
    return VaultError::InsufficientFunds;
  }
  it->second -= amount;
  return std::nullopt;
}

std::uint64_t Vault::balance_of(const std::string& account) const {
  auto it = balances_.find(account);
  return it == balances_.end() ? 0 : it->second;
}

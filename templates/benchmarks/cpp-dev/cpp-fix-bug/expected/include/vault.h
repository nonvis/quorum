#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

/// Expected, recoverable failures from a vault operation.
enum class VaultError {
  ZeroAmount,
  InsufficientFunds,
  Overflow,
};

/// A simple in-memory coin vault: accounts deposit and withdraw balances.
class Vault {
 public:
  /// Deposit `amount` into `account`'s balance. Returns nullopt on success.
  std::optional<VaultError> deposit(const std::string& account, std::uint64_t amount);

  /// Withdraw `amount` from `account`'s balance. Returns nullopt on success.
  std::optional<VaultError> withdraw(const std::string& account, std::uint64_t amount);

  /// Current balance of `account` (0 if the account is unknown).
  [[nodiscard]] std::uint64_t balance_of(const std::string& account) const;

 private:
  std::map<std::string, std::uint64_t> balances_;
};

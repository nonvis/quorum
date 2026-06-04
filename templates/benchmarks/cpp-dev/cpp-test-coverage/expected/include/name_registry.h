#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>

/// Result of a name-registry operation.
enum class RegistryError {
  Ok,
  EmptyName,
  NameTooLong,
  NameTaken,
  NotFound,
  NotOwner,
};

/// An owner is an opaque address identifier.
using Owner = std::string;

/// A registry of short names (1..=32 bytes) owned by addresses. Owners may
/// transfer a name they hold or release it.
class NameRegistry {
 public:
  /// Register `name` to `owner`. Fails if the name is empty, too long, or taken.
  RegistryError register_name(const std::string& name, const Owner& owner);

  /// Transfer `name` from `from` to `to`. `from` must be the current owner.
  /// Transferring to the current owner succeeds as a no-op.
  RegistryError transfer(const std::string& name, const Owner& from, const Owner& to);

  /// Release `name`. `owner` must be the current owner.
  RegistryError release(const std::string& name, const Owner& owner);

  /// Current owner of `name`, if registered.
  [[nodiscard]] std::optional<Owner> owner_of(const std::string& name) const;

 private:
  static constexpr std::size_t kMaxNameLen = 32;
  std::map<std::string, Owner> names_;
};

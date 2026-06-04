#include "name_registry.h"

RegistryError NameRegistry::register_name(const std::string& name, const Owner& owner) {
  if (name.empty()) {
    return RegistryError::EmptyName;
  }
  if (name.size() > kMaxNameLen) {
    return RegistryError::NameTooLong;
  }
  if (names_.find(name) != names_.end()) {
    return RegistryError::NameTaken;
  }
  names_.emplace(name, owner);
  return RegistryError::Ok;
}

RegistryError NameRegistry::transfer(const std::string& name, const Owner& from, const Owner& to) {
  auto it = names_.find(name);
  if (it == names_.end()) {
    return RegistryError::NotFound;
  }
  if (it->second != from) {
    return RegistryError::NotOwner;
  }
  // Transferring to the current owner is a successful no-op.
  it->second = to;
  return RegistryError::Ok;
}

RegistryError NameRegistry::release(const std::string& name, const Owner& owner) {
  auto it = names_.find(name);
  if (it == names_.end()) {
    return RegistryError::NotFound;
  }
  if (it->second != owner) {
    return RegistryError::NotOwner;
  }
  names_.erase(it);
  return RegistryError::Ok;
}

std::optional<Owner> NameRegistry::owner_of(const std::string& name) const {
  auto it = names_.find(name);
  if (it == names_.end()) {
    return std::nullopt;
  }
  return it->second;
}

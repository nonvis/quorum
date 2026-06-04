#include "registry.h"

Registry::Registry() {}

Registry::~Registry() {
  for (size_t i = 0; i < entries_.size(); ++i) {
    delete entries_[i];
  }
}

Registry::Entry* Registry::find(const std::string& name) const {
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i]->name == name) {
      return entries_[i];
    }
  }
  return NULL;
}

Status Registry::add(const std::string& name, uint64_t value) {
  if (find(name) != NULL) {
    return STATUS_DUPLICATE;
  }
  Entry* e = new Entry();
  e->name = name;
  e->value = value;
  entries_.push_back(e);
  return STATUS_OK;
}

bool Registry::lookup(const std::string& name, uint64_t* out) const {
  Entry* e = find(name);
  if (e == NULL) {
    return false;
  }
  *out = e->value;
  return true;
}

Status Registry::remove(const std::string& name) {
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i]->name == name) {
      delete entries_[i];
      entries_.erase(entries_.begin() + i);
      return STATUS_OK;
    }
  }
  return STATUS_NOT_FOUND;
}

size_t Registry::get_count() const {
  return entries_.size();
}

#ifndef REGISTRY_H
#define REGISTRY_H

#include <stdint.h>

#include <string>
#include <vector>

// Status of a registry operation.
enum Status {
  STATUS_OK = 0,
  STATUS_NOT_FOUND = 1,
  STATUS_DUPLICATE = 2
};

// A named key/value registry. Entries are heap-allocated and owned by the
// registry (freed in the destructor).
class Registry {
 public:
  Registry();
  ~Registry();

  // Add a named entry with a value. Returns STATUS_DUPLICATE if the name
  // already exists.
  Status add(const std::string& name, uint64_t value);

  // Look up a name. On success writes the value through `out` and returns
  // true; on miss returns false and leaves `out` untouched.
  bool lookup(const std::string& name, uint64_t* out) const;

  // Remove a named entry. Returns STATUS_NOT_FOUND if it does not exist.
  Status remove(const std::string& name);

  // Number of entries currently held.
  size_t get_count() const;

 private:
  struct Entry {
    std::string name;
    uint64_t value;
  };

  typedef std::vector<Entry*> EntryList;
  EntryList entries_;

  Entry* find(const std::string& name) const;

  // Non-copyable (C++03 style: declared private, not defined).
  Registry(const Registry&);
  Registry& operator=(const Registry&);
};

#endif  // REGISTRY_H

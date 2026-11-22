#include <cstdio>
#include <vector>

#include "world_texture_lru.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

void test_lru_eviction_order_and_touch() {
  carview::WorldTextureLru lru;
  lru.set_budget(0, 2);
  lru.insert(1, 100, 1);
  lru.insert(2, 100, 2);
  lru.insert(3, 100, 3);
  CHECK(lru.count() == 3);
  CHECK(lru.total_bytes() == 300);

  // Touching 1 at frame 4 promotes it ahead of 3 and 2; 2 is now LRU.
  lru.touch(1, 4);
  // Same-frame touch is a no-op and keeps 1 at MRU.
  lru.touch(1, 4);

  const uint64_t epoch = lru.begin_pin_epoch();
  std::vector<uint32_t> evicted;
  lru.evict_unpinned(epoch, evicted);
  CHECK(evicted.size() == 1 && evicted[0] == 2);
  CHECK(!lru.contains(2));
  CHECK(lru.contains(1) && lru.contains(3));
  CHECK(lru.count() == 2);
  CHECK(lru.total_bytes() == 200);
  CHECK(lru.evictions() == 1);
}

void test_byte_budget_enforcement() {
  carview::WorldTextureLru lru;
  lru.set_budget(1000, 0);
  lru.insert(10, 500, 1);
  lru.insert(20, 600, 2);
  lru.insert(30, 400, 3);
  lru.touch(10, 3);  // Order MRU->LRU: 10, 30, 20

  const uint64_t epoch = lru.begin_pin_epoch();
  std::vector<uint32_t> evicted;
  lru.evict_unpinned(epoch, evicted);
  CHECK(evicted.size() == 1 && evicted[0] == 20);
  CHECK(lru.total_bytes() == 900);
  CHECK(lru.count() == 2);
}

void test_pin_epoch_protection_and_expiry() {
  carview::WorldTextureLru lru;
  lru.set_budget(0, 2);
  lru.insert(1, 100, 1);
  lru.insert(2, 100, 2);
  lru.insert(3, 100, 3);
  lru.insert(4, 100, 4);

  // 1 is oldest, but pinned in epoch1: 2 and 3 must be evicted instead.
  const uint64_t epoch1 = lru.begin_pin_epoch();
  lru.pin(1, epoch1);
  std::vector<uint32_t> evicted;
  lru.evict_unpinned(epoch1, evicted);
  CHECK(evicted.size() == 2);
  CHECK(evicted[0] == 2 && evicted[1] == 3);
  CHECK(lru.contains(1) && lru.contains(4));

  // New epoch without pinning 1: 1's stale epoch1 pin must not protect it.
  lru.insert(5, 100, 5);
  const uint64_t epoch2 = lru.begin_pin_epoch();
  evicted.clear();
  lru.evict_unpinned(epoch2, evicted);
  CHECK(evicted.size() == 1 && evicted[0] == 1);
  CHECK(!lru.contains(1));
  CHECK(lru.contains(4) && lru.contains(5));
}

void test_oversized_pinned_set_stops_cleanly() {
  carview::WorldTextureLru lru;
  lru.set_budget(100, 1);
  lru.insert(1, 100, 1);
  lru.insert(2, 100, 2);
  lru.insert(3, 100, 3);

  const uint64_t epoch = lru.begin_pin_epoch();
  lru.pin(1, epoch);
  lru.pin(2, epoch);
  lru.pin(3, epoch);
  std::vector<uint32_t> evicted;
  lru.evict_unpinned(epoch, evicted);
  CHECK(evicted.empty());
  CHECK(lru.count() == 3);
  CHECK(lru.total_bytes() == 300);
}

void test_reupload_and_reinsert_accounting() {
  carview::WorldTextureLru lru;
  lru.set_budget(0, 1);
  lru.insert(1, 100, 1);
  lru.insert(2, 200, 2);

  const uint64_t epoch = lru.begin_pin_epoch();
  std::vector<uint32_t> evicted;
  lru.evict_unpinned(epoch, evicted);
  CHECK(evicted.size() == 1 && evicted[0] == 1);
  CHECK(lru.reuploads() == 0);

  // Re-inserting previously evicted record 1 increments reuploads.
  lru.insert(1, 150, 3);
  CHECK(lru.uploads() == 3);
  CHECK(lru.reuploads() == 1);
  CHECK(lru.total_bytes() == 350);

  // Re-inserting already resident record updates bytes without bumping uploads/count.
  lru.insert(1, 250, 4);
  CHECK(lru.count() == 2);
  CHECK(lru.uploads() == 3);
  CHECK(lru.total_bytes() == 450);
}

}  // namespace

int main() {
  test_lru_eviction_order_and_touch();
  test_byte_budget_enforcement();
  test_pin_epoch_protection_and_expiry();
  test_oversized_pinned_set_stops_cleanly();
  test_reupload_and_reinsert_accounting();
  std::printf("world_texture_lru_tests: %d failure(s)\n", failures);
  return failures ? 1 : 0;
}

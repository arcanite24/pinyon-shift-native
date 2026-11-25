#include <cstdio>
#include <vector>

#include "world_texture_lru.h"
#define CARVIEW_ARENA_LRU_ONLY
#include "showroom.h"

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

void test_arena_lru_o1_eviction_and_coalescing() {
  // Capacity for 4 draws of 100 verts / 300 indices each.
  carview::WorldArenaLru arena(400, 1200);
  for (uint64_t k = 1; k <= 4; ++k) {
    auto* e = arena.place(k, 100, 300, double(k), 0.0, k, 10);
    CHECK(e != nullptr);
    arena.mark_valid(k);
  }
  CHECK(arena.count() == 4);
  CHECK(arena.vertex_used() == 400);
  CHECK(arena.index_used() == 1200);

  // Release 1 (oldest, tag 5), 2 (tag 8), 3 (tag 12 - still in flight when frames_done == 10).
  // Draw 4 stays referenced (live).
  arena.release(1, 5);
  arena.release(2, 8);
  arena.release(3, 12);
  CHECK(arena.referenced() == 1);
  CHECK(arena.unreferenced_count() == 3);

  // Re-acquiring draw 1 promotes it back to referenced; releasing it at tag 9 puts it at MRU.
  CHECK(arena.acquire(1, 11) != nullptr);
  arena.release(1, 9);

  // Now unref LRU order (MRU -> LRU): 1 (tag 9), 3 (tag 12), 2 (tag 8).
  // Placing draw 5 (200 verts, 600 indices) with frames_done == 10 must evict 2 and 1,
  // coalesce their adjacent/freed ranges into a single 200-vert block, keep draw 3 (in flight)
  // and draw 4 (referenced), and succeed without any full reset!
  std::vector<uint64_t> evicted;
  auto* e5 = arena.place(5, 200, 600, 5.0, 0.0, 12, 10, &evicted);
  CHECK(e5 != nullptr);
  arena.mark_valid(5);
  CHECK(evicted.size() == 2);
  CHECK(evicted[0] == 2 && evicted[1] == 1);
  CHECK(!arena.contains(1) && !arena.contains(2));
  CHECK(arena.contains(3) && arena.contains(4) && arena.contains(5));
  CHECK(arena.vertex_used() == 400);
  CHECK(arena.index_used() == 1200);

  // Draw 6 cannot fit while draw 3 is still pinned by in-flight GPU tag 12 (frames_done == 10).
  auto* e6_blocked = arena.place(6, 100, 300, 6.0, 0.0, 13, 10);
  CHECK(e6_blocked == nullptr);
  CHECK(arena.contains(3) && arena.contains(4) && arena.contains(5));

  // Once GPU finishes frame 12, draw 3 becomes evictable and draw 6 fits immediately.
  evicted.clear();
  auto* e6 = arena.place(6, 100, 300, 6.0, 0.0, 14, 12, &evicted);
  CHECK(e6 != nullptr);
  CHECK(evicted.size() == 1 && evicted[0] == 3);
  // Rolling back an invalid placement frees its slot immediately without entering LRU.
  arena.release(6, 14);
  CHECK(!arena.contains(6));
  CHECK(arena.vertex_used() == 300);
  CHECK(arena.vertex_largest_free() == 100);
  CHECK(arena.vertex_free_ranges() == 1);
}

void test_arena_lru_large_streaming_no_full_reset() {
  // Simulates driving across many world tiles where new_draws * resident_count far exceeds
  // the legacy 400,000 full-reset threshold (e.g., 1,000 new draws * 1,000 resident = 1,000,000).
  // Live draws must stay resident across waves while oldest unreferenced draws evict in O(1).
  constexpr uint32_t kCapDraws = 1200;
  carview::WorldArenaLru arena(kCapDraws * 64, kCapDraws * 192);

  // Wave 0: 1000 draws placed, 200 stay live across the whole drive, 800 released at frame 1.
  for (uint64_t k = 1; k <= 1000; ++k) {
    auto* e = arena.place(k, 64, 192, 0.0, 0.0, 1, 0);
    CHECK(e != nullptr);
    arena.mark_valid(k);
    if (k > 200) arena.release(k, 1);
  }
  CHECK(arena.count() == 1000);
  CHECK(arena.referenced() == 200);

  // Wave 1: 1000 new draws arrive at frame 5 (frames_done == 4).
  // Previously 1000 * 1000 = 1,000,000 > 400,000 triggered vkDeviceWaitIdle() + full wipe.
  // Now all 200 live draws stay untouched and exactly 800 old draws evict incrementally.
  std::vector<uint64_t> evicted;
  for (uint64_t k = 1001; k <= 2000; ++k) {
    auto* e = arena.place(k, 64, 192, 0.0, 0.0, 5, 4, &evicted);
    CHECK(e != nullptr);
    arena.mark_valid(k);
  }
  CHECK(evicted.size() == 800);
  CHECK(arena.count() == 1200);
  CHECK(arena.referenced() == 1200);
  for (uint64_t k = 1; k <= 200; ++k) {
    CHECK(arena.contains(k));
  }
  for (uint64_t k = 201; k <= 1000; ++k) {
    CHECK(!arena.contains(k));
  }
}

}  // namespace

int main() {
  test_lru_eviction_order_and_touch();
  test_byte_budget_enforcement();
  test_pin_epoch_protection_and_expiry();
  test_oversized_pinned_set_stops_cleanly();
  test_reupload_and_reinsert_accounting();
  test_arena_lru_o1_eviction_and_coalescing();
  test_arena_lru_large_streaming_no_full_reset();
  std::printf("world_texture_lru_tests: %d failure(s)\n", failures);
  return failures ? 1 : 0;
}

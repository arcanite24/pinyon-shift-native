// Pure LRU residency tracker for streamed world textures (no Vulkan dependency).
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace carview {

struct WorldTextureBudget {
  uint64_t max_bytes = 0;
  size_t max_count = 0;
};

class WorldTextureLru {
 public:
  void set_budget(uint64_t max_bytes, size_t max_count) {
    max_bytes_ = max_bytes;
    max_count_ = max_count;
  }

  WorldTextureBudget budget() const { return {max_bytes_, max_count_}; }

  void insert(uint32_t record, uint64_t bytes, uint64_t frame) {
    auto it = entries_.find(record);
    if (it != entries_.end()) {
      total_bytes_ = total_bytes_ - it->second.bytes + bytes;
      it->second.bytes = bytes;
      it->second.last_used_frame = frame;
      order_.splice(order_.begin(), order_, it->second.lru_it);
      return;
    }
    order_.push_front(record);
    entries_.emplace(record, Entry{bytes, frame, 0, order_.begin()});
    total_bytes_ += bytes;
    ++uploads_;
    if (ever_evicted_.count(record)) ++reuploads_;
  }

  void touch(uint32_t record, uint64_t frame) {
    auto it = entries_.find(record);
    if (it == entries_.end() || it->second.last_used_frame == frame) return;
    it->second.last_used_frame = frame;
    order_.splice(order_.begin(), order_, it->second.lru_it);
  }

  uint64_t begin_pin_epoch() { return ++pin_epoch_; }

  void pin(uint32_t record, uint64_t epoch) {
    auto it = entries_.find(record);
    if (it != entries_.end()) it->second.pin_epoch = epoch;
  }

  void evict_unpinned(uint64_t pin_epoch, std::vector<uint32_t>& evicted_out) {
    auto over_budget = [&]() {
      return (max_bytes_ > 0 && total_bytes_ > max_bytes_) ||
             (max_count_ > 0 && entries_.size() > max_count_);
    };
    auto it = order_.end();
    while (it != order_.begin() && over_budget()) {
      --it;
      const uint32_t rec = *it;
      auto eit = entries_.find(rec);
      if (eit == entries_.end()) {
        it = order_.erase(it);
        continue;
      }
      if (eit->second.pin_epoch == pin_epoch) continue;
      total_bytes_ -= eit->second.bytes;
      entries_.erase(eit);
      it = order_.erase(it);
      if (ever_evicted_.insert(rec).second) {
        ever_evicted_order_.push_back(rec);
        while (ever_evicted_order_.size() > kMaxTrackedEvictions) {
          ever_evicted_.erase(ever_evicted_order_.front());
          ever_evicted_order_.pop_front();
        }
      }
      ++evictions_;
      evicted_out.push_back(rec);
    }
  }

  bool contains(uint32_t record) const { return entries_.find(record) != entries_.end(); }
  uint64_t total_bytes() const { return total_bytes_; }
  size_t count() const { return entries_.size(); }
  uint64_t uploads() const { return uploads_; }
  uint64_t evictions() const { return evictions_; }
  uint64_t reuploads() const { return reuploads_; }

 private:
  struct Entry {
    uint64_t bytes = 0;
    uint64_t last_used_frame = 0;
    uint64_t pin_epoch = 0;
    std::list<uint32_t>::iterator lru_it;
  };

  static constexpr size_t kMaxTrackedEvictions = 16384;
  std::list<uint32_t> order_;  // MRU at front(), LRU at back()
  std::unordered_map<uint32_t, Entry> entries_;
  std::unordered_set<uint32_t> ever_evicted_;
  std::deque<uint32_t> ever_evicted_order_;
  uint64_t total_bytes_ = 0;
  uint64_t max_bytes_ = 0;
  size_t max_count_ = 0;
  uint64_t pin_epoch_ = 0;
  uint64_t uploads_ = 0;
  uint64_t evictions_ = 0;
  uint64_t reuploads_ = 0;
};

}  // namespace carview

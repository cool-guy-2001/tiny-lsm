#include "../../include/block/block.h"
#include "../../include/block/block_iterator.h"
#include <cstdint>
#include <memory>

namespace tiny_lsm {
BlockIterator::BlockIterator(std::shared_ptr<Block> b, size_t index,
                             uint64_t tranc_id)
    : block(b), current_index(index), tranc_id_(tranc_id),
      cached_value(std::nullopt) {
  skip_by_tranc_id();
}

BlockIterator::BlockIterator(std::shared_ptr<Block> b, const std::string &key,
                             uint64_t tranc_id)
    : block(b), current_index(0), tranc_id_(tranc_id),
      cached_value(std::nullopt) {
  if (!block) {
    return;
  }

  size_t left = 0;
  size_t right = block->offsets.size();
  while (left < right) {
    size_t mid = left + (right - left) / 2;
    if (block->compare_key_at(block->offsets[mid], key) < 0) {
      left = mid + 1;
    } else {
      right = mid;
    }
  }
  current_index = left;
  skip_by_tranc_id();
}

BlockIterator::pointer BlockIterator::operator->() const {
  update_current();
  if (!cached_value.has_value()) {
    return nullptr;
  }
  return &(*cached_value);
}

BlockIterator &BlockIterator::operator++() {
  cached_value = std::nullopt;
  if (!block || current_index >= block->offsets.size()) {
    return *this;
  }

  std::string current_key = block->get_key_at(block->offsets[current_index]);
  ++current_index;
  while (current_index < block->offsets.size() &&
         block->get_key_at(block->offsets[current_index]) == current_key) {
    ++current_index;
  }

  skip_by_tranc_id();
  return *this;
}

bool BlockIterator::operator==(const BlockIterator &other) const {
  return block.get() == other.block.get() && current_index == other.current_index;
}

bool BlockIterator::operator!=(const BlockIterator &other) const {
  return !(*this == other);
}

BlockIterator::value_type BlockIterator::operator*() const {
  update_current();
  if (!cached_value.has_value()) {
    return {"", ""};
  }
  return *cached_value;
}

bool BlockIterator::is_end() {
  return !block || current_index >= block->offsets.size();
}

void BlockIterator::update_current() const {
  if (!block || current_index >= block->offsets.size()) {
    cached_value = std::nullopt;
    return;
  }

  auto offset = block->offsets[current_index];
  cached_value = std::make_pair(block->get_key_at(offset), block->get_value_at(offset));
}

void BlockIterator::skip_by_tranc_id() {
  if (!block) {
    return;
  }

  while (current_index < block->offsets.size()) {
    if (tranc_id_ == 0) {
      return;
    }

    std::string key = block->get_key_at(block->offsets[current_index]);
    size_t i = current_index;
    bool found_visible = false;

    while (i < block->offsets.size() && block->get_key_at(block->offsets[i]) == key) {
      if (block->get_tranc_id_at(block->offsets[i]) <= tranc_id_) {
        current_index = i;
        found_visible = true;
        break;
      }
      ++i;
    }

    if (found_visible) {
      return;
    }

    current_index = i;
  }
}
} // namespace tiny_lsm

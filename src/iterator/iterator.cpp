#include "../../include/iterator/iterator.h"
#include <cstddef>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

namespace tiny_lsm {

// *************************** SearchItem ***************************
bool operator<(const SearchItem &a, const SearchItem &b) {
  // TODO: Lab2.2 实现比较规则
  if (a.key_ < b.key_)
    return true;
  if (a.key_ > b.key_)
    return false;
  return a.idx_ > b.idx_;
}

bool operator>(const SearchItem &a, const SearchItem &b) {
  // TODO: Lab2.2 实现比较规则
  if (a.key_ > b.key_)
    return true;
  if (a.key_ < b.key_)
    return false;
  return a.idx_ < b.idx_;
}

bool operator==(const SearchItem &a, const SearchItem &b) {
  // TODO: Lab2.2 实现比较规则
  if (a.key_ != b.key_)
    return false;
  return a.idx_ == b.idx_;
}

// *************************** HeapIterator ***************************
HeapIterator::HeapIterator(bool skip_delete)
    : max_tranc_id_(0), skip_delete_(skip_delete) {
  // TODO: Lab2.2 实现 HeapIterator 构造函数
  current.reset();
}
HeapIterator::HeapIterator(std::vector<SearchItem> item_vec,
                           uint64_t max_tranc_id, bool skip_delete)
    : max_tranc_id_(max_tranc_id), skip_delete_(skip_delete) {
  // TODO: Lab2.2 实现 HeapIterator 构造函数
  for (auto it : item_vec) {
    items.push(it);
  }

  while (is_valid() && skip_delete_ && !top_value_legal()) {
    auto del_key = items.top().key_;
    while (is_valid() && items.top().key_ == del_key)
      items.pop();
  }
  if (is_valid()) {
    update_current();
  } else {
    current = nullptr;
  }
}

HeapIterator::pointer HeapIterator::operator->() const {
  // TODO: Lab2.2 实现 -> 重载
  if(current!=nullptr){
    return current.get();
  }
  return nullptr;
}

HeapIterator::value_type HeapIterator::operator*() const {
  // TODO: Lab2.2 实现 * 重载
  if(current!=nullptr)
    return *current;
  return {};
}

BaseIterator &HeapIterator::operator++() {
  // TODO: Lab2.2 实现 ++ 重载
  if (is_end()) {
    current = nullptr;
    return *this;
  }
  std::string last_key = current->first;
  while (is_valid() && items.top().key_ == last_key)
    items.pop();
  while (is_valid() && skip_delete_ && items.top().value_ == "") {
    auto del_key = items.top().key_;
    while (is_valid() && items.top().key_ == del_key)
      items.pop();
  }
  if (is_valid()) {
    update_current();
  } else {
    current = nullptr;
  }
  return *this;
}

bool HeapIterator::operator==(const BaseIterator &other) const {
  // TODO: Lab2.2 实现 == 重载
  auto p=dynamic_cast<const HeapIterator*>(&other);
  if(!p)
    return false;
  if(p->is_end()&&this->is_end()){
    return true;
  }
  if(p->is_end()!=this->is_end()){
    return false;
  }
  if(!p->is_end()&&!this->is_end()){
    if(p->current!=nullptr&&this->current!=nullptr){
      return p->current->first==this->current->first;
    }else{
      return false;
    }
  }
  return false;
}

bool HeapIterator::operator!=(const BaseIterator &other) const {
  // TODO: Lab2.2 实现 != 重载
  //return !(HeapIterator::operator==(other));
  return !(*this==other);
  //return true;
}

bool HeapIterator::top_value_legal() const {
  // TODO: Lab2.2 判断顶部元素是否合法
  // ? 被删除的值是不合法
  // ? 不允许访问的事务创建或更改的键值对不合法(暂时忽略)
  if (items.top().value_ == "") {
    return false;
  }
  return true;
}

void HeapIterator::skip_by_tranc_id() {
  // TODO: Lab2.2 后续的Lab实现, 只是作为标记提醒
}

bool HeapIterator::is_end() const { return items.empty(); }
bool HeapIterator::is_valid() const { return !items.empty(); }

void HeapIterator::update_current() const {
  // current 缓存了当前键值对的值, 你实现 -> 重载时可能需要
  // TODO: Lab2.2 更新当前缓存值
  current=std::make_shared<value_type>(items.top().key_,items.top().value_);
}

IteratorType HeapIterator::get_type() const {
  return IteratorType::HeapIterator;
}

uint64_t HeapIterator::get_tranc_id() const { return max_tranc_id_; }
} // namespace tiny_lsm
// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2015 XSky <haomai@xsky.com>
 *
 * Author: Haomai Wang <haomaiwang@gmail.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_MSG_SLABPOOL_H
#define CEPH_MSG_SLABPOOL_H

#include <vector>
#include <list>
#include <limits>

#include "include/Context.h"
#include "include/Spinlock.h"
#include "include/error.h"
#include "include/page.h"
#include "common/perf_counters.h"

static const uint16_t SLAB_MAGIC_NUMBER = 0x51AB; // meant to be 'SLAB' :-)
typedef uint64_t uintptr_t;

class SlabAllocator;

enum {
  l_slabpool_first = 93000,
  l_slabpool_alloc,
  l_slabpool_alloc_new_page,
  l_slabpool_free,
  l_slabpool_reclaim,
  l_slabpool_total_bytes,
  l_slabpool_available_bytes,
  l_slabpool_last,
};

/*
 * SlabPageDesc is 1:1 mapped to slab page.
 * footprint: 80b for each slab page.
 */
struct SlabPageDesc {
 private:
  void *page;
  std::vector<uintptr_t> free_objects;
  size_t max_object_count;
  uint32_t id; // index into slab page vector
  uint16_t magic_number;
  uint8_t class_id;

 public:
  SlabPageDesc(void *data, size_t objects, size_t object_size, uint8_t id, uint32_t idx)
    : page(data), max_object_count(objects), id(idx), magic_number(SLAB_MAGIC_NUMBER), class_id(id) {
    uintptr_t object = reinterpret_cast<uintptr_t>(page);
    // we already return the first object to caller, see 'create_from_new_page'
    free_objects.reserve(objects - 1);
    for (size_t i = 1u; i < objects; i++) {
      object += object_size;
      free_objects.push_back(object);
    }
  }
  ~SlabPageDesc() {
    ::free(page);
  }

  bool empty() const {
    return free_objects.empty();
  }

  size_t size() const {
    return free_objects.size();
  }

  uint32_t index() const {
    return id;
  }

  uint16_t magic() const {
    return magic_number;
  }

  uint8_t slab_class_id() const {
    return class_id;
  }

  bool can_reclaim() const {
    return max_object_count == free_objects.size();
  }

  void* allocate_object() {
    assert(!free_objects.empty());
    void *object = reinterpret_cast<void*>(free_objects.back());
    free_objects.pop_back();
    return object;
  }

  void free_object(void *object) {
    free_objects.push_back(reinterpret_cast<uintptr_t>(object));
  }
};

class SlabClass {
 private:
  std::list<SlabPageDesc*> free_slab_pages;
  size_t obj_size;
  uint8_t class_id;

 public:
  SlabClass(size_t obj_size, uint8_t id)
    : obj_size(obj_size), class_id(id) {}
  ~SlabClass() {
    free_slab_pages.clear();
  }

  size_t object_size() const {
    return obj_size;
  }

  size_t size() const {
    return free_slab_pages.size();
  }

  bool empty() const {
    return free_slab_pages.empty();
  }

  uint8_t slab_class_id() const {
    return class_id;
  }

  void *create(uint32_t *idx) {
    assert(!free_slab_pages.empty());
    SlabPageDesc *desc = free_slab_pages.back();
    void *object = desc->allocate_object();
    if (desc->empty()) {
      // if empty, remove desc from the list of slab pages with free objects.
      free_slab_pages.pop_back();
    }
    *idx = desc->index();
    return object;
  }

  int create_from_new_page(uint64_t max_object_size, uint32_t slab_page_index, SlabPageDesc **desc, void **data) {
    // allocate slab page.
    int r = ::posix_memalign(data, CEPH_PAGE_SIZE, max_object_size);
    if (r != 0) {
        return -errno;
    }
    // allocate descriptor to slab page.
    assert(obj_size % CEPH_PAGE_SIZE == 0);
    uint64_t objects = max_object_size / obj_size;

    try {
      *desc = new SlabPageDesc(*data, objects, obj_size, class_id, slab_page_index);
    } catch (const std::bad_alloc& e) {
      ::free(data);
      return -ENOMEM;
    }

    if (!(*desc)->empty())
      free_slab_pages.push_front(*desc);
    // first object from the allocated slab page is returned.
    return 0;
  }

  void free(void *object, SlabPageDesc *desc) {
    desc->free_object(object);
    if (desc->size() == 1) {
      // push back desc into the list of slab pages with free objects.
      free_slab_pages.push_back(desc);
    }
  }

  SlabPageDesc* reclaim_one() {
    for (std::list<SlabPageDesc*>::iterator it = free_slab_pages.begin();
         it != free_slab_pages.end(); ++it) {
      if ((*it)->can_reclaim()) {
        free_slab_pages.erase(it);
        return *it;
      }
    }
    return NULL;
  }
};

// Note: It's not a thread-safe structure
class SlabAllocator {
 private:
  std::vector<size_t> slab_class_sizes;
  std::vector<SlabClass> slab_classes;
  std::vector<SlabPageDesc*> slab_pages_vector;
  uint64_t max_object_size;
  uint64_t resident_slab_pages;

  Spinlock pending_free_lock;
  std::vector<std::pair<uint32_t, void*> > pending_frees;
  std::vector<uint32_t> recycle_indexs;
  // slab_alloc_dist used to track the alloc size distribution,
  // alloc_slab_class_history records the alloc class owner within AllocHistoryNumber, it's a loopback history
  // history_index indicated the earliest alloc_slab_class_history index
  uint16_t history_index;
  uint8_t last_reclaim_slab_class_id;
  std::vector<uint16_t> slab_alloc_dist;
  std::vector<uint8_t> alloc_slab_class_history;
  PerfCounters *logger;

  // we use this special value avoid history_index reset to 0 when hitting the
  // max tracked history number
  static const int AllocHistoryNumber = 1 << 16;
  static const int HeavyFreeItems = 100;
 private:
  void initialize_slab_allocator(double growth_factor) {
    const size_t initial_size = CEPH_PAGE_SIZE;
    size_t size = initial_size; // initial object size
    uint8_t slab_class_id = 0U;

    while (max_object_size / size > 1) {
      size = (size + CEPH_PAGE_SIZE - 1) & ~(CEPH_PAGE_SIZE - 1);
      slab_class_sizes.push_back(size);
      slab_classes.push_back(SlabClass(size, slab_class_id));
      size *= growth_factor;
      assert(slab_class_id < std::numeric_limits<uint8_t>::max() - 1);
      slab_class_id++;
    }
    slab_class_sizes.push_back(max_object_size);
    slab_classes.push_back(SlabClass(max_object_size, slab_class_id));

    slab_pages_vector.reserve(resident_slab_pages*2);

    slab_alloc_dist.reserve(slab_classes.size());
    alloc_slab_class_history.resize(AllocHistoryNumber, std::numeric_limits<uint8_t>::max());
  }

  SlabClass* get_slab_class(const size_t size) {
      // given a size, find slab class with binary search.
      std::vector<size_t>::iterator i = std::lower_bound(
          slab_class_sizes.begin(), slab_class_sizes.end(), size);
      if (i == slab_class_sizes.end())
          return NULL;
      return &slab_classes[std::distance(slab_class_sizes.begin(), i)];
  }

  SlabClass* get_slab_class(const uint8_t slab_class_id) {
      assert(slab_class_id <= slab_classes.size());
      return &slab_classes[slab_class_id];
  }

  size_t actual_free() {
    std::vector<pair<uint32_t, void*> > freeing;
    pending_free_lock.lock();
    freeing.swap(pending_frees);
    pending_free_lock.unlock();

    size_t bytes = 0;
    for (std::vector<pair<uint32_t, void*> >::iterator it = freeing.begin();
        it != freeing.end(); ++it) {
      SlabPageDesc *desc = slab_pages_vector[it->first];
      assert(desc && desc->magic() == SLAB_MAGIC_NUMBER);
      SlabClass* slab_class = get_slab_class(desc->slab_class_id());
      slab_class->free(it->second, desc);
      bytes += slab_class->object_size();
    }
    if (logger) {
      logger->inc(l_slabpool_free, freeing.size());
      logger->inc(l_slabpool_available_bytes, bytes);
    }
    return freeing.size();
  }

 public:
  SlabAllocator(CephContext *cct, const std::string& n, double growth_factor,
                uint64_t resident, uint64_t max_obj_size)
      : max_object_size(max_obj_size), resident_slab_pages(resident / max_obj_size),
        history_index(0), last_reclaim_slab_class_id(1), logger(NULL) {
    if (cct && cct->_conf->slab_perf_counter) {
      PerfCountersBuilder b(cct, string("slab-") + n, l_slabpool_first, l_slabpool_last);
      b.add_u64_counter(l_slabpool_alloc, "alloc_calls", "The number of allocation request number");
      b.add_u64_counter(l_slabpool_alloc_new_page, "alloc_call_new_page", "The number of allocation request to system");
      b.add_u64_counter(l_slabpool_free, "free_calls", "Free request number");
      b.add_u64_counter(l_slabpool_reclaim, "relaim_calls", "Reclaim request number");
      b.add_u64_counter(l_slabpool_total_bytes, "total_bytes", "Total memory bytes in pool");
      b.add_u64_counter(l_slabpool_available_bytes, "available_bytes", "Available memory bytes in pool");

      logger = b.create_perf_counters();
      cct->get_perfcounters_collection()->add(logger);
    }
    initialize_slab_allocator(growth_factor);
  }

  ~SlabAllocator() {
    for (std::vector<SlabPageDesc*>::iterator it = slab_pages_vector.begin();
         it != slab_pages_vector.end(); ++it) {
      if (*it == NULL) {
        continue;
      }
      delete *it;
    }
  }

  int create(const size_t size, uint32_t *idx, void **data) {
    SlabClass *slab_class = get_slab_class(size);
    if (!slab_class)
      return -EINVAL;

    if (!slab_class->empty()) {
      *data = slab_class->create(idx);
    } else {
      size_t index_to_insert;
      if (recycle_indexs.empty()) {
        index_to_insert = slab_pages_vector.size();
      } else {
        index_to_insert = recycle_indexs.back();
        recycle_indexs.pop_back();
      }
      SlabPageDesc *desc;
      int r = slab_class->create_from_new_page(max_object_size, index_to_insert, &desc, data);
      if (r < 0)
        return r;
      *idx = index_to_insert;
      if (index_to_insert == slab_pages_vector.size())
        slab_pages_vector.push_back(desc);
      else
        slab_pages_vector[index_to_insert] = desc;
      if (logger) {
        logger->inc(l_slabpool_alloc_new_page);
        logger->inc(l_slabpool_available_bytes, max_object_size);
        logger->inc(l_slabpool_total_bytes, max_object_size);
      }
    }
    if (logger) {
      logger->dec(l_slabpool_available_bytes, size);
      logger->inc(l_slabpool_alloc);
    }
    uint8_t evict_slab_class_id = alloc_slab_class_history[history_index];
    if (evict_slab_class_id < std::numeric_limits<uint8_t>::max())
      --slab_alloc_dist[evict_slab_class_id];
    ++slab_alloc_dist[slab_class->slab_class_id()];
    alloc_slab_class_history[history_index++] = slab_class->slab_class_id();

    return 0;
  }

  /**
   * Free an item back to its original slab class.
   */
  void free(uint32_t slab_page_id, void *data) {
    if (data) {
      pending_free_lock.lock();
      pending_frees.push_back(std::make_pair(slab_page_id, data));
      pending_free_lock.unlock();
    }
  }

  uint64_t size() const {
    return (slab_pages_vector.size() - recycle_indexs.size()) * max_object_size;
  }

  uint64_t max_size() const {
    return max_object_size;
  }

  /**
   * Helper function: Print all available slab classes and their respective properties.
   */
  void print_slab_classes() {
    uint8_t class_id = 0;
    for (std::vector<SlabClass>::const_iterator it = slab_classes.begin();
       it != slab_classes.end(); ++it) {
      printf("slab[%3d]\tsize: %10lu\tper-slab-page: %5lu\n", class_id, it->object_size(), max_object_size / it->object_size());
      class_id++;
    }
  }

  /**
   * Helper function: Useful for getting a slab class' chunk size from a size parameter.
   */
  size_t class_size(const size_t size) {
    SlabClass *slab_class = get_slab_class(size);
    return (slab_class) ? slab_class->object_size() : 0;
  }

  /**
   * Reclaim page if existing pages greater than permitted resident pages
   * Return true means no need to reclaim again currently, otherwise indicate
   * caller we need to reclaim continuously
   */
  bool reclaim() {
    size_t freed = actual_free();
    if (freed > HeavyFreeItems)
      return false;

    if ((slab_pages_vector.size() - recycle_indexs.size()) <= resident_slab_pages)
      return true;
    uint64_t limit = double(slab_alloc_dist[last_reclaim_slab_class_id]) / AllocHistoryNumber * resident_slab_pages;
    if (slab_classes[last_reclaim_slab_class_id].size() > limit) {
      SlabPageDesc *page = slab_classes[last_reclaim_slab_class_id].reclaim_one();
      if (page) {
        if (page->index() == slab_pages_vector.size() - 1) {
          slab_pages_vector.pop_back();
        } else {
          recycle_indexs.push_back(page->index());
          slab_pages_vector[page->index()] = NULL;
        }
        delete page;
      }
    }
    if (++last_reclaim_slab_class_id >= slab_classes.size())
      last_reclaim_slab_class_id = 0;
    if (logger)
      logger->inc(l_slabpool_reclaim);
    return false;
  }
};

#endif /* CEPH_MSG_SLABPOOL_H */

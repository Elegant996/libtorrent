// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2005-2011, Jari Sundell <jaris@ifi.uio.no>

#include <algorithm>
#include <functional>
#include <limits>
#include <numeric>

#include "download/download_main.h"
#include "protocol/peer_connection_base.h"
#include "torrent/download/choke_group.h"
#include "torrent/download/choke_queue.h"
#include "torrent/download/resource_manager.h"
#include "torrent/exceptions.h"
#include "torrent/utils/log.h"

namespace torrent {

const Rate*
resource_manager_entry::up_rate() const {
  return m_download->info()->up_rate();
}
const Rate*
resource_manager_entry::down_rate() const {
  return m_download->info()->down_rate();
}

ResourceManager::~ResourceManager() {
  if (m_currentlyUploadUnchoked != 0) {
    destruct_error("ResourceManager::~ResourceManager() called but "
                   "m_currentlyUploadUnchoked != 0.");
    return;
  }

  if (m_currentlyDownloadUnchoked != 0) {
    destruct_error("ResourceManager::~ResourceManager() called but "
                   "m_currentlyDownloadUnchoked != 0.");
    return;
  }

  std::for_each(choke_base_type::begin(),
                choke_base_type::end(),
                [](choke_group* g) { delete g; });
}

// If called directly ensure a valid group has been selected.
ResourceManager::iterator
ResourceManager::insert(const resource_manager_entry& entry) {
  bool will_realloc = true; // size() == capacity();

  auto itr = base_type::insert(find_group_end(entry.group()), entry);

  DownloadMain* download = itr->download();

  download->set_choke_group(choke_base_type::at(entry.group()));

  if (will_realloc) {
    update_group_iterators();
  } else {
    auto group_itr = choke_base_type::begin() + itr->group();
    (*group_itr)->set_last((*group_itr)->last() + 1);

    std::for_each(++group_itr,
                  choke_base_type::end(),
                  std::mem_fn(&choke_group::inc_iterators));
  }

  choke_queue::move_connections(nullptr,
                                group_at(entry.group())->up_queue(),
                                download,
                                download->up_group_entry());
  choke_queue::move_connections(nullptr,
                                group_at(entry.group())->down_queue(),
                                download,
                                download->down_group_entry());

  return itr;
}

void
ResourceManager::update_group_iterators() {
  auto entry_itr = base_type::begin();
  auto group_itr = choke_base_type::begin();

  while (group_itr != choke_base_type::end()) {
    (*group_itr)->set_first(&*entry_itr);
    entry_itr = std::find_if(entry_itr, end(), [group_itr, this](value_type v) {
      return (std::distance(choke_base_type::begin(), group_itr)) < v.group();
    });
    (*group_itr)->set_last(&*entry_itr);
    group_itr++;
  }
}

void
ResourceManager::validate_group_iterators() noexcept(false) {
  if (base_type::empty()) {
    for (auto group_itr = choke_base_type::begin();
         group_itr != choke_base_type::end();
         group_itr++) {
      if ((*group_itr)->first() != nullptr || (*group_itr)->last() != nullptr) {
        throw internal_error(
          "ResourceManager::receive_tick() invalid non-null iterator.");
      }
    }
  } else {
    auto entry_itr = base_type::begin();
    for (auto group_itr = choke_base_type::begin();
         group_itr != choke_base_type::end();
         group_itr++) {
      if ((*group_itr)->first() != &*entry_itr)
        throw internal_error(
          "ResourceManager::receive_tick() invalid first iterator.");

      entry_itr =
        std::find_if(entry_itr, end(), [group_itr, this](value_type v) {
          return (std::distance(choke_base_type::begin(), group_itr)) <
                 v.group();
        });

      if ((*group_itr)->last() != &*entry_itr)
        throw internal_error(
          "ResourceManager::receive_tick() invalid last iterator.");
    }
  }
}

void
ResourceManager::erase(DownloadMain* d) noexcept(false) {
  auto itr = std::find_if(
    begin(), end(), [d](value_type e) { return d == e.download(); });

  if (itr == end())
    throw internal_error("ResourceManager::erase() itr == end().");

  choke_queue::move_connections(
    group_at(itr->group())->up_queue(), nullptr, d, d->up_group_entry());
  choke_queue::move_connections(
    group_at(itr->group())->down_queue(), nullptr, d, d->down_group_entry());

  auto group_itr = choke_base_type::begin() + itr->group();

  if (base_type::size() == 1) {
    std::for_each(group_itr, choke_base_type::end(), [](choke_group* cg) {
      cg->set_first(nullptr);
      cg->set_last(nullptr);
    });
  } else {
    (*group_itr)->set_last((*group_itr)->last() - 1);

    std::for_each(++group_itr,
                  choke_base_type::end(),
                  std::mem_fn(&choke_group::dec_iterators));
  }

  base_type::erase(itr);
}

void
ResourceManager::push_group(const std::string& name) {
  if (name.empty() ||
      std::any_of(choke_base_type::begin(),
                  choke_base_type::end(),
                  [name](choke_group* g) { return name == g->name(); }))
    throw input_error("Duplicate name for choke group.");

  choke_base_type::push_back(new choke_group());

  choke_base_type::back()->set_name(name);

  if (!base_type::empty()) {
    choke_base_type::back()->set_first(&*base_type::end());
    choke_base_type::back()->set_last(&*base_type::end());
  }

  choke_base_type::back()->up_queue()->set_heuristics(
    choke_queue::HEURISTICS_UPLOAD_LEECH);
  choke_base_type::back()->down_queue()->set_heuristics(
    choke_queue::HEURISTICS_DOWNLOAD_LEECH);

  choke_base_type::back()->up_queue()->set_slot_unchoke(
    [this](int num) { receive_upload_unchoke(num); });
  choke_base_type::back()->down_queue()->set_slot_unchoke(
    [this](int num) { receive_download_unchoke(num); });

  choke_base_type::back()->up_queue()->set_slot_can_unchoke(
    [this]() { return retrieve_upload_can_unchoke(); });
  choke_base_type::back()->down_queue()->set_slot_can_unchoke(
    [this]() { return retrieve_download_can_unchoke(); });

  choke_base_type::back()->up_queue()->set_slot_connection(
    [](PeerConnectionBase* pcb, bool choke) {
      return pcb->receive_upload_choke(choke);
    });
  choke_base_type::back()->down_queue()->set_slot_connection(
    [](PeerConnectionBase* pcb, bool choke) {
      return pcb->receive_download_choke(choke);
    });
}

ResourceManager::iterator
ResourceManager::find(DownloadMain* d) {
  return std::find_if(
    begin(), end(), [d](value_type e) { return d == e.download(); });
}

ResourceManager::iterator
ResourceManager::find_throw(DownloadMain* d) {
  auto itr = std::find_if(
    begin(), end(), [d](value_type e) { return d == e.download(); });

  if (itr == end())
    throw input_error("Could not find download in resource manager.");

  return itr;
}

ResourceManager::iterator
ResourceManager::find_group_end(uint16_t group) {
  return std::find_if(
    begin(), end(), [group](value_type v) { return group < v.group(); });
}

choke_group*
ResourceManager::group_at(uint16_t grp) {
  if (grp >= choke_base_type::size())
    throw input_error("Choke group not found.");

  return choke_base_type::at(grp);
}

choke_group*
ResourceManager::group_at_name(const std::string& name) {
  auto itr = std::find_if(choke_base_type::begin(),
                          choke_base_type::end(),
                          [name](choke_group* g) { return name == g->name(); });

  if (itr == choke_base_type::end())
    throw input_error("Choke group not found.");

  return *itr;
}

int
ResourceManager::group_index_of(const std::string& name) {
  auto itr = std::find_if(choke_base_type::begin(),
                          choke_base_type::end(),
                          [name](choke_group* g) { return name == g->name(); });

  if (itr == choke_base_type::end())
    throw input_error("Choke group not found.");

  return std::distance(choke_base_type::begin(), itr);
}

void
ResourceManager::set_priority(iterator itr, uint16_t pri) {
  itr->set_priority(pri);
}

void
ResourceManager::set_group(iterator itr, uint16_t grp) {
  if (itr->group() == grp)
    return;

  if (grp >= choke_base_type::size())
    throw input_error("Choke group not found.");

  choke_queue::move_connections(itr->download()->choke_group()->up_queue(),
                                choke_base_type::at(grp)->up_queue(),
                                itr->download(),
                                itr->download()->up_group_entry());
  choke_queue::move_connections(itr->download()->choke_group()->down_queue(),
                                choke_base_type::at(grp)->down_queue(),
                                itr->download(),
                                itr->download()->down_group_entry());

  auto group_src  = choke_base_type::begin() + itr->group();
  auto group_dest = choke_base_type::begin() + grp;

  resource_manager_entry entry = *itr;
  entry.set_group(grp);
  entry.download()->set_choke_group(choke_base_type::at(entry.group()));

  base_type::erase(itr);
  base_type::insert(find_group_end(entry.group()), entry);

  // Update the group iterators after the move. We know the groups are
  // not the same, so no need to check for that.
  if (group_dest < group_src) {
    (*group_dest)->set_last((*group_dest)->last() + 1);
    std::for_each(
      ++group_dest, group_src, std::mem_fn(&choke_group::inc_iterators));
    (*group_src)->set_first((*group_src)->first() + 1);
  } else {
    (*group_src)->set_last((*group_src)->last() - 1);
    std::for_each(
      ++group_src, group_dest, std::mem_fn(&choke_group::dec_iterators));
    (*group_dest)->set_first((*group_dest)->first() - 1);
  }
}

void
ResourceManager::set_max_upload_unchoked(unsigned int m) {
  if (m > (1 << 16))
    throw input_error("Max unchoked must be between 0 and 2^16.");

  m_maxUploadUnchoked = m;
}

void
ResourceManager::set_max_download_unchoked(unsigned int m) {
  if (m > (1 << 16))
    throw input_error("Max unchoked must be between 0 and 2^16.");

  m_maxDownloadUnchoked = m;
}

// The choking choke manager won't updated it's count until after
// possibly multiple calls of this function.
void
ResourceManager::receive_upload_unchoke(int num) noexcept(false) {
  lt_log_print(LOG_PEER_INFO,
               "Upload unchoked slots adjust; currently:%u adjust:%i",
               m_currentlyUploadUnchoked,
               num);

  if ((int)m_currentlyUploadUnchoked + num < 0)
    throw internal_error("ResourceManager::receive_upload_unchoke(...) "
                         "received an invalid value.");

  m_currentlyUploadUnchoked += num;
}

void
ResourceManager::receive_download_unchoke(int num) noexcept(false) {
  lt_log_print(LOG_PEER_INFO,
               "Download unchoked slots adjust; currently:%u adjust:%i",
               m_currentlyDownloadUnchoked,
               num);

  if ((int)m_currentlyDownloadUnchoked + num < 0)
    throw internal_error("ResourceManager::receive_download_unchoke(...) "
                         "received an invalid value.");

  m_currentlyDownloadUnchoked += num;
}

int
ResourceManager::retrieve_upload_can_unchoke() {
  if (m_maxUploadUnchoked == 0)
    return std::numeric_limits<int>::max();

  return (int)m_maxUploadUnchoked - (int)m_currentlyUploadUnchoked;
}

int
ResourceManager::retrieve_download_can_unchoke() {
  if (m_maxDownloadUnchoked == 0)
    return std::numeric_limits<int>::max();

  return (int)m_maxDownloadUnchoked - (int)m_currentlyDownloadUnchoked;
}

void
ResourceManager::receive_tick() {
  validate_group_iterators();

  m_currentlyUploadUnchoked +=
    balance_unchoked(choke_base_type::size(), m_maxUploadUnchoked, true);
  m_currentlyDownloadUnchoked +=
    balance_unchoked(choke_base_type::size(), m_maxDownloadUnchoked, false);

  auto up_unchoked = std::accumulate(
    choke_base_type::begin(),
    choke_base_type::end(),
    static_cast<unsigned int>(0),
    [](auto&& sum, choke_group* cg) { return sum + cg->up_unchoked(); });

  auto down_unchoked = std::accumulate(
    choke_base_type::begin(),
    choke_base_type::end(),
    static_cast<unsigned int>(0),
    [](auto&& sum, choke_group* cg) { return sum + cg->down_unchoked(); });

  if (m_currentlyUploadUnchoked != up_unchoked)
    throw torrent::internal_error(
      "m_currentlyUploadUnchoked != "
      "choke_base_type::back()->up_queue()->size_unchoked()");

  if (m_currentlyDownloadUnchoked != down_unchoked)
    throw torrent::internal_error(
      "m_currentlyDownloadUnchoked != "
      "choke_base_type::back()->down_queue()->size_unchoked()");
}

unsigned int
ResourceManager::total_weight() const {
  // TODO: This doesn't take into account inactive downloads.
  unsigned int result = 0;

  for (const auto& entry : *this) {
    result += entry.priority();
  }

  return result;
}

int
ResourceManager::balance_unchoked(unsigned int weight,
                                  unsigned int max_unchoked,
                                  bool         is_up) noexcept(false) {
  int change = 0;

  if (max_unchoked == 0) {
    auto group_itr = choke_base_type::begin();

    while (group_itr != choke_base_type::end()) {
      choke_queue* cm =
        is_up ? (*group_itr)->up_queue() : (*group_itr)->down_queue();

      change += cm->cycle(std::numeric_limits<unsigned int>::max());
      group_itr++;
    }

    return change;
  }

  unsigned int quota = max_unchoked;

  // We put the downloads with fewest interested first so that those
  // with more interested will gain any unused slots from the
  // preceding downloads. Consider multiplying with priority.
  //
  // Consider skipping the leading zero interested downloads. Though
  // that won't work as they need to choke peers once their priority
  // is turned off.

  auto choke_groups =
    static_cast<choke_group**>(malloc(group_size() * sizeof(choke_group*)));
  std::copy(choke_base_type::begin(), choke_base_type::end(), choke_groups);

  // Start with the group requesting fewest slots (relative to weight)
  // so that we only need to iterate through the list once allocating
  // slots. There will be no slots left unallocated unless all groups
  // have reached max slots allowed.

  choke_group** group_first = choke_groups;
  choke_group** group_last  = choke_groups + group_size();

  if (is_up) {
    std::sort(
      group_first, group_last, [](choke_group* left, choke_group* right) {
        return left->up_requested() < right->up_requested();
      });
    lt_log_print(
      LOG_PEER_DEBUG,
      "Upload unchoked slots cycle; currently:%u adjusted:%i max_unchoked:%u",
      m_currentlyUploadUnchoked,
      change,
      max_unchoked);
  } else {
    std::sort(
      group_first, group_last, [](choke_group* left, choke_group* right) {
        return left->down_requested() < right->down_requested();
      });
    lt_log_print(LOG_PEER_DEBUG,
                 "Download unchoked slots cycle; currently:%u adjusted:%i "
                 "max_unchoked:%u",
                 m_currentlyDownloadUnchoked,
                 change,
                 max_unchoked);
  }

  while (group_first != group_last) {
    choke_queue* cm =
      is_up ? (*group_first)->up_queue() : (*group_first)->down_queue();

    // change += cm->cycle(weight != 0 ? (quota * itr->priority()) / weight :
    // 0);
    change += cm->cycle(weight != 0 ? quota / weight : 0);

    quota -= cm->size_unchoked();
    // weight -= itr->priority();
    weight--;
    group_first++;
  }

  free(choke_groups);

  if (weight != 0)
    throw internal_error(
      "ResourceManager::balance_unchoked(...) weight did not reach zero.");

  return change;
}

} // namespace torrent

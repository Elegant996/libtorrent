// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2005-2011, Jari Sundell <jaris@ifi.uio.no>

#include <algorithm>
#include <functional>
#include <set>

#include "data/chunk.h"
#include "torrent/data/block_failed.h"
#include "torrent/data/block_list.h"
#include "torrent/data/block_transfer.h"
#include "torrent/data/piece.h"
#include "torrent/data/transfer_list.h"
#include "torrent/exceptions.h"
#include "torrent/peer/peer_info.h"
#include "torrent/utils/timer.h"

namespace torrent {

// TODO: Derp if transfer list isn't cleared...

TransferList::~TransferList() {
  if (!base_type::empty())
    destruct_error(
      "TransferList::~TransferList() called on an non-empty object");
}

TransferList::iterator
TransferList::find(uint32_t index) {
  return std::find_if(
    begin(), end(), [index](BlockList* b) { return index == b->index(); });
}

TransferList::const_iterator
TransferList::find(uint32_t index) const {
  return std::find_if(
    begin(), end(), [index](BlockList* b) { return index == b->index(); });
}

void
TransferList::clear() {
  for (const auto& blockList : *this) {
    m_slot_canceled(blockList->index());
  }

  for (const auto& blockList : *this) {
    delete blockList;
  }

  base_type::clear();
}

TransferList::iterator
TransferList::insert(const Piece& piece, uint32_t blockSize) {
  if (find(piece.index()) != end())
    throw internal_error("Delegator::new_chunk(...) received an index that is "
                         "already delegated.");

  auto blockList = new BlockList(piece, blockSize);

  m_slot_queued(piece.index());

  return base_type::insert(end(), blockList);
}

// TODO: Create a destructor to ensure all blocklists have been
// cleared/invaldiated?

TransferList::iterator
TransferList::erase(iterator itr) {
  if (itr == end())
    throw internal_error("TransferList::erase(...) itr == m_chunks.end().");

  delete *itr;

  return base_type::erase(itr);
}

void
TransferList::finished(BlockTransfer* transfer) {
  if (!transfer->is_valid())
    throw internal_error(
      "TransferList::finished(...) got transfer with wrong state.");

  uint32_t index = transfer->block()->index();

  // Marks the transfer as complete and erases it.
  if (transfer->block()->completed(transfer))
    m_slot_completed(index);
}

void
TransferList::hash_succeeded(uint32_t index, Chunk* chunk) {
  auto blockListItr = find(index);

  if ((Block::size_type)std::count_if((*blockListItr)->begin(),
                                      (*blockListItr)->end(),
                                      std::mem_fn(&Block::is_finished)) !=
      (*blockListItr)->size())
    throw internal_error("TransferList::hash_succeeded(...) Finished blocks "
                         "does not match size.");

  // The chunk should also be marked here or by the caller so that it
  // gets priority for syncing back to disk.

  if ((*blockListItr)->failed() != 0)
    mark_failed_peers(*blockListItr, chunk);

  // Add to a list of finished chunks indices with timestamps. This is
  // mainly used for torrent resume data on which chunks need to be
  // rehashed on crashes.
  //
  // We assume the chunk gets sync'ed within 10 minutes, so minimum
  // retention time of 30 minutes should be enough. The list only gets
  // pruned every 60 minutes, so any timer that reads values once
  // every 30 minutes is guaranteed to get them all as long as it is
  // ordered properly.
  m_completedList.push_back(
    std::make_pair(utils::timer::current().usec(), index));

  if (utils::timer(m_completedList.front().first) +
        utils::timer::from_minutes(60) <
      utils::timer::current()) {
    auto itr =
      std::find_if(m_completedList.begin(),
                   m_completedList.end(),
                   [](completed_list_type::value_type v) {
                     return (utils::timer::current() -
                             utils::timer::from_minutes(30)) <= v.first;
                   });
    m_completedList.erase(m_completedList.begin(), itr);
  }

  m_succeededCount++;
  erase(blockListItr);
}

struct transfer_list_compare_data {
  transfer_list_compare_data(Chunk* chunk, const Piece& p)
    : m_chunk(chunk)
    , m_piece(p) {}

  bool operator()(BlockFailed::value_type failed) {
    return m_chunk->compare_buffer(
      failed.first, m_piece.offset(), m_piece.length());
  }

  Chunk* m_chunk;
  Piece  m_piece;
};

void
TransferList::hash_failed(uint32_t index, Chunk* chunk) {
  auto blockListItr = find(index);

  if (blockListItr == end())
    throw internal_error(
      "TransferList::hash_failed(...) Could not find index.");

  if ((Block::size_type)std::count_if((*blockListItr)->begin(),
                                      (*blockListItr)->end(),
                                      std::mem_fn(&Block::is_finished)) !=
      (*blockListItr)->size())
    throw internal_error(
      "TransferList::hash_failed(...) Finished blocks does not match size.");

  m_failedCount++;

  // Could propably also check promoted against size of the block
  // list.

  if ((*blockListItr)->attempt() == 0) {
    unsigned int promoted = update_failed(*blockListItr, chunk);

    if (promoted > 0 || promoted < (*blockListItr)->size()) {
      // Retry with the most popular blocks.
      (*blockListItr)->set_attempt(1);
      retry_most_popular(*blockListItr, chunk);

      // Also consider various other schemes, like using blocks from
      // only/mainly one peer.

      return;
    }
  }

  // Should we check if there's any peers whom have sent us bad data
  // before, and just clear those first?

  // Re-download the blocks.
  (*blockListItr)->do_all_failed();
}

// update_failed(...) either increments the reference count of a
// failed entry, or creates a new one if the data differs.
unsigned int
TransferList::update_failed(BlockList* blockList, Chunk* chunk) {
  unsigned int promoted = 0;

  blockList->inc_failed();

  for (auto& block : *blockList) {
    if (block.failed_list() == nullptr) {
      block.set_failed_list(new BlockFailed());
    }

    auto failedItr =
      std::find_if(block.failed_list()->begin(),
                   block.failed_list()->end(),
                   transfer_list_compare_data(chunk, block.piece()));

    if (failedItr == block.failed_list()->end()) {
      // We've never encountered this data before, make a new entry.
      char* buffer = new char[block.piece().length()];

      chunk->to_buffer(buffer, block.piece().offset(), block.piece().length());

      block.failed_list()->push_back(BlockFailed::value_type(buffer, 1));
      failedItr = block.failed_list()->end() - 1;

      // Count how many new data sets?

    } else {
      // Increment promoted when the entry's reference count becomes
      // larger than others, but not if it previously was the largest.

      auto maxItr = block.failed_list()->max_element();

      if (maxItr->second == failedItr->second &&
          maxItr != (block.failed_list()->reverse_max_element().base() - 1))
        promoted++;

      failedItr->second++;
    }

    block.failed_list()->set_current(failedItr);
    block.leader()->set_failed_index(failedItr - block.failed_list()->begin());
  }

  return promoted;
}

void
TransferList::mark_failed_peers(BlockList* blockList, Chunk* chunk) {
  std::set<PeerInfo*> badPeers;

  for (auto& block : *blockList) {
    // This chunk data is good, set it as current and
    // everyone who sent something else is a bad peer.
    block.failed_list()->set_current(
      std::find_if(block.failed_list()->begin(),
                   block.failed_list()->end(),
                   transfer_list_compare_data(chunk, block.piece())));

    for (const auto& transfer : *block.transfers()) {
      if (transfer->failed_index() != block.failed_list()->current() &&
          transfer->failed_index() != ~uint32_t()) {
        badPeers.insert(transfer->peer_info());
      }
    }
  }

  for (const auto& peer : badPeers) {
    m_slot_corrupt(peer);
  }
}

// Copy the stored data to the chunk from the failed entries with
// largest reference counts.
void
TransferList::retry_most_popular(BlockList* blockList, Chunk* chunk) {
  for (auto& block : *blockList) {

    auto failedItr = block.failed_list()->reverse_max_element();

    if (failedItr == block.failed_list()->rend())
      throw internal_error(
        "TransferList::retry_most_popular(...) No failed list entry found.");

    // The data is the same, so no need to copy.
    if (failedItr == block.failed_list()->current_reverse_iterator())
      continue;

    // Change the leader to the currently held buffer?

    chunk->from_buffer(
      failedItr->first, block.piece().offset(), block.piece().length());

    block.failed_list()->set_current(failedItr);
  }

  m_slot_completed(blockList->index());
}

} // namespace torrent

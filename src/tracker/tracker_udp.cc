// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2005-2011, Jari Sundell <jaris@ifi.uio.no>

#define __STDC_FORMAT_MACROS

#include <sys/types.h>

#include <cstdio>

#include "manager.h"
#include "net/address_list.h"
#include "torrent/connection_manager.h"
#include "torrent/download_info.h"
#include "torrent/exceptions.h"
#include "torrent/poll.h"
#include "torrent/tracker_list.h"
#include "torrent/utils/error_number.h"
#include "torrent/utils/log.h"
#include "torrent/utils/option_strings.h"
#include "torrent/utils/random.h"
#include "torrent/utils/uri_parser.h"
#include "tracker/tracker_udp.h"

#define LT_LOG_TRACKER(log_level, log_fmt, ...)                                \
  lt_log_print_info(LOG_TRACKER_##log_level,                                   \
                    m_parent->info(),                                          \
                    "tracker_udp",                                             \
                    "[%u] " log_fmt,                                           \
                    group(),                                                   \
                    __VA_ARGS__);

#define LT_LOG_TRACKER_DUMP(                                                   \
  log_level, log_dump_data, log_dump_size, log_fmt, ...)                       \
  lt_log_print_info_dump(LOG_TRACKER_##log_level,                              \
                         log_dump_data,                                        \
                         log_dump_size,                                        \
                         m_parent->info(),                                     \
                         "tracker_udp",                                        \
                         "[%u] " log_fmt,                                      \
                         group(),                                              \
                         __VA_ARGS__);

namespace torrent {

TrackerUdp::TrackerUdp(TrackerList* parent, const std::string& url, int flags)
  : Tracker(parent, url, flags)
  ,

  m_port(0)
  ,

  m_slot_resolver(nullptr)
  , m_readBuffer(nullptr)
  , m_writeBuffer(nullptr) {

  m_taskTimeout.slot() = [this]() { receive_timeout(); };
}

TrackerUdp::~TrackerUdp() {
  if (m_slot_resolver != nullptr) {
    *m_slot_resolver = resolver_type();
    m_slot_resolver  = nullptr;
  }

  close_directly();
}

bool
TrackerUdp::is_busy() const {
  return get_fd().is_valid();
}

void
TrackerUdp::send_state(int state) {
  close_directly();
  m_latest_event = state;

  hostname_type hostname;

  if (!parse_udp_url(m_url, hostname, m_port))
    return receive_failed("could not parse hostname or port");

  LT_LOG_TRACKER(DEBUG, "hostname lookup (address:%s)", hostname.data());

  m_sendState = state;

  // Because we can only remember one slot, set any pending resolves blocked
  // so that if this tracker is deleted, the member function won't be called.
  if (m_slot_resolver != nullptr) {
    *m_slot_resolver = resolver_type();
    m_slot_resolver  = nullptr;
  }

  m_slot_resolver = make_resolver_slot(hostname);
}

bool
TrackerUdp::parse_udp_url(const std::string&,
                          hostname_type& hostname,
                          int&           port) const {
  if (std::sscanf(
        m_url.c_str(), "udp://%1023[^:]:%i", hostname.data(), &port) == 2 &&
      hostname[0] != '\0' && port > 0 && port < (1 << 16))
    return true;

  if (std::sscanf(
        m_url.c_str(), "udp://[%1023[^]]]:%i", hostname.data(), &port) == 2 &&
      hostname[0] != '\0' && port > 0 && port < (1 << 16))
    return true;

  return false;
}

TrackerUdp::resolver_type*
TrackerUdp::make_resolver_slot(const hostname_type& hostname) {
  return manager->connection_manager()->resolver()(
    hostname.data(),
    PF_UNSPEC,
    SOCK_DGRAM,
    [this](const sockaddr* sa, int err) { start_announce(sa, err); });
}

void
TrackerUdp::start_announce(const sockaddr* sa, int) {
  if (m_slot_resolver != nullptr) {
    *m_slot_resolver = resolver_type();
    m_slot_resolver  = nullptr;
  }

  if (sa == nullptr)
    return receive_failed("could not resolve hostname");

  m_connectAddress = *utils::socket_address::cast_from(sa);
  m_connectAddress.set_port(m_port);

  LT_LOG_TRACKER(DEBUG,
                 "address found (address:%s)",
                 m_connectAddress.address_str().c_str());

  if (!m_connectAddress.is_valid())
    return receive_failed("invalid tracker address");

  // TODO: Make each of these a separate error... at the very least separate
  // open and bind.
  if (!get_fd().open_datagram() || !get_fd().set_nonblock())
    return receive_failed("could not open UDP socket");

  auto bind_address = utils::socket_address::cast_from(
    manager->connection_manager()->bind_address());

  if (bind_address->is_bindable() && !get_fd().bind(*bind_address))
    return receive_failed(
      "failed to bind socket to udp address '" +
      bind_address->pretty_address_str() + "' with error '" +
      utils::error_number::current().message().c_str() + "'");

  m_readBuffer  = new ReadBuffer;
  m_writeBuffer = new WriteBuffer;

  prepare_connect_input();

  manager->poll()->open(this);
  manager->poll()->insert_read(this);
  manager->poll()->insert_write(this);
  manager->poll()->insert_error(this);

  m_tries = m_parent->info()->udp_tries();
  priority_queue_insert(
    &taskScheduler,
    &m_taskTimeout,
    (cachedTime + utils::timer::from_seconds(m_parent->info()->udp_timeout()))
      .round_seconds());
}

void
TrackerUdp::close() {
  if (!get_fd().is_valid())
    return;

  LT_LOG_TRACKER(DEBUG,
                 "request cancelled (state:%s url:%s)",
                 option_as_string(OPTION_TRACKER_EVENT, m_latest_event),
                 m_url.c_str());

  close_directly();
}

void
TrackerUdp::disown() {
  if (!get_fd().is_valid())
    return;

  LT_LOG_TRACKER(DEBUG,
                 "request disowned (state:%s url:%s)",
                 option_as_string(OPTION_TRACKER_EVENT, m_latest_event),
                 m_url.c_str());

  close_directly();
}

void
TrackerUdp::close_directly() {
  if (!get_fd().is_valid())
    return;

  delete m_readBuffer;
  delete m_writeBuffer;

  m_readBuffer  = nullptr;
  m_writeBuffer = nullptr;

  priority_queue_erase(&taskScheduler, &m_taskTimeout);

  manager->poll()->remove_read(this);
  manager->poll()->remove_write(this);
  manager->poll()->remove_error(this);
  manager->poll()->close(this);

  get_fd().close();
  get_fd().clear();
}

TrackerUdp::Type
TrackerUdp::type() const {
  return TRACKER_UDP;
}

void
TrackerUdp::receive_failed(const std::string& msg) {
  close_directly();
  m_parent->receive_failed(this, msg);
}

void
TrackerUdp::receive_timeout() {
  if (m_taskTimeout.is_queued())
    throw internal_error("TrackerUdp::receive_timeout() called but "
                         "m_taskTimeout is still scheduled.");

  if (--m_tries == 0) {
    receive_failed("unable to connect to UDP tracker");
  } else {
    priority_queue_insert(
      &taskScheduler,
      &m_taskTimeout,
      (cachedTime + utils::timer::from_seconds(m_parent->info()->udp_timeout()))
        .round_seconds());

    manager->poll()->insert_write(this);
  }
}

void
TrackerUdp::event_read() {
  utils::socket_address sa;

  int s = read_datagram(m_readBuffer->begin(), m_readBuffer->reserved(), &sa);

  if (s < 0)
    return;

  m_readBuffer->reset_position();
  m_readBuffer->set_end(s);

  LT_LOG_TRACKER_DUMP(
    DEBUG, (const char*)m_readBuffer->begin(), s, "received reply", 0);

  if (s < 4)
    return;

  // Make sure sa is from the source we expected?

  // Do something with the content here.
  switch (m_readBuffer->read_32()) {
    case 0:
      if (m_action != 0 || !process_connect_output())
        return;

      prepare_announce_input();

      priority_queue_erase(&taskScheduler, &m_taskTimeout);
      priority_queue_insert(&taskScheduler,
                            &m_taskTimeout,
                            (cachedTime + utils::timer::from_seconds(
                                            m_parent->info()->udp_timeout()))
                              .round_seconds());

      m_tries = m_parent->info()->udp_tries();
      manager->poll()->insert_write(this);
      return;

    case 1:
      if (m_action != 1 || !process_announce_output())
        return;

      return;

    case 3:
      if (!process_error_output())
        return;

      return;

    default:
      return;
  };
}

void
TrackerUdp::event_write() {
  if (m_writeBuffer->size_end() == 0)
    throw internal_error(
      "TrackerUdp::write() called but the write buffer is empty.");

  write_datagram(
    m_writeBuffer->begin(), m_writeBuffer->size_end(), &m_connectAddress);

  manager->poll()->remove_write(this);
}

void
TrackerUdp::event_error() {}

void
TrackerUdp::prepare_connect_input() {
  m_writeBuffer->reset();
  m_writeBuffer->write_64(m_connectionId = magic_connection_id);
  m_writeBuffer->write_32(m_action = 0);
  m_writeBuffer->write_32(m_transactionId = random_uint32());

  LT_LOG_TRACKER_DUMP(DEBUG,
                      m_writeBuffer->begin(),
                      m_writeBuffer->size_end(),
                      "prepare connect (id:%" PRIx32 ")",
                      m_transactionId);
}

void
TrackerUdp::prepare_announce_input() {
  DownloadInfo* info = m_parent->info();

  m_writeBuffer->reset();

  m_writeBuffer->write_64(m_connectionId);
  m_writeBuffer->write_32(m_action = 1);
  m_writeBuffer->write_32(m_transactionId = random_uint32());

  m_writeBuffer->write_range(info->hash().begin(), info->hash().end());
  m_writeBuffer->write_range(info->local_id().begin(), info->local_id().end());

  uint64_t uploaded_adjusted  = info->uploaded_adjusted();
  uint64_t completed_adjusted = info->completed_adjusted();
  uint64_t download_left      = info->slot_left()();

  m_writeBuffer->write_64(completed_adjusted);
  m_writeBuffer->write_64(download_left);
  m_writeBuffer->write_64(uploaded_adjusted);
  m_writeBuffer->write_32(m_sendState);

  const utils::socket_address* localAddress = utils::socket_address::cast_from(
    manager->connection_manager()->local_address());

  uint32_t local_addr = 0;

  if (localAddress->family() == utils::socket_address::af_inet)
    local_addr = localAddress->sa_inet()->address_n();

  m_writeBuffer->write_32_n(local_addr);
  m_writeBuffer->write_32(m_parent->key());
  m_writeBuffer->write_32(m_parent->numwant());
  m_writeBuffer->write_16(manager->connection_manager()->listen_port());

  if (m_writeBuffer->size_end() != 98)
    throw internal_error(
      "TrackerUdp::prepare_announce_input() ended up with the wrong size");

  LT_LOG_TRACKER_DUMP(DEBUG,
                      m_writeBuffer->begin(),
                      m_writeBuffer->size_end(),
                      "prepare announce (state:%s id:%" PRIx32
                      " up_adj:%" PRIu64 " completed_adj:%" PRIu64
                      " left_adj:%" PRIu64 ")",
                      option_as_string(OPTION_TRACKER_EVENT, m_sendState),
                      m_transactionId,
                      uploaded_adjusted,
                      completed_adjusted,
                      download_left);
}

bool
TrackerUdp::process_connect_output() {
  if (m_readBuffer->size_end() < 16 ||
      m_readBuffer->read_32() != m_transactionId)
    return false;

  m_connectionId = m_readBuffer->read_64();

  return true;
}

bool
TrackerUdp::process_announce_output() {
  if (m_readBuffer->size_end() < 20 ||
      m_readBuffer->read_32() != m_transactionId)
    return false;

  set_normal_interval(m_readBuffer->read_32());
  set_min_interval(default_min_interval);

  m_scrape_incomplete = m_readBuffer->read_32(); // leechers
  m_scrape_complete   = m_readBuffer->read_32(); // seeders
  m_scrape_time_last  = utils::timer::current().seconds();

  AddressList l;

  std::copy(
    reinterpret_cast<const SocketAddressCompact*>(m_readBuffer->position()),
    reinterpret_cast<const SocketAddressCompact*>(
      m_readBuffer->end() -
      m_readBuffer->remaining() % sizeof(SocketAddressCompact)),
    std::back_inserter(l));

  // Some logic here to decided on whetever we're going to close the
  // connection or not?
  close_directly();
  m_parent->receive_success(this, &l);

  return true;
}

bool
TrackerUdp::process_error_output() {
  if (m_readBuffer->size_end() < 8 ||
      m_readBuffer->read_32() != m_transactionId)
    return false;

  receive_failed("received error message: " +
                 std::string(m_readBuffer->position(), m_readBuffer->end()));
  return true;
}

} // namespace torrent

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2005-2011, Jari Sundell <jaris@ifi.uio.no>

#define __STDC_FORMAT_MACROS

#include <iomanip>
#include <sstream>

#include "globals.h"
#include "manager.h"
#include "net/address_list.h"
#include "net/local_addr.h"
#include "torrent/connection_manager.h"
#include "torrent/download_info.h"
#include "torrent/exceptions.h"
#include "torrent/http.h"
#include "torrent/object_stream.h"
#include "torrent/tracker_list.h"
#include "torrent/utils/log.h"
#include "torrent/utils/option_strings.h"
#include "torrent/utils/string_manip.h"
#include "tracker/tracker_http.h"

#define LT_LOG_TRACKER(log_level, log_fmt, ...)                                \
  lt_log_print_info(LOG_TRACKER_##log_level,                                   \
                    m_parent->info(),                                          \
                    "tracker",                                                 \
                    "[%u] " log_fmt,                                           \
                    group(),                                                   \
                    __VA_ARGS__);

#define LT_LOG_TRACKER_DUMP(                                                   \
  log_level, log_dump_data, log_dump_size, log_fmt, ...)                       \
  lt_log_print_info_dump(LOG_TRACKER_##log_level,                              \
                         log_dump_data,                                        \
                         log_dump_size,                                        \
                         m_parent->info(),                                     \
                         "tracker",                                            \
                         "[%u] " log_fmt,                                      \
                         group(),                                              \
                         __VA_ARGS__);

namespace torrent {

TrackerHttp::TrackerHttp(TrackerList* parent, const std::string& url, int flags)
  : Tracker(parent, url, flags)
  ,

  m_get(Http::slot_factory()())
  , m_data(nullptr) {

  m_get->signal_done().push_back([this]() { receive_done(); });
  m_get->signal_failed().push_back(
    [this](const std::string& s) { m_normal_interval = 0; m_min_interval = 0; receive_failed(s); });

  // Haven't considered if this needs any stronger error detection,
  // can dropping the '?' be used for malicious purposes?
  size_t delim_options = url.rfind('?');

  m_dropDeliminator = delim_options != std::string::npos &&
                      url.find('/', delim_options) == std::string::npos;

  // Check the url if we can use scrape.
  size_t delim_slash = url.rfind('/');

  if (delim_slash != std::string::npos &&
      url.find("/announce", delim_slash) == delim_slash)
    m_flags |= flag_can_scrape;
}

TrackerHttp::~TrackerHttp() {
  delete m_get;
  delete m_data;
}

bool
TrackerHttp::is_busy() const {
  return m_data != nullptr;
}

void
TrackerHttp::request_prefix(std::stringstream* stream, const std::string& url) {
  char hash[61];

  *utils::copy_escape_html(m_parent->info()->hash().begin(),
                           m_parent->info()->hash().end(),
                           hash) = '\0';
  *stream << url << (m_dropDeliminator ? '&' : '?') << "info_hash=" << hash;
}

void
TrackerHttp::send_state(int state) {
  close_directly();

  if (m_parent == nullptr)
    throw internal_error(
      "TrackerHttp::send_state(...) does not have a valid m_parent.");

  m_latest_event = state;

  std::stringstream s;
  s.imbue(std::locale::classic());

  char localId[61];

  DownloadInfo* info = m_parent->info();

  request_prefix(&s, m_url);

  *utils::copy_escape_html(
    info->local_id().begin(), info->local_id().end(), localId) = '\0';

  s << "&peer_id=" << localId;

  if (m_parent->key())
    s << "&key=" << std::hex << std::setw(8) << std::setfill('0')
      << m_parent->key() << std::dec;

  if (!m_tracker_id.empty())
    s << "&trackerid=" << utils::copy_escape_html(m_tracker_id);

  const utils::socket_address* localAddress = utils::socket_address::cast_from(
    manager->connection_manager()->local_address());

  if (!localAddress->is_address_any())
    s << "&ip=" << localAddress->address_str();

  if (localAddress->is_address_any() &&
      localAddress->family() == utils::socket_address::pf_inet) {
    utils::socket_address local_v6;
    if (get_local_address(utils::socket_address::af_inet6, &local_v6))
      s << "&ipv6=" << utils::copy_escape_html(local_v6.address_str());
  }

  if (localAddress->is_address_any() &&
      localAddress->family() == utils::socket_address::pf_inet6) {
    utils::socket_address local_v4;
    if (get_local_address(utils::socket_address::af_inet, &local_v4))
      s << "&ipv4=" << local_v4.address_str();
  }

  if (info->is_compact())
    s << "&compact=1";

  if (m_parent->numwant() >= 0 && state != DownloadInfo::STOPPED)
    s << "&numwant=" << m_parent->numwant();

  if (manager->connection_manager()->listen_port())
    s << "&port=" << manager->connection_manager()->listen_port();

  uint64_t uploaded_adjusted  = info->uploaded_adjusted();
  uint64_t completed_adjusted = info->completed_adjusted();
  uint64_t download_left      = info->slot_left()();

  s << "&uploaded=" << uploaded_adjusted << "&downloaded=" << completed_adjusted
    << "&left=" << download_left;

  switch (state) {
    case DownloadInfo::STARTED:
      s << "&event=started";
      break;
    case DownloadInfo::STOPPED:
      s << "&event=stopped";
      break;
    case DownloadInfo::COMPLETED:
      s << "&event=completed";
      break;
    default:
      break;
  }

  m_data = new std::stringstream();

  std::string request_url = s.str();

  LT_LOG_TRACKER_DUMP(DEBUG,
                      request_url.c_str(),
                      request_url.size(),
                      "Tracker HTTP request: state:%s up_adj:%" PRIu64
                      " completed_adj:%" PRIu64 " left_adj:%" PRIu64 ".",
                      option_as_string(OPTION_TRACKER_EVENT, state),
                      uploaded_adjusted,
                      completed_adjusted,
                      download_left);

  m_get->set_url(request_url);
  m_get->set_stream(m_data);
  m_get->set_timeout(2 * 60);

  m_get->start();
}

void
TrackerHttp::send_scrape() {
  if (m_data != nullptr)
    return;

  m_latest_event = EVENT_SCRAPE;

  std::stringstream s;
  s.imbue(std::locale::classic());

  request_prefix(&s, scrape_url_from(m_url));

  m_data = new std::stringstream();

  std::string request_url = s.str();

  LT_LOG_TRACKER_DUMP(
    DEBUG, request_url.c_str(), request_url.size(), "Tracker HTTP scrape.", 0);

  m_get->set_url(request_url);
  m_get->set_stream(m_data);
  m_get->set_timeout(2 * 60);

  m_get->start();
}

void
TrackerHttp::close() {
  if (m_data == nullptr)
    return;

  LT_LOG_TRACKER(DEBUG,
                 "Tracker HTTP request cancelled: state:%s url:%s.",
                 option_as_string(OPTION_TRACKER_EVENT, m_latest_event),
                 m_url.c_str());

  close_directly();
}

void
TrackerHttp::disown() {
  if (m_data == nullptr)
    return;

  LT_LOG_TRACKER(DEBUG,
                 "Tracker HTTP request disowned: state:%s url:%s.",
                 option_as_string(OPTION_TRACKER_EVENT, m_latest_event),
                 m_url.c_str());

  m_get->set_delete_self();
  m_get->set_delete_stream();
  m_get->signal_done().clear();
  m_get->signal_failed().clear();

  // Allocate this dynamically, so that we don't need to do this here.
  m_get  = Http::slot_factory()();
  m_data = nullptr;
}

TrackerHttp::Type
TrackerHttp::type() const {
  return TRACKER_HTTP;
}

void
TrackerHttp::close_directly() {
  if (m_data == nullptr)
    return;

  m_get->close();
  m_get->set_stream(nullptr);

  delete m_data;
  m_data = nullptr;
}

void
TrackerHttp::receive_done() {
  if (m_data == nullptr)
    throw internal_error(
      "TrackerHttp::receive_done() called on an invalid object");

  if (lt_log_is_valid(LOG_TRACKER_DEBUG)) {
    std::string dump = m_data->str();
    LT_LOG_TRACKER_DUMP(
      DEBUG, dump.c_str(), dump.size(), "Tracker HTTP reply.", 0);
  }

  Object b;
  *m_data >> b;

  if (m_data->fail()) {
    std::string dump = m_data->str();
    return receive_failed(
      "Could not parse bencoded data: " +
      utils::sanitize(utils::striptags(dump)).substr(0, 99));
  }

  if (!b.is_map())
    return receive_failed("Root not a bencoded map");

  if (b.has_key("failure reason")) {
    if (m_latest_event != EVENT_SCRAPE)
      process_failure(b);
    return receive_failed("Failure reason \"" +
                          (b.get_key("failure reason").is_string()
                             ? b.get_key_string("failure reason")
                             : std::string("failure reason not a string")) +
                          "\"");
  }

  if (m_latest_event == EVENT_SCRAPE)
    process_scrape(b);
  else
    process_success(b);
}

void
TrackerHttp::receive_failed(const std::string& msg) {
  if (lt_log_is_valid(LOG_TRACKER_DEBUG)) {
    std::string dump = m_data->str();
    LT_LOG_TRACKER_DUMP(
      DEBUG, dump.c_str(), dump.size(), "Tracker HTTP failed.", 0);
  }

  close_directly();

  if (m_latest_event == EVENT_SCRAPE)
    m_parent->receive_scrape_failed(this, msg);
  else
    m_parent->receive_failed(this, msg);
}

void
TrackerHttp::process_failure(const Object& object) {
  if (object.has_key_value("interval"))
    set_normal_interval(object.get_key_value("interval"));

  if (object.has_key_value("min interval"))
    set_min_interval(object.get_key_value("min interval"));

  if (object.has_key_string("tracker id"))
    m_tracker_id = object.get_key_string("tracker id");

  if (object.has_key_value("complete") && object.has_key_value("incomplete")) {
    m_scrape_complete = std::max<int64_t>(object.get_key_value("complete"), 0);
    m_scrape_incomplete =
      std::max<int64_t>(object.get_key_value("incomplete"), 0);
    m_scrape_time_last = cachedTime.seconds();
  }

  if (object.has_key_value("downloaded"))
    m_scrape_downloaded =
      std::max<int64_t>(object.get_key_value("downloaded"), 0);
}

void
TrackerHttp::process_success(const Object& object) {
  if (object.has_key_value("interval"))
    set_normal_interval(object.get_key_value("interval"));
  else
    set_normal_interval(default_normal_interval);

  if (object.has_key_value("min interval"))
    set_min_interval(object.get_key_value("min interval"));
  else
    set_normal_interval(default_min_interval);

  if (object.has_key_string("tracker id"))
    m_tracker_id = object.get_key_string("tracker id");

  if (object.has_key_value("complete") && object.has_key_value("incomplete")) {
    m_scrape_complete = std::max<int64_t>(object.get_key_value("complete"), 0);
    m_scrape_incomplete =
      std::max<int64_t>(object.get_key_value("incomplete"), 0);
    m_scrape_time_last = cachedTime.seconds();
  }

  if (object.has_key_value("downloaded"))
    m_scrape_downloaded =
      std::max<int64_t>(object.get_key_value("downloaded"), 0);
  
  AddressList l;

  if (!object.has_key("peers") && !object.has_key("peers6"))
    return receive_failed("No peers returned");

  if (object.has_key("peers")) {
    try {
      // Due to some trackers sending the wrong type when no peers are
      // available, don't bork on it.
      if (object.get_key("peers").is_string())
        l.parse_address_compact(object.get_key_string("peers"));

      else if (object.get_key("peers").is_list())
        l.parse_address_normal(object.get_key_list("peers"));

    } catch (bencode_error& e) {
      return receive_failed(e.what());
    }
  }

  if (object.has_key_string("peers6"))
    l.parse_address_compact_ipv6(object.get_key_string("peers6"));

  close_directly();
  m_parent->receive_success(this, &l);
}

void
TrackerHttp::process_scrape(const Object& object) {
  if (!object.has_key_map("files"))
    return receive_failed("Tracker scrape does not have files entry.");

  // Add better validation here...
  const Object& files = object.get_key("files");

  if (!files.has_key_map(m_parent->info()->hash().str()))
    return receive_failed("Tracker scrape replay did not contain infohash.");

  const Object& stats = files.get_key(m_parent->info()->hash().str());

  if (stats.has_key_value("complete"))
    m_scrape_complete = std::max<int64_t>(stats.get_key_value("complete"), 0);

  if (stats.has_key_value("incomplete"))
    m_scrape_incomplete =
      std::max<int64_t>(stats.get_key_value("incomplete"), 0);

  if (stats.has_key_value("downloaded"))
    m_scrape_downloaded =
      std::max<int64_t>(stats.get_key_value("downloaded"), 0);

  LT_LOG_TRACKER(INFO,
                 "Tracker scrape for %u torrents: complete:%u incomplete:%u "
                 "downloaded:%u.",
                 files.as_map().size(),
                 m_scrape_complete,
                 m_scrape_incomplete,
                 m_scrape_downloaded);

  close_directly();
  m_parent->receive_scrape_success(this);
}

} // namespace torrent

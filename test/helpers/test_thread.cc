#include <cppunit/extensions/HelperMacros.h>
#include <unistd.h>

#include "thread_disk.h"
#include "torrent/exceptions.h"
#include "torrent/poll_select.h"

#include "test/helpers/test_thread.h"

test_thread::test_thread()
  : m_test_state(TEST_NONE)
  , m_test_flags(0) {}

void
test_thread::init_thread() {
  m_state      = STATE_INITIALIZED;
  m_test_state = TEST_PRE_START;
  m_poll       = torrent::PollSelect::create(256);
}

void
test_thread::call_events() {
  if ((m_test_flags & test_flag_pre_stop) && m_test_state == TEST_PRE_START &&
      m_state == STATE_ACTIVE)
    m_test_state = TEST_PRE_STOP;

  if ((m_test_flags & test_flag_acquire_global)) {
    acquire_global_lock();
    m_test_flags &= ~test_flag_acquire_global;
    m_test_flags |= test_flag_has_global;
  }

  if ((m_flags & flag_do_shutdown)) {
    if ((m_flags & flag_did_shutdown))
      throw torrent::internal_error("Already trigged shutdown.");

    m_flags |= flag_did_shutdown;
    throw torrent::shutdown_exception();
  }

  if ((m_test_flags & test_flag_pre_poke)) {
  }

  if ((m_test_flags & test_flag_do_work)) {
    usleep(10 * 1000); // TODO: Don't just sleep, as that give up core.
    m_test_flags &= ~test_flag_do_work;
  }

  if ((m_test_flags & test_flag_post_poke)) {
  }
}

thread_management_type::thread_management_type() {
  CPPUNIT_ASSERT(torrent::thread_base::trylock_global_lock());
}

thread_management_type::~thread_management_type() {
  torrent::thread_base::release_global_lock();
}

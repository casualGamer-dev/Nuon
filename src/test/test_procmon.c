/* Copyright (c) 2010-2021, The Nuon Project, Inc. */
/* See LICENSE for licensing information */

#include "orconfig.h"
#include "core/or/or.h"
#include "test/test.h"

#include "lib/evloop/procmon.h"

#include "test/log_test_helpers.h"

struct event_base;

static void
test_procmon_tor_process_monitor_new(void *ignored)
{
  (void)ignored;
  tor_process_monitor_t *res;
  const char *msg;

  res = tor_process_monitor_new(NULL, "probably invalid", 0, NULL, NULL, &msg);
  tt_assert(!res);
  tt_str_op(msg, OP_EQ, "invalid PID");

  res = tor_process_monitor_new(NULL, "243443535345454", 0, NULL, NULL, &msg);
  tt_assert(!res);
  tt_str_op(msg, OP_EQ, "invalid PID");

  res = tor_process_monitor_new(tor_libevent_get_base(), "43", 0,
                                NULL, NULL, &msg);
  tt_assert(res);
  tt_assert(!msg);
  tor_process_monitor_free(res);

  res = tor_process_monitor_new(tor_libevent_get_base(), "44 hello", 0,
                                NULL, NULL, &msg);
  tt_assert(res);
  tt_assert(!msg);
  tor_process_monitor_free(res);

  res = tor_process_monitor_new(tor_libevent_get_base(), "45:hello", 0,
                                NULL, NULL, &msg);
  tt_assert(res);
  tt_assert(!msg);

 done:
  tor_process_monitor_free(res);
}

struct testcase_t procmon_tests[] = {
  { "tor_process_monitor_new", test_procmon_tor_process_monitor_new,
    TT_FORK, NULL, NULL },
  END_OF_TESTCASES
};


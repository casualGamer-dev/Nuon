/* Copyright (c) 2013-2021, The Nuon Project, Inc. */
/* See LICENSE for licensing information */

#define CONFIG_PRIVATE

#include "orconfig.h"
#include "core/or/or.h"
#include "app/config/config.h"
#include "lib/err/torerr.h"
#include "lib/log/log.h"
#include "test/test.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

static void
dummy_cb_fn(int severity, log_domain_mask_t domain, const char *msg)
{
  (void)severity; (void)domain; (void)msg;
}

static void
test_get_sigsafe_err_fds(void *arg)
{
  const int *fds;
  int n;
  log_severity_list_t include_bug, no_bug, no_bug2;
  (void) arg;
  init_logging(1);

  n = tor_log_get_sigsafe_err_fds(&fds);
  tt_int_op(n, OP_EQ, 1);
  tt_int_op(fds[0], OP_EQ, STDERR_FILENO);

  set_log_severity_config(LOG_WARN, LOG_ERR, &include_bug);
  set_log_severity_config(LOG_WARN, LOG_ERR, &no_bug);
  no_bug.masks[SEVERITY_MASK_IDX(LOG_ERR)] &= ~(LD_BUG|LD_GENERAL);
  set_log_severity_config(LOG_INFO, LOG_NOTICE, &no_bug2);

  /* Add some logs; make sure the output is as expected. */
  mark_logs_temp();
  add_stream_log(&include_bug, "dummy-1", 3);
  add_stream_log(&no_bug, "dummy-2", 4);
  add_stream_log(&no_bug2, "dummy-3", 5);
  add_callback_log(&include_bug, dummy_cb_fn);
  close_temp_logs();
  tor_log_update_sigsafe_err_fds();

  n = tor_log_get_sigsafe_err_fds(&fds);
  tt_int_op(n, OP_EQ, 2);
  tt_int_op(fds[0], OP_EQ, STDERR_FILENO);
  tt_int_op(fds[1], OP_EQ, 3);

  /* Allow STDOUT to replace STDERR. */
  add_stream_log(&include_bug, "dummy-4", STDOUT_FILENO);
  tor_log_update_sigsafe_err_fds();
  n = tor_log_get_sigsafe_err_fds(&fds);
  tt_int_op(n, OP_EQ, 2);
  tt_int_op(fds[0], OP_EQ, 3);
  tt_int_op(fds[1], OP_EQ, STDOUT_FILENO);

  /* But don't allow it to replace explicit STDERR. */
  add_stream_log(&include_bug, "dummy-5", STDERR_FILENO);
  tor_log_update_sigsafe_err_fds();
  n = tor_log_get_sigsafe_err_fds(&fds);
  tt_int_op(n, OP_EQ, 3);
  tt_int_op(fds[0], OP_EQ, STDERR_FILENO);
  tt_int_op(fds[1], OP_EQ, STDOUT_FILENO);
  tt_int_op(fds[2], OP_EQ, 3);

  /* Don't overflow the array. */
  {
    int i;
    for (i=5; i<20; ++i) {
      add_stream_log(&include_bug, "x-dummy", i);
    }
  }
  tor_log_update_sigsafe_err_fds();
  n = tor_log_get_sigsafe_err_fds(&fds);
  tt_int_op(n, OP_EQ, 8);

 done:
  ;
}

static void
test_sigsafe_err(void *arg)
{
  const char *fn=get_fname("sigsafe_err_log");
  char *content=NULL;
  log_severity_list_t include_bug;
  smartlist_t *lines = smartlist_new();
  (void)arg;

  set_log_severity_config(LOG_WARN, LOG_ERR, &include_bug);

  init_logging(1);
  mark_logs_temp();
  open_and_add_file_log(&include_bug, fn, 0);
  tor_log_update_sigsafe_err_fds();
  close_temp_logs();

  close(STDERR_FILENO);
  log_err(LD_BUG, "Say, this isn't too cool.");
  tor_log_err_sigsafe("Minimal.\n", NULL);

  set_log_time_granularity(100*1000);
  tor_log_err_sigsafe("Testing any ",
                      "attempt to manually log ",
                      "from a signal.\n",
                      NULL);
  mark_logs_temp();
  close_temp_logs();
  close(STDERR_FILENO);
  content = read_file_to_str(fn, 0, NULL);

  tt_ptr_op(content, OP_NE, NULL);
  smartlist_split_string(lines, content, "\n", 0, 0);
  tt_int_op(smartlist_len(lines), OP_GE, 5);

  if (strstr(smartlist_get(lines, 0), "opening new log file")) {
    void *item = smartlist_get(lines, 0);
    smartlist_del_keeporder(lines, 0);
    tor_free(item);
  }

  tt_assert(strstr(smartlist_get(lines, 0), "Say, this isn't too cool"));
  tt_str_op(smartlist_get(lines, 1), OP_EQ, "");
  tt_assert(!strcmpstart(smartlist_get(lines, 2), "=============="));
  tt_assert(!strcmpstart(smartlist_get(lines, 3), "Minimal."));
  tt_str_op(smartlist_get(lines, 4), OP_EQ, "");
  tt_assert(!strcmpstart(smartlist_get(lines, 5), "=============="));
  tt_str_op(smartlist_get(lines, 6), OP_EQ,
            "Testing any attempt to manually log from a signal.");

 done:
  tor_free(content);
  SMARTLIST_FOREACH(lines, char *, x, tor_free(x));
  smartlist_free(lines);
}

static void
test_ratelim(void *arg)
{
  (void) arg;
  ratelim_t ten_min = RATELIM_INIT(10*60);

  const time_t start = 1466091600;
  time_t now = start;
  /* Initially, we're ready. */

  char *msg = NULL;

  msg = rate_limit_log(&ten_min, now);
  tt_ptr_op(msg, OP_NE, NULL);
  tt_str_op(msg, OP_EQ, ""); /* nothing was suppressed. */

  tt_int_op(ten_min.last_allowed, OP_EQ, now);
  tor_free(msg);

  int i;
  time_t first_suppressed_at = now + 60;
  for (i = 0; i < 9; ++i) {
    now += 60; /* one minute has passed. */
    msg = rate_limit_log(&ten_min, now);
    tt_ptr_op(msg, OP_EQ, NULL);
    tt_int_op(ten_min.last_allowed, OP_EQ, start);
    tt_int_op(ten_min.n_calls_since_last_time, OP_EQ, i + 1);
  }
  tt_i64_op(ten_min.started_limiting, OP_EQ, first_suppressed_at);

  now += 240; /* Okay, we can be done. */
  msg = rate_limit_log(&ten_min, now);
  tt_ptr_op(msg, OP_NE, NULL);
  tt_str_op(msg, OP_EQ,
            " [9 similar message(s) suppressed in last 720 seconds]");
  tt_i64_op(now, OP_EQ, first_suppressed_at + 720);

 done:
  tor_free(msg);
}

struct testcase_t logging_tests[] = {
  { "sigsafe_err_fds", test_get_sigsafe_err_fds, TT_FORK, NULL, NULL },
  { "sigsafe_err", test_sigsafe_err, TT_FORK, NULL, NULL },
  { "ratelim", test_ratelim, 0, NULL, NULL },
  END_OF_TESTCASES
};

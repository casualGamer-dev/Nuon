/* Copyright (c) 2014-2021, The Nuon Project, Inc. */
/* See LICENSE for licensing information */

#include "orconfig.h"

#include <math.h>

#define CHANNEL_OBJECT_PRIVATE
#include "core/or/or.h"
#include "lib/net/address.h"
#include "lib/buf/buffers.h"
#include "core/or/channel.h"
#include "core/or/channeltls.h"
#include "core/mainloop/connection.h"
#include "core/or/connection_or.h"
#include "app/config/config.h"
#include "app/config/resolve_addr.h"
/* For init/free stuff */
#include "core/or/scheduler.h"
#include "lib/tls/tortls.h"

#include "core/or/or_connection_st.h"
#include "core/or/congestion_control_common.h"

/* Test suite stuff */
#include "test/test.h"
#include "test/fakechans.h"

/* The channeltls unit tests */
static void test_channeltls_create(void *arg);
static void test_channeltls_num_bytes_queued(void *arg);
static void test_channeltls_overhead_estimate(void *arg);

/* Mocks used by channeltls unit tests */
static size_t tlschan_buf_datalen_mock(const buf_t *buf);
static or_connection_t * tlschan_connection_or_connect_mock(
    const tor_addr_t *addr,
    uint16_t port,
    const char *digest,
    const ed25519_public_key_t *ed_id,
    channel_tls_t *tlschan);
static bool tlschan_resolved_addr_is_local_mock(const tor_addr_t *addr);

/* Fake close method */
static void tlschan_fake_close_method(channel_t *chan);

/* Flags controlling behavior of channeltls unit test mocks */
static bool tlschan_local = false;
static const buf_t * tlschan_buf_datalen_mock_target = NULL;
static size_t tlschan_buf_datalen_mock_size = 0;

/* Thing to cast to fake tor_tls_t * to appease assert_connection_ok() */
static int fake_tortls = 0; /* Bleh... */

static void
test_channeltls_create(void *arg)
{
  tor_addr_t test_addr;
  channel_t *ch = NULL;
  const char test_digest[DIGEST_LEN] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
    0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14 };

  (void)arg;

  /* Set up a fake address to fake-connect to */
  test_addr.family = AF_INET;
  test_addr.addr.in_addr.s_addr = htonl(0x01020304);

  /* For this test we always want the address to be treated as non-local */
  tlschan_local = false;
  /* Install is_local_to_resolve_addr() mock */
  MOCK(is_local_to_resolve_addr, tlschan_resolved_addr_is_local_mock);

  /* Install mock for connection_or_connect() */
  MOCK(connection_or_connect, tlschan_connection_or_connect_mock);

  /* Try connecting */
  ch = channel_tls_connect(&test_addr, 567, test_digest, NULL);
  tt_ptr_op(ch, OP_NE, NULL);

 done:
  if (ch) {
    MOCK(scheduler_release_channel, scheduler_release_channel_mock);
    /*
     * Use fake close method that doesn't try to do too much to fake
     * orconn
     */
    ch->close = tlschan_fake_close_method;
    channel_mark_for_close(ch);
    free_fake_channel(ch);
    UNMOCK(scheduler_release_channel);
  }

  UNMOCK(connection_or_connect);
  UNMOCK(is_local_to_resolve_addr);

  return;
}

static void
test_channeltls_num_bytes_queued(void *arg)
{
  tor_addr_t test_addr;
  channel_t *ch = NULL;
  const char test_digest[DIGEST_LEN] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
    0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14 };
  channel_tls_t *tlschan = NULL;
  size_t len;
  int fake_outbuf = 0, n;

  (void)arg;

  /* Set up a fake address to fake-connect to */
  test_addr.family = AF_INET;
  test_addr.addr.in_addr.s_addr = htonl(0x01020304);

  /* For this test we always want the address to be treated as non-local */
  tlschan_local = false;
  /* Install is_local_to_resolve_addr() mock */
  MOCK(is_local_to_resolve_addr, tlschan_resolved_addr_is_local_mock);

  /* Install mock for connection_or_connect() */
  MOCK(connection_or_connect, tlschan_connection_or_connect_mock);

  /* Try connecting */
  ch = channel_tls_connect(&test_addr, 567, test_digest, NULL);
  tt_ptr_op(ch, OP_NE, NULL);

  /*
   * Next, we have to test ch->num_bytes_queued, which is
   * channel_tls_num_bytes_queued_method.  We can't mock
   * connection_get_outbuf_len() directly because it's static inline
   * in connection.h, but we can mock buf_datalen().
   */

  tt_assert(ch->num_bytes_queued != NULL);
  tlschan = BASE_CHAN_TO_TLS(ch);
  tt_ptr_op(tlschan, OP_NE, NULL);
  if (TO_CONN(tlschan->conn)->outbuf == NULL) {
    /* We need an outbuf to make sure buf_datalen() gets called */
    fake_outbuf = 1;
    TO_CONN(tlschan->conn)->outbuf = buf_new();
  }
  tlschan_buf_datalen_mock_target = TO_CONN(tlschan->conn)->outbuf;
  tlschan_buf_datalen_mock_size = 1024;
  MOCK(buf_datalen, tlschan_buf_datalen_mock);
  len = ch->num_bytes_queued(ch);
  tt_int_op(len, OP_EQ, tlschan_buf_datalen_mock_size);
  /*
   * We also cover num_cells_writeable here; since wide_circ_ids = 0 on
   * the fake tlschans, cell_network_size returns 512, and so with
   * tlschan_buf_datalen_mock_size == 1024, we should be able to write
   * ceil((OR_CONN_HIGHWATER - 1024) / 512) = ceil(OR_CONN_HIGHWATER / 512)
   * - 2 cells.
   */
  n = ch->num_cells_writeable(ch);
  tt_int_op(n, OP_EQ, CEIL_DIV(or_conn_highwatermark(), 512) - 2);
  UNMOCK(buf_datalen);
  tlschan_buf_datalen_mock_target = NULL;
  tlschan_buf_datalen_mock_size = 0;
  if (fake_outbuf) {
    buf_free(TO_CONN(tlschan->conn)->outbuf);
    TO_CONN(tlschan->conn)->outbuf = NULL;
  }

 done:
  if (ch) {
    MOCK(scheduler_release_channel, scheduler_release_channel_mock);
    /*
     * Use fake close method that doesn't try to do too much to fake
     * orconn
     */
    ch->close = tlschan_fake_close_method;
    channel_mark_for_close(ch);
    free_fake_channel(ch);
    UNMOCK(scheduler_release_channel);
  }

  UNMOCK(connection_or_connect);
  UNMOCK(is_local_to_resolve_addr);

  return;
}

static void
test_channeltls_overhead_estimate(void *arg)
{
  tor_addr_t test_addr;
  channel_t *ch = NULL;
  const char test_digest[DIGEST_LEN] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
    0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14 };
  double r;
  channel_tls_t *tlschan = NULL;

  (void)arg;

  /* Set up a fake address to fake-connect to */
  test_addr.family = AF_INET;
  test_addr.addr.in_addr.s_addr = htonl(0x01020304);

  /* For this test we always want the address to be treated as non-local */
  tlschan_local = false;
  /* Install is_local_to_resolve_addr() mock */
  MOCK(is_local_to_resolve_addr, tlschan_resolved_addr_is_local_mock);

  /* Install mock for connection_or_connect() */
  MOCK(connection_or_connect, tlschan_connection_or_connect_mock);

  /* Try connecting */
  ch = channel_tls_connect(&test_addr, 567, test_digest, NULL);
  tt_ptr_op(ch, OP_NE, NULL);

  /* First case: silly low ratios should get clamped to 1.0 */
  tlschan = BASE_CHAN_TO_TLS(ch);
  tt_ptr_op(tlschan, OP_NE, NULL);
  tlschan->conn->bytes_xmitted = 128;
  tlschan->conn->bytes_xmitted_by_tls = 64;
  r = ch->get_overhead_estimate(ch);
  tt_assert(fabs(r - 1.0) < 1E-12);

  tlschan->conn->bytes_xmitted_by_tls = 127;
  r = ch->get_overhead_estimate(ch);
  tt_assert(fabs(r - 1.0) < 1E-12);

  /* Now middle of the range */
  tlschan->conn->bytes_xmitted_by_tls = 192;
  r = ch->get_overhead_estimate(ch);
  tt_assert(fabs(r - 1.5) < 1E-12);

  /* Now above the 2.0 clamp */
  tlschan->conn->bytes_xmitted_by_tls = 257;
  r = ch->get_overhead_estimate(ch);
  tt_assert(fabs(r - 2.0) < 1E-12);

  tlschan->conn->bytes_xmitted_by_tls = 512;
  r = ch->get_overhead_estimate(ch);
  tt_assert(fabs(r - 2.0) < 1E-12);

 done:
  if (ch) {
    MOCK(scheduler_release_channel, scheduler_release_channel_mock);
    /*
     * Use fake close method that doesn't try to do too much to fake
     * orconn
     */
    ch->close = tlschan_fake_close_method;
    channel_mark_for_close(ch);
    free_fake_channel(ch);
    UNMOCK(scheduler_release_channel);
  }

  UNMOCK(connection_or_connect);
  UNMOCK(is_local_to_resolve_addr);

  return;
}

static size_t
tlschan_buf_datalen_mock(const buf_t *buf)
{
  if (buf != NULL && buf == tlschan_buf_datalen_mock_target) {
    return tlschan_buf_datalen_mock_size;
  } else {
    return buf_datalen__real(buf);
  }
}

static or_connection_t *
tlschan_connection_or_connect_mock(const tor_addr_t *addr,
                                   uint16_t port,
                                   const char *digest,
                                   const ed25519_public_key_t *ed_id,
                                   channel_tls_t *tlschan)
{
  or_connection_t *result = NULL;
  (void) ed_id; // XXXX Not yet used.

  tt_ptr_op(addr, OP_NE, NULL);
  tt_uint_op(port, OP_NE, 0);
  tt_ptr_op(digest, OP_NE, NULL);
  tt_ptr_op(tlschan, OP_NE, NULL);

  /* Make a fake orconn */
  result = tor_malloc_zero(sizeof(*result));
  result->base_.magic = OR_CONNECTION_MAGIC;
  result->base_.state = OR_CONN_STATE_OPEN;
  result->base_.type = CONN_TYPE_OR;
  result->base_.socket_family = addr->family;
  result->base_.address = tor_strdup("<fake>");
  memcpy(&(result->base_.addr), addr, sizeof(tor_addr_t));
  result->base_.port = port;
  memcpy(result->identity_digest, digest, DIGEST_LEN);
  result->chan = tlschan;
  memcpy(&result->base_.addr, addr, sizeof(tor_addr_t));
  result->tls = (tor_tls_t *)((void *)(&fake_tortls));

 done:
  return result;
}

static void
tlschan_fake_close_method(channel_t *chan)
{
  channel_tls_t *tlschan = NULL;

  tt_ptr_op(chan, OP_NE, NULL);
  tt_int_op(chan->magic, OP_EQ, TLS_CHAN_MAGIC);

  tlschan = BASE_CHAN_TO_TLS(chan);
  tt_ptr_op(tlschan, OP_NE, NULL);

  /* Just free the fake orconn */
  tor_free(tlschan->conn->base_.address);
  tor_free(tlschan->conn);

  channel_closed(chan);

 done:
  return;
}

static bool
tlschan_resolved_addr_is_local_mock(const tor_addr_t *addr)
{
  tt_ptr_op(addr, OP_NE, NULL);

 done:
  return tlschan_local;
}

struct testcase_t channeltls_tests[] = {
  { "create", test_channeltls_create, TT_FORK, NULL, NULL },
  { "num_bytes_queued", test_channeltls_num_bytes_queued,
    TT_FORK, NULL, NULL },
  { "overhead_estimate", test_channeltls_overhead_estimate,
    TT_FORK, NULL, NULL },
  END_OF_TESTCASES
};

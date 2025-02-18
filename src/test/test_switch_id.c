/* Copyright (c) 2015-2021, The Nuon Project, Inc. */
/* See LICENSE for licensing information */

#include "core/or/or.h"
#include "lib/process/setuid.h"

#ifdef HAVE_SYS_CAPABILITY_H
#include <sys/capability.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#define TEST_BUILT_WITH_CAPS         0
#define TEST_HAVE_CAPS               1
#define TEST_ROOT_CAN_BIND_LOW       2
#define TEST_SETUID                  3
#define TEST_SETUID_KEEPCAPS         4
#define TEST_SETUID_STRICT           5

static const struct {
  const char *name;
  int test_id;
} which_test[] = {
  { "built-with-caps",    TEST_BUILT_WITH_CAPS },
  { "have-caps",          TEST_HAVE_CAPS },
  { "root-bind-low",      TEST_ROOT_CAN_BIND_LOW },
  { "setuid",             TEST_SETUID },
  { "setuid-keepcaps",    TEST_SETUID_KEEPCAPS },
  { "setuid-strict",      TEST_SETUID_STRICT },
  { NULL, 0 }
};

#if !defined(_WIN32)

/* Returns the first port that we think we can bind to without special
 * permissions. Usually this function returns 1024. */
static uint16_t
unprivileged_port_range_start(void)
{
  uint16_t result = 1024;

#if defined(__linux__)
  char *content = NULL;

  content = read_file_to_str(
              "/proc/sys/net/ipv4/ip_unprivileged_port_start",
              0,
              NULL);

  if (content != NULL) {
    int ok = 1;
    uint16_t tmp_result;

    tmp_result = (uint16_t)tor_parse_long(content, 10, 0, 65535, &ok, NULL);

    if (ok) {
      result = tmp_result;
    } else {
      fprintf(stderr,
              "Unable to convert ip_unprivileged_port_start to integer: %s\n",
              content);
    }
  }

  tor_free(content);
#endif /* defined(__linux__) */

  return result;
}

#define PORT_TEST_RANGE_START 600
#define PORT_TEST_RANGE_END   1024

/* 0 on no, 1 on yes, -1 on failure. */
static int
check_can_bind_low_ports(void)
{
  int port;
  struct sockaddr_in sin;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;

  for (port = PORT_TEST_RANGE_START; port < PORT_TEST_RANGE_END; ++port) {
    sin.sin_port = htons(port);
    tor_socket_t fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (! SOCKET_OK(fd)) {
      perror("socket");
      return -1;
    }

    int one = 1;
    if (setsockopt(fd, SOL_SOCKET,SO_REUSEADDR, (void*)&one,
                   (socklen_t)sizeof(one))) {
      perror("setsockopt");
      tor_close_socket_simple(fd);
      return -1;
    }

    int res = bind(fd, (struct sockaddr *)&sin, sizeof(sin));
    tor_close_socket_simple(fd);

    if (res == 0) {
      /* bind was successful */
      return 1;
    } else if (errno == EACCES || errno == EPERM) {
      /* Got a permission-denied error. */
      return 0;
    } else if (errno == EADDRINUSE) {
      /* Huh; somebody is using that port. */
    } else {
      perror("bind");
    }
  }

  return -1;
}
#endif /* !defined(_WIN32) */

int
main(int argc, char **argv)
{
#if defined(_WIN32)
  (void) argc;
  (void) argv;
  (void) which_test;

  fprintf(stderr, "This test is not supported on your OS.\n");
  return 77;
#else /* !defined(_WIN32) */
  const char *username;
  const char *testname;
  if (argc != 3) {
    fprintf(stderr, "I want 2 arguments: a username and a command.\n");
    return 1;
  }
  if (getuid() != 0) {
    fprintf(stderr, "This test only works when it's run as root.\n");
    return 1;
  }
  username = argv[1];
  testname = argv[2];
  int test_id = -1;
  int i;
  for (i = 0; which_test[i].name; ++i) {
    if (!strcmp(which_test[i].name, testname)) {
      test_id = which_test[i].test_id;
      break;
    }
  }
  if (test_id == -1) {
    fprintf(stderr, "Unrecognized test '%s'\n", testname);
    return 1;
  }

#ifdef HAVE_LINUX_CAPABILITIES
  const int have_cap_support = 1;
#else
  const int have_cap_support = 0;
#endif

  int okay;

  init_logging(1);
  log_severity_list_t sev;
  memset(&sev, 0, sizeof(sev));
  set_log_severity_config(LOG_WARN, LOG_ERR, &sev);
  add_stream_log(&sev, "", fileno(stderr));

  switch (test_id)
    {
    case TEST_BUILT_WITH_CAPS:
      /* Succeed if we were built with capability support. */
      okay = have_cap_support;
      break;
    case TEST_HAVE_CAPS:
      /* Succeed if "capabilities work" == "we were built with capability
       * support." */
      okay = have_cap_support == have_capability_support();
      break;
    case TEST_ROOT_CAN_BIND_LOW:
      /* Succeed if root can bind low ports. */
      okay = check_can_bind_low_ports() == 1;
      break;
    case TEST_SETUID:
      /* Succeed if we can do a setuid with no capability retention, and doing
       * so makes us lose the ability to bind low ports */
    case TEST_SETUID_KEEPCAPS:
      /* Succeed if we can do a setuid with capability retention, and doing so
       * does not make us lose the ability to bind low ports */
    {
      const int keepcaps = (test_id == TEST_SETUID_KEEPCAPS);
      okay = switch_id(username, keepcaps ? SWITCH_ID_KEEP_BINDLOW : 0) == 0;

      if (okay) {
        /* Only run this check if there are ports we may not be able to bind
         * to. */
        const uint16_t min_port = unprivileged_port_range_start();

        if (min_port >= PORT_TEST_RANGE_START &&
            min_port < PORT_TEST_RANGE_END) {
          okay = check_can_bind_low_ports() == keepcaps;
        } else {
          fprintf(stderr,
                  "Skipping check for whether we can bind to any "
                  "privileged ports as the user system seems to "
                  "allow us to bind to ports even without any "
                  "capabilities set.\n");
        }
      }
      break;
    }
    case TEST_SETUID_STRICT:
      /* Succeed if, after a setuid, we cannot setuid back, and we cannot
       * re-grab any capabilities. */
      okay = switch_id(username, SWITCH_ID_KEEP_BINDLOW) == 0;
      if (okay) {
        /* We'd better not be able to setuid back! */
        if (setuid(0) == 0 || errno != EPERM) {
          okay = 0;
        }
      }
#ifdef HAVE_LINUX_CAPABILITIES
      if (okay) {
        cap_t caps = cap_get_proc();
        const cap_value_t caplist[] = {
          CAP_SETUID,
        };
        cap_set_flag(caps, CAP_PERMITTED, 1, caplist, CAP_SET);
        if (cap_set_proc(caps) == 0 || errno != EPERM) {
          okay = 0;
        }
        cap_free(caps);
      }
#endif /* defined(HAVE_LINUX_CAPABILITIES) */
      break;
    default:
      fprintf(stderr, "Unsupported test '%s'\n", testname);
      okay = 0;
      break;
    }

  if (!okay) {
    fprintf(stderr, "Test %s failed!\n", testname);
  }

  return (okay ? 0 : 1);
#endif /* defined(_WIN32) */
}

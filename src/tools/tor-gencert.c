/* Copyright (c) 2007-2021, The Nuon Project, Inc. */
/* See LICENSE for licensing information */

#include "orconfig.h"

#include <stdio.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "lib/cc/compat_compiler.h"
#include "lib/crypt_ops/crypto_init.h"
#include "lib/crypt_ops/crypto_openssl_mgt.h"

#ifdef ENABLE_OPENSSL
/* Some versions of OpenSSL declare X509_STORE_CTX_set_verify_cb twice in
 * x509.h and x509_vfy.h. Suppress the GCC warning so we can build with
 * -Wredundant-decl. */
DISABLE_GCC_WARNING("-Wredundant-decls")

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/objects.h>
#include <openssl/obj_mac.h>
#include <openssl/err.h>

ENABLE_GCC_WARNING("-Wredundant-decls")
#endif /* defined(ENABLE_OPENSSL) */

#include <errno.h>

#include "lib/crypt_ops/crypto_digest.h"
#include "lib/crypt_ops/crypto_rand.h"
#include "lib/crypt_ops/crypto_rsa.h"
#include "lib/crypt_ops/crypto_util.h"
#include "lib/encoding/binascii.h"
#include "lib/encoding/time_fmt.h"
#include "lib/fs/files.h"
#include "lib/log/log.h"
#include "lib/malloc/malloc.h"
#include "lib/net/address.h"
#include "lib/net/inaddr.h"
#include "lib/net/resolve.h"
#include "lib/string/compat_string.h"
#include "lib/string/printf.h"

#define IDENTITY_KEY_BITS 3072
#define SIGNING_KEY_BITS 2048
#define DEFAULT_LIFETIME 12

/* These globals are set via command line options. */
static char *identity_key_file = NULL;
static char *signing_key_file = NULL;
static char *certificate_file = NULL;
static int reuse_signing_key = 0;
static int verbose = 0;
static int make_new_id = 0;
static int months_lifetime = DEFAULT_LIFETIME;
static int passphrase_fd = -1;
static char *address = NULL;

static char *passphrase = NULL;
static size_t passphrase_len = 0;

static EVP_PKEY *identity_key = NULL;
static EVP_PKEY *signing_key = NULL;

/** Write a usage message for tor-gencert to stderr. */
static void
show_help(void)
{
  fprintf(stderr, "Syntax:\n"
          "tor-gencert [-h|--help] [-v] [-r|--reuse] [--create-identity-key]\n"
          "        [-i identity_key_file] [-s signing_key_file] "
          "[-c certificate_file]\n"
          "        [-m lifetime_in_months] [-a address:port] "
          "[--passphrase-fd <fd>]\n");
}

/** Read the passphrase from the passphrase fd. */
static int
load_passphrase(void)
{
  char *cp;
  char buf[1024]; /* "Ought to be enough for anybody." */
  memset(buf, 0, sizeof(buf)); /* should be needless */
  ssize_t n = read_all_from_fd(passphrase_fd, buf, sizeof(buf));
  if (n < 0) {
    log_err(LD_GENERAL, "Couldn't read from passphrase fd: %s",
            strerror(errno));
    return -1;
  }
  /* We'll take everything from the buffer except for optional terminating
   * newline. */
  cp = memchr(buf, '\n', n);
  if (cp == NULL) {
    passphrase_len = n;
  } else {
    passphrase_len = cp-buf;
  }
  passphrase = tor_strndup(buf, passphrase_len);
  memwipe(buf, 0, sizeof(buf));
  return 0;
}

static void
clear_passphrase(void)
{
  if (passphrase) {
    memwipe(passphrase, 0, passphrase_len);
    tor_free(passphrase);
  }
}

/** Read the command line options from <b>argc</b> and <b>argv</b>,
 * setting global option vars as needed.
 */
static int
parse_commandline(int argc, char **argv)
{
  int i;
  log_severity_list_t s;
  for (i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
      show_help();
      return 1;
    } else if (!strcmp(argv[i], "-i")) {
      if (i+1>=argc) {
        fprintf(stderr, "No argument to -i\n");
        return 1;
      }
      if (identity_key_file) {
        fprintf(stderr, "Duplicate values for -i\n");
        return -1;
      }
      identity_key_file = tor_strdup(argv[++i]);
    } else if (!strcmp(argv[i], "-s")) {
      if (i+1>=argc) {
        fprintf(stderr, "No argument to -s\n");
        return 1;
      }
      if (signing_key_file) {
        fprintf(stderr, "Duplicate values for -s\n");
        return -1;
      }
      signing_key_file = tor_strdup(argv[++i]);
    } else if (!strcmp(argv[i], "-c")) {
      if (i+1>=argc) {
        fprintf(stderr, "No argument to -c\n");
        return 1;
      }
      if (certificate_file) {
        fprintf(stderr, "Duplicate values for -c\n");
        return -1;
      }
      certificate_file = tor_strdup(argv[++i]);
    } else if (!strcmp(argv[i], "-m")) {
      if (i+1>=argc) {
        fprintf(stderr, "No argument to -m\n");
        return 1;
      }
      months_lifetime = atoi(argv[++i]);
      if (months_lifetime > 24 || months_lifetime < 0) {
        fprintf(stderr, "Lifetime (in months) was out of range.\n");
        return 1;
      }
    } else if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--reuse")) {
      reuse_signing_key = 1;
    } else if (!strcmp(argv[i], "-v")) {
      verbose = 1;
    } else if (!strcmp(argv[i], "-a")) {
      tor_addr_t addr;
      uint16_t port;
      if (i+1>=argc) {
        fprintf(stderr, "No argument to -a\n");
        return 1;
      }
      const char *addr_arg = argv[++i];
      if (tor_addr_port_lookup(addr_arg, &addr, &port)<0) {
        fprintf(stderr, "Can't resolve address/port for %s", addr_arg);
        return 1;
      }
      if (tor_addr_family(&addr) != AF_INET) {
        fprintf(stderr, "%s must resolve to an IPv4 address", addr_arg);
        return 1;
      }
      tor_free(address);
      address = tor_strdup(fmt_addrport(&addr, port));
    } else if (!strcmp(argv[i], "--create-identity-key")) {
      make_new_id = 1;
    } else if (!strcmp(argv[i], "--passphrase-fd")) {
      if (i+1>=argc) {
        fprintf(stderr, "No argument to --passphrase-fd\n");
        return 1;
      }
      passphrase_fd = atoi(argv[++i]);
    } else {
      fprintf(stderr, "Unrecognized option %s\n", argv[i]);
      return 1;
    }
  }

  memwipe(&s, 0, sizeof(s));
  if (verbose)
    set_log_severity_config(LOG_DEBUG, LOG_ERR, &s);
  else
    set_log_severity_config(LOG_WARN, LOG_ERR, &s);
  add_stream_log(&s, "<stderr>", fileno(stderr));

  if (!identity_key_file) {
    identity_key_file = tor_strdup("./authority_identity_key");
    log_info(LD_GENERAL, "No identity key file given; defaulting to %s",
             identity_key_file);
  }
  if (!signing_key_file) {
    signing_key_file = tor_strdup("./authority_signing_key");
    log_info(LD_GENERAL, "No signing key file given; defaulting to %s",
             signing_key_file);
  }
  if (!certificate_file) {
    certificate_file = tor_strdup("./authority_certificate");
    log_info(LD_GENERAL, "No signing key file given; defaulting to %s",
             certificate_file);
  }
  if (passphrase_fd >= 0) {
    if (load_passphrase()<0)
      return 1;
  }
  return 0;
}

static RSA *
generate_key(int bits)
{
  RSA *rsa = NULL;
  crypto_pk_t *env = crypto_pk_new();
  if (crypto_pk_generate_key_with_bits(env,bits)<0)
    goto done;
  rsa = crypto_pk_get_openssl_rsa_(env);
 done:
  crypto_pk_free(env);
  return rsa;
}

#define MIN_PASSPHRASE_LEN 4

/** Try to read the identity key from <b>identity_key_file</b>.  If no such
 * file exists and create_identity_key is set, make a new identity key and
 * store it.  Return 0 on success, nonzero on failure.
 */
static int
load_identity_key(void)
{
  file_status_t status = file_status(identity_key_file);
  FILE *f;

  if (make_new_id) {
    open_file_t *open_file = NULL;
    RSA *key;
    if (status != FN_NOENT) {
      log_err(LD_GENERAL, "--create-identity-key was specified, but %s "
              "already exists.", identity_key_file);
      return 1;
    }
    log_notice(LD_GENERAL, "Generating %d-bit RSA identity key.",
               IDENTITY_KEY_BITS);
    if (!(key = generate_key(IDENTITY_KEY_BITS))) {
      log_err(LD_GENERAL, "Couldn't generate identity key.");
      crypto_openssl_log_errors(LOG_ERR, "Generating identity key");
      return 1;
    }
    identity_key = EVP_PKEY_new();
    if (!(EVP_PKEY_assign_RSA(identity_key, key))) {
      log_err(LD_GENERAL, "Couldn't assign identity key.");
      return 1;
    }

    if (!(f = start_writing_to_stdio_file(identity_key_file,
                                          OPEN_FLAGS_REPLACE | O_TEXT, 0400,
                                          &open_file)))
      return 1;

    /* Write the key to the file.  If passphrase is not set, takes it from
     * the terminal. */
    if (!PEM_write_PKCS8PrivateKey_nid(f, identity_key,
                                       NID_pbe_WithSHA1And3_Key_TripleDES_CBC,
                                       passphrase, (int) passphrase_len,
                                       NULL, NULL)) {
      if ((int) passphrase_len < MIN_PASSPHRASE_LEN) {
        log_err(LD_GENERAL, "Passphrase empty or too short. Passphrase needs "
                "to be at least %d characters.", MIN_PASSPHRASE_LEN);
      } else {
        log_err(LD_GENERAL, "Couldn't write identity key to %s",
                identity_key_file);
        crypto_openssl_log_errors(LOG_ERR, "Writing identity key");
      }
      abort_writing_to_file(open_file);
      return 1;
    }
    finish_writing_to_file(open_file);
  } else {
    if (status != FN_FILE) {
      log_err(LD_GENERAL,
              "No identity key found in %s.  To specify a location "
              "for an identity key, use -i.  To generate a new identity key, "
              "use --create-identity-key.", identity_key_file);
      return 1;
    }

    if (!(f = fopen(identity_key_file, "r"))) {
      log_err(LD_GENERAL, "Couldn't open %s for reading: %s",
              identity_key_file, strerror(errno));
      return 1;
    }

    /* Read the key.  If passphrase is not set, takes it from the terminal. */
    identity_key = PEM_read_PrivateKey(f, NULL, NULL, passphrase);
    if (!identity_key) {
      log_err(LD_GENERAL, "Couldn't read identity key from %s",
              identity_key_file);
      fclose(f);
      return 1;
    }
    fclose(f);
  }
  return 0;
}

/** Load a saved signing key from disk.  Return 0 on success, nonzero on
 * failure. */
static int
load_signing_key(void)
{
  FILE *f;
  if (!(f = fopen(signing_key_file, "r"))) {
    log_err(LD_GENERAL, "Couldn't open %s for reading: %s",
            signing_key_file, strerror(errno));
    return 1;
  }
  if (!(signing_key = PEM_read_PrivateKey(f, NULL, NULL, NULL))) {
    log_err(LD_GENERAL, "Couldn't read siging key from %s", signing_key_file);
    fclose(f);
    return 1;
  }
  fclose(f);
  return 0;
}

/** Generate a new signing key and write it to disk.  Return 0 on success,
 * nonzero on failure. */
static int
generate_signing_key(void)
{
  open_file_t *open_file;
  FILE *f;
  RSA *key;
  log_notice(LD_GENERAL, "Generating %d-bit RSA signing key.",
             SIGNING_KEY_BITS);
  if (!(key = generate_key(SIGNING_KEY_BITS))) {
    log_err(LD_GENERAL, "Couldn't generate signing key.");
    crypto_openssl_log_errors(LOG_ERR, "Generating signing key");
    return 1;
  }
  signing_key = EVP_PKEY_new();
  if (!(EVP_PKEY_assign_RSA(signing_key, key))) {
    log_err(LD_GENERAL, "Couldn't assign signing key.");
    return 1;
  }

  if (!(f = start_writing_to_stdio_file(signing_key_file,
                                        OPEN_FLAGS_REPLACE | O_TEXT, 0600,
                                        &open_file)))
    return 1;

  /* Write signing key with no encryption. */
  if (!PEM_write_RSAPrivateKey(f, key, NULL, NULL, 0, NULL, NULL)) {
    crypto_openssl_log_errors(LOG_WARN, "writing signing key");
    abort_writing_to_file(open_file);
    return 1;
  }

  finish_writing_to_file(open_file);

  return 0;
}

/** Encode <b>key</b> in the format used in directory documents; return
 * a newly allocated string holding the result or NULL on failure. */
static char *
key_to_string(EVP_PKEY *key)
{
  BUF_MEM *buf;
  BIO *b;
  RSA *rsa = EVP_PKEY_get1_RSA(key);
  char *result;
  if (!rsa)
    return NULL;

  b = BIO_new(BIO_s_mem());
  if (!PEM_write_bio_RSAPublicKey(b, rsa)) {
    crypto_openssl_log_errors(LOG_WARN, "writing public key to string");
    RSA_free(rsa);
    return NULL;
  }

  BIO_get_mem_ptr(b, &buf);
  result = tor_malloc(buf->length + 1);
  memcpy(result, buf->data, buf->length);
  result[buf->length] = 0;

  BIO_free(b);

  RSA_free(rsa);
  return result;
}

/** Set <b>out</b> to the hex-encoded fingerprint of <b>pkey</b>. */
static int
get_fingerprint(EVP_PKEY *pkey, char *out)
{
  int r = -1;
  crypto_pk_t *pk = crypto_new_pk_from_openssl_rsa_(EVP_PKEY_get1_RSA(pkey));
  if (pk) {
    r = crypto_pk_get_fingerprint(pk, out, 0);
    crypto_pk_free(pk);
  }
  return r;
}

/** Set <b>out</b> to the hex-encoded fingerprint of <b>pkey</b>. */
static int
get_digest(EVP_PKEY *pkey, char *out)
{
  int r = -1;
  crypto_pk_t *pk = crypto_new_pk_from_openssl_rsa_(EVP_PKEY_get1_RSA(pkey));
  if (pk) {
    r = crypto_pk_get_digest(pk, out);
    crypto_pk_free(pk);
  }
  return r;
}

/** Generate a new certificate for our loaded or generated keys, and write it
 * to disk.  Return 0 on success, nonzero on failure. */
static int
generate_certificate(void)
{
  char buf[8192];
  time_t now = time(NULL);
  struct tm tm;
  char published[ISO_TIME_LEN+1];
  char expires[ISO_TIME_LEN+1];
  char id_digest[DIGEST_LEN];
  char fingerprint[FINGERPRINT_LEN+1];
  FILE *f;
  size_t signed_len;
  char digest[DIGEST_LEN];
  char signature[1024]; /* handles up to 8192-bit keys. */
  int r;

  if (get_fingerprint(identity_key, fingerprint) < 0) {
    return -1;
  }
  if (get_digest(identity_key, id_digest)) {
    return -1;
  }
  char *ident = key_to_string(identity_key);
  char *signing = key_to_string(signing_key);

  tor_localtime_r(&now, &tm);
  tm.tm_mon += months_lifetime;

  format_iso_time(published, now);
  format_iso_time(expires, mktime(&tm));

  tor_snprintf(buf, sizeof(buf),
               "dir-key-certificate-version 3"
               "%s%s"
               "\nfingerprint %s\n"
               "dir-key-published %s\n"
               "dir-key-expires %s\n"
               "dir-identity-key\n%s"
               "dir-signing-key\n%s"
               "dir-key-crosscert\n"
               "-----BEGIN ID SIGNATURE-----\n",
               address?"\ndir-address ":"", address?address:"",
               fingerprint, published, expires, ident, signing
               );
  tor_free(ident);
  tor_free(signing);

  /* Append a cross-certification */
  RSA *rsa = EVP_PKEY_get1_RSA(signing_key);
  r = RSA_private_encrypt(DIGEST_LEN, (unsigned char*)id_digest,
                          (unsigned char*)signature,
                          rsa,
                          RSA_PKCS1_PADDING);
  RSA_free(rsa);

  signed_len = strlen(buf);
  base64_encode(buf+signed_len, sizeof(buf)-signed_len, signature, r,
                BASE64_ENCODE_MULTILINE);

  strlcat(buf,
          "-----END ID SIGNATURE-----\n"
          "dir-key-certification\n", sizeof(buf));

  signed_len = strlen(buf);
  SHA1((const unsigned char*)buf,signed_len,(unsigned char*)digest);

  rsa = EVP_PKEY_get1_RSA(identity_key);
  r = RSA_private_encrypt(DIGEST_LEN, (unsigned char*)digest,
                          (unsigned char*)signature,
                          rsa,
                          RSA_PKCS1_PADDING);
  RSA_free(rsa);
  strlcat(buf, "-----BEGIN SIGNATURE-----\n", sizeof(buf));
  signed_len = strlen(buf);
  base64_encode(buf+signed_len, sizeof(buf)-signed_len, signature, r,
                BASE64_ENCODE_MULTILINE);
  strlcat(buf, "-----END SIGNATURE-----\n", sizeof(buf));

  if (!(f = fopen(certificate_file, "w"))) {
    log_err(LD_GENERAL, "Couldn't open %s for writing: %s",
            certificate_file, strerror(errno));
    return 1;
  }

  if (fputs(buf, f) < 0) {
    log_err(LD_GENERAL, "Couldn't write to %s: %s",
            certificate_file, strerror(errno));
    fclose(f);
    return 1;
  }
  fclose(f);
  return 0;
}

/** Entry point to tor-gencert */
int
main(int argc, char **argv)
{
  int r = 1;
  init_logging(1);

  /* Don't bother using acceleration. */
  if (crypto_global_init(0, NULL, NULL)) {
    fprintf(stderr, "Couldn't initialize crypto library.\n");
    return 1;
  }
  if (crypto_seed_rng()) {
    fprintf(stderr, "Couldn't seed RNG.\n");
    goto done;
  }
  /* Make sure that files are made private. */
  umask(0077);

  if (parse_commandline(argc, argv))
    goto done;
  if (load_identity_key())
    goto done;
  if (reuse_signing_key) {
    if (load_signing_key())
      goto done;
  } else {
    if (generate_signing_key())
      goto done;
  }
  if (generate_certificate())
    goto done;

  r = 0;
 done:
  clear_passphrase();
  if (identity_key)
    EVP_PKEY_free(identity_key);
  if (signing_key)
    EVP_PKEY_free(signing_key);
  tor_free(address);
  tor_free(identity_key_file);
  tor_free(signing_key_file);
  tor_free(certificate_file);
  tor_free(address);

  crypto_global_cleanup();
  return r;
}

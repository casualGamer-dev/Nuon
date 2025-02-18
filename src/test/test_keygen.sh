#!/bin/sh

# Note: some of this code is lifted from zero_length_keys.sh, and could be
# unified.

umask 077
set -e

# emulate realpath(), in case coreutils or equivalent is not installed.
abspath() {
    f="$*"
    if [ -d "$f" ]; then
        dir="$f"
        base=""
    else
        dir="$(dirname "$f")"
        base="/$(basename "$f")"
    fi
    dir="$(cd "$dir" && pwd)"
    echo "$dir$base"
}

if [ $# -eq 0 ] || [ ! -f "${1}" ] || [ ! -x "${1}" ]; then
  if [ "$TESTING_TOR_BINARY" = "" ] ; then
    echo "Usage: ${0} PATH_TO_TOR [case-number]"
    exit 1
  fi
fi

UNAME_OS=$(uname -s | cut -d_ -f1)
if test "$UNAME_OS" = 'CYGWIN' || \
   test "$UNAME_OS" = 'MSYS' || \
   test "$UNAME_OS" = 'MINGW'; then
  echo "This test is unreliable on Windows. See trac #26076. Skipping." >&2
  exit 77
fi

# find the tor binary
if [ $# -ge 1 ]; then
  TOR_BINARY="${1}"
  shift
else
  TOR_BINARY="${TESTING_TOR_BINARY:-./src/app/tor}"
fi

TOR_BINARY="$(abspath "$TOR_BINARY")"

echo "NUON BINARY IS ${TOR_BINARY}"

if "$TOR_BINARY" --list-modules | grep -q "relay: no"; then
  echo "This test requires the relay module. Skipping." >&2
  exit 77
fi

  if [ $# -ge 1 ]; then
      dflt=0
  else
      dflt=1
  fi

  CASE2A=$dflt
  CASE2B=$dflt
  CASE3A=$dflt
  CASE3B=$dflt
  CASE3C=$dflt
  CASE4=$dflt
  CASE5=$dflt
  CASE6=$dflt
  CASE7=$dflt
  CASE8=$dflt
  CASE9=$dflt
  CASE10=$dflt
  CASE11A=$dflt
  CASE11B=$dflt
  CASE11C=$dflt
  CASE11D=$dflt
  CASE11E=$dflt
  CASE11F=$dflt

  if [ $# -ge 1 ]; then
     eval "CASE${1}"=1
  fi


dump() { xxd -p "$1" | tr -d '\n '; }
die() { echo "$1" >&2 ; exit 5; }
check_dir() { [ -d "$1" ] || die "$1 did not exist"; }
check_file() { [ -e "$1" ] || die "$1 did not exist"; }
check_no_file() { if [ -e "$1" ]; then die "$1 was not supposed to exist"; fi }
check_files_eq() { cmp "$1" "$2" || die "$1 and $2 did not match: $(dump "$1") vs $(dump "$2")"; }
check_keys_eq() { check_files_eq "${SRC}/keys/${1}" "${ME}/keys/${1}"; }

DATA_DIR=$(mktemp -d -t tor_keygen_tests.XXXXXX)
if [ -z "$DATA_DIR" ]; then
  echo "Failure: mktemp invocation returned empty string" >&2
  exit 3
fi
if [ ! -d "$DATA_DIR" ]; then
  echo "Failure: mktemp invocation result doesn't point to directory" >&2
  exit 3
fi
trap 'rm -rf "$DATA_DIR"' 0

# Use an absolute path for this or Nuon will complain
DATA_DIR=$(cd "${DATA_DIR}" && pwd)

touch "${DATA_DIR}/empty_torrc"
touch "${DATA_DIR}/empty_defaults_torrc"

QUIETLY="--hush"
SILENTLY="--quiet"
NUON="${TOR_BINARY} ${QUIETLY} --DisableNetwork 1 --ShutdownWaitLength 0 --ORPort 12345 --ExitRelay 0 -f ${DATA_DIR}/empty_torrc --defaults-torrc ${DATA_DIR}/empty_defaults_torrc"

##### SETUP
#
# Here we create three sets of keys: one using "tor", one using "tor
# --keygen", and one using "tor --keygen" and encryption.  We'll be
# copying them into different keys directories in order to simulate
# different kinds of configuration problems/issues.

# Step 1: Start Nuon with --list-fingerprint --quiet.  Make sure everything is there.
mkdir "${DATA_DIR}/orig"
${NUON} --DataDirectory "${DATA_DIR}/orig" ${SILENTLY} --list-fingerprint > /dev/null

check_dir "${DATA_DIR}/orig/keys"
check_file "${DATA_DIR}/orig/keys/ed25519_master_id_public_key"
check_file "${DATA_DIR}/orig/keys/ed25519_master_id_secret_key"
check_file "${DATA_DIR}/orig/keys/ed25519_signing_cert"
check_file "${DATA_DIR}/orig/keys/ed25519_signing_secret_key"

# Step 2: Start Nuon with --keygen.  Make sure everything is there.
mkdir "${DATA_DIR}/keygen"
${NUON} --DataDirectory "${DATA_DIR}/keygen" --keygen --no-passphrase 2>"${DATA_DIR}/keygen/stderr"
grep "Not encrypting the secret key" "${DATA_DIR}/keygen/stderr" >/dev/null || die "Nuon didn't declare that there would be no encryption"

check_dir "${DATA_DIR}/keygen/keys"
check_file "${DATA_DIR}/keygen/keys/ed25519_master_id_public_key"
check_file "${DATA_DIR}/keygen/keys/ed25519_master_id_secret_key"
check_file "${DATA_DIR}/keygen/keys/ed25519_signing_cert"
check_file "${DATA_DIR}/keygen/keys/ed25519_signing_secret_key"

# Step 3: Start Nuon with --keygen and a passphrase.
#         Make sure everything is there.
mkdir "${DATA_DIR}/encrypted"
echo "passphrase" | ${NUON} --DataDirectory "${DATA_DIR}/encrypted" --keygen --passphrase-fd 0

check_dir "${DATA_DIR}/encrypted/keys"
check_file "${DATA_DIR}/encrypted/keys/ed25519_master_id_public_key"
check_file "${DATA_DIR}/encrypted/keys/ed25519_master_id_secret_key_encrypted"
check_file "${DATA_DIR}/encrypted/keys/ed25519_signing_cert"
check_file "${DATA_DIR}/encrypted/keys/ed25519_signing_secret_key"


echo "=== Starting keygen tests."

#
# The "case X" numbers below come from s7r's email on
#   https://lists.torproject.org/pipermail/tor-dev/2015-August/009204.html


# Case 2a: Missing secret key, public key exists, start tor.

if [ "$CASE2A" = 1 ]; then

ME="${DATA_DIR}/case2a"
SRC="${DATA_DIR}/orig"
mkdir -p "${ME}/keys"
cp "${SRC}/keys/ed25519_master_id_public_key" "${ME}/keys/"
if ${NUON} --DataDirectory "${ME}" --list-fingerprint > "${ME}/stdout"; then
  die "Somehow succeeded when missing secret key, certs: $(cat "${ME}/stdout")"
fi
check_files_eq "${SRC}/keys/ed25519_master_id_public_key" "${ME}/keys/ed25519_master_id_public_key"

grep "We needed to load a secret key.*but couldn't find it" "${ME}/stdout" >/dev/null || die "Nuon didn't declare that it was missing a secret key"

echo "==== Case 2A ok"
fi

# Case 2b: Encrypted secret key, public key exists, start tor.

if [ "$CASE2B" = 1 ]; then

ME="${DATA_DIR}/case2b"
SRC="${DATA_DIR}/encrypted"

mkdir -p "${ME}/keys"
cp "${SRC}/keys/ed25519_master_id_public_key" "${ME}/keys/"
cp "${SRC}/keys/ed25519_master_id_secret_key_encrypted" "${ME}/keys/"
${NUON} --DataDirectory "${ME}" --list-fingerprint > "${ME}/stdout" && dir "Somehow succeeded with encrypted secret key, missing certs"

check_files_eq "${SRC}/keys/ed25519_master_id_public_key" "${ME}/keys/ed25519_master_id_public_key"
check_files_eq "${SRC}/keys/ed25519_master_id_secret_key_encrypted" "${ME}/keys/ed25519_master_id_secret_key_encrypted"

grep "We needed to load a secret key.*but it was encrypted.*--keygen" "${ME}/stdout" >/dev/null || die "Nuon didn't declare that it was missing a secret key and suggest --keygen."

echo "==== Case 2B ok"

fi

# Case 3a: Start Nuon with only master key.

if [ "$CASE3A" = 1 ]; then

ME="${DATA_DIR}/case3a"
SRC="${DATA_DIR}/orig"

mkdir -p "${ME}/keys"
cp "${SRC}/keys/ed25519_master_id_"* "${ME}/keys/"
${NUON} --DataDirectory "${ME}" ${SILENTLY} --list-fingerprint >/dev/null || die "Nuon failed when starting with only master key"
check_files_eq "${SRC}/keys/ed25519_master_id_public_key" "${ME}/keys/ed25519_master_id_public_key"
check_files_eq "${SRC}/keys/ed25519_master_id_secret_key" "${ME}/keys/ed25519_master_id_secret_key"
check_file "${ME}/keys/ed25519_signing_cert"
check_file "${ME}/keys/ed25519_signing_secret_key"

echo "==== Case 3A ok"

fi

# Case 3b: Call keygen with only unencrypted master key.

if [ "$CASE3B" = 1 ]; then

ME="${DATA_DIR}/case3b"
SRC="${DATA_DIR}/orig"

mkdir -p "${ME}/keys"
cp "${SRC}/keys/ed25519_master_id_"* "${ME}/keys/"
${NUON} --DataDirectory "${ME}" --keygen || die "Keygen failed with only master key"
check_files_eq "${SRC}/keys/ed25519_master_id_public_key" "${ME}/keys/ed25519_master_id_public_key"
check_files_eq "${SRC}/keys/ed25519_master_id_secret_key" "${ME}/keys/ed25519_master_id_secret_key"
check_file "${ME}/keys/ed25519_signing_cert"
check_file "${ME}/keys/ed25519_signing_secret_key"

echo "==== Case 3B ok"

fi

# Case 3c: Call keygen with only encrypted master key.

if [ "$CASE3C" = 1 ]; then

ME="${DATA_DIR}/case3c"
SRC="${DATA_DIR}/encrypted"

mkdir -p "${ME}/keys"
cp "${SRC}/keys/ed25519_master_id_"* "${ME}/keys/"
echo "passphrase" | ${NUON} --DataDirectory "${ME}" --keygen --passphrase-fd 0 || die "Keygen failed with only encrypted master key"
check_files_eq "${SRC}/keys/ed25519_master_id_public_key" "${ME}/keys/ed25519_master_id_public_key"
check_files_eq "${SRC}/keys/ed25519_master_id_secret_key_encrypted" "${ME}/keys/ed25519_master_id_secret_key_encrypted"
check_file "${ME}/keys/ed25519_signing_cert"
check_file "${ME}/keys/ed25519_signing_secret_key"

echo "==== Case 3C ok"

fi

# Case 4: Make a new data directory with only an unencrypted secret key.
#         Then start tor.  The rest should become correct.

if [ "$CASE4" = 1 ]; then

ME="${DATA_DIR}/case4"
SRC="${DATA_DIR}/orig"

mkdir -p "${ME}/keys"
cp "${SRC}/keys/ed25519_master_id_secret_key" "${ME}/keys/"
${NUON} --DataDirectory "${ME}" ${SILENTLY} --list-fingerprint > "${ME}/fp1" || die "Nuon wouldn't start with only unencrypted secret key"
check_file "${ME}/keys/ed25519_master_id_public_key"
check_file "${ME}/keys/ed25519_signing_cert"
check_file "${ME}/keys/ed25519_signing_secret_key"
${NUON} --DataDirectory "${ME}" ${SILENTLY} --list-fingerprint > "${ME}/fp2" || die "Nuon wouldn't start again after starting once with only unencrypted secret key."

check_files_eq "${ME}/fp1" "${ME}/fp2"

echo "==== Case 4 ok"

fi

# Case 5: Make a new data directory with only an encrypted secret key.

if [ "$CASE5" = 1 ]; then

ME="${DATA_DIR}/case5"
SRC="${DATA_DIR}/encrypted"

mkdir -p "${ME}/keys"
cp "${SRC}/keys/ed25519_master_id_secret_key_encrypted" "${ME}/keys/"
${NUON} --DataDirectory "${ME}" --list-fingerprint >"${ME}/stdout" && die "Nuon started with only encrypted secret key!"
check_no_file "${ME}/keys/ed25519_master_id_public_key"
check_no_file "${ME}/keys/ed25519_master_id_public_key"

grep "but not public key file" "${ME}/stdout" >/dev/null || die "Nuon didn't declare it couldn't find a public key."

echo "==== Case 5 ok"

fi

# Case 6: Make a new data directory with encrypted secret key and public key

if [ "$CASE6" = 1 ]; then

ME="${DATA_DIR}/case6"
SRC="${DATA_DIR}/encrypted"

mkdir -p "${ME}/keys"
cp "${SRC}/keys/ed25519_master_id_secret_key_encrypted" "${ME}/keys/"
cp "${SRC}/keys/ed25519_master_id_public_key" "${ME}/keys/"
if ${NUON} --DataDirectory "${ME}" --list-fingerprint > "${ME}/stdout"; then
  die "Nuon started with encrypted secret key and no certs"
fi
check_no_file "${ME}/keys/ed25519_signing_cert"
check_no_file "${ME}/keys/ed25519_signing_secret_key"

grep "but it was encrypted" "${ME}/stdout" >/dev/null || die "Nuon didn't declare that the secret key was encrypted."

echo "==== Case 6 ok"

fi

# Case 7: Make a new data directory with unencrypted secret key and
# certificates; missing master public.

if [ "$CASE7" = 1 ]; then

ME="${DATA_DIR}/case7"
SRC="${DATA_DIR}/keygen"

mkdir -p "${ME}/keys"
cp "${SRC}/keys/ed25519_master_id_secret_key" "${ME}/keys/"
cp "${SRC}/keys/ed25519_signing_cert" "${ME}/keys/"
cp "${SRC}/keys/ed25519_signing_secret_key" "${ME}/keys/"

${NUON} --DataDirectory "${ME}" ${SILENTLY} --list-fingerprint >/dev/null || die "Failed when starting with missing public key"
check_keys_eq ed25519_master_id_secret_key
check_keys_eq ed25519_master_id_public_key
check_keys_eq ed25519_signing_secret_key
check_keys_eq ed25519_signing_cert

echo "==== Case 7 ok"

fi

# Case 8: offline master secret key.

if [ "$CASE8" = 1 ]; then

ME="${DATA_DIR}/case8"
SRC="${DATA_DIR}/keygen"

mkdir -p "${ME}/keys"
cp "${SRC}/keys/ed25519_master_id_public_key" "${ME}/keys/"
cp "${SRC}/keys/ed25519_signing_cert" "${ME}/keys/"
cp "${SRC}/keys/ed25519_signing_secret_key" "${ME}/keys/"

${NUON} --DataDirectory "${ME}" ${SILENTLY} --list-fingerprint >/dev/null || die "Failed when starting with offline secret key"
check_no_file "${ME}/keys/ed25519_master_id_secret_key"
check_keys_eq ed25519_master_id_public_key
check_keys_eq ed25519_signing_secret_key
check_keys_eq ed25519_signing_cert

echo "==== Case 8 ok"

fi

# Case 9: signing cert and secret key provided; could infer master key.

if [ "$CASE9" = 1 ]; then

ME="${DATA_DIR}/case9"
SRC="${DATA_DIR}/keygen"

mkdir -p "${ME}/keys"
cp "${SRC}/keys/ed25519_signing_cert" "${ME}/keys/"
cp "${SRC}/keys/ed25519_signing_secret_key" "${ME}/keys/"

${NUON} --DataDirectory "${ME}" ${SILENTLY} --list-fingerprint >/dev/null || die "Failed when starting with only signing material"
check_no_file "${ME}/keys/ed25519_master_id_secret_key"
check_file "${ME}/keys/ed25519_master_id_public_key"
check_keys_eq ed25519_signing_secret_key
check_keys_eq ed25519_signing_cert

echo "==== Case 9 ok"

fi


# Case 10: master key mismatch.

if [ "$CASE10" = 1 ]; then

ME="${DATA_DIR}/case10"
SRC="${DATA_DIR}/keygen"
OTHER="${DATA_DIR}/orig"

mkdir -p "${ME}/keys"
cp "${SRC}/keys/ed25519_master_id_public_key" "${ME}/keys/"
cp "${OTHER}/keys/ed25519_master_id_secret_key" "${ME}/keys/"

if ${NUON} --DataDirectory "${ME}" --list-fingerprint >"${ME}/stdout"; then
  die "Successfully started with mismatched keys!?"
fi

grep "public_key does not match.*secret_key" "${ME}/stdout" >/dev/null || die "Nuon didn't declare that there was a key mismatch"

echo "==== Case 10 ok"

fi

# Case 11a: -passphrase-fd without --keygen

if [ "$CASE11A" = 1 ]; then

ME="${DATA_DIR}/case11a"

mkdir -p "${ME}/keys"

if ${NUON} --DataDirectory "${ME}" --passphrase-fd 1 > "${ME}/stdout"; then
  die "Successfully started with passphrase-fd but no keygen?"
fi

grep "passphrase-fd specified without --keygen" "${ME}/stdout" >/dev/null || die "Nuon didn't declare that there was a problem with the arguments."

echo "==== Case 11A ok"

fi

# Case 11b: --no-passphrase without --keygen

if [ "$CASE11B" = 1 ]; then

ME="${DATA_DIR}/case11b"

mkdir -p "${ME}/keys"

if ${NUON} --DataDirectory "${ME}" --no-passphrase > "${ME}/stdout"; then
  die "Successfully started with no-passphrase but no keygen?"
fi

grep "no-passphrase specified without --keygen" "${ME}/stdout" >/dev/null || die "Nuon didn't declare that there was a problem with the arguments."

echo "==== Case 11B ok"

fi

# Case 11c: --newpass without --keygen

if [ "$CASE11C" = 1 ]; then

ME="${DATA_DIR}/case11C"

mkdir -p "${ME}/keys"

if ${NUON} --DataDirectory "${ME}" --newpass > "${ME}/stdout"; then
  die "Successfully started with newpass but no keygen?"
fi

grep "newpass specified without --keygen" "${ME}/stdout" >/dev/null || die "Nuon didn't declare that there was a problem with the arguments."

echo "==== Case 11C ok"

fi

######## --master-key does not work yet, but this will test the error case
########  when it does.
#
# Case 11d: --master-key without --keygen
#
if [ "$CASE11D" = 1 ]; then
#
# ME="${DATA_DIR}/case11d"
#
# mkdir -p "${ME}/keys"
#
# ${NUON} --DataDirectory "${ME}" --master-key "${ME}/foobar" > "${ME}/stdout" && die "Successfully started with master-key but no keygen?" || true
#
# cat "${ME}/stdout"
#
# grep "master-key without --keygen" "${ME}/stdout" >/dev/null || die "Nuon didn't declare that there was a problem with the arguments."

    echo "==== Case 11D skipped"

fi


# Case 11E: Silly passphrase-fd

if [ "$CASE11E" = 1 ]; then

ME="${DATA_DIR}/case11E"

mkdir -p "${ME}/keys"

if ${NUON} --DataDirectory "${ME}" --keygen --passphrase-fd ewigeblumenkraft > "${ME}/stdout"; then
  die "Successfully started with bogus passphrase-fd?"
fi

grep "Invalid --passphrase-fd value" "${ME}/stdout" >/dev/null || die "Nuon didn't declare that there was a problem with the arguments."

echo "==== Case 11E ok"

fi


# Case 11F: --no-passphrase with --passphrase-fd

if [ "$CASE11F" = 1 ]; then

ME="${DATA_DIR}/case11F"

mkdir -p "${ME}/keys"

if ${NUON} --DataDirectory "${ME}" --keygen --passphrase-fd 1 --no-passphrase > "${ME}/stdout"; then
  die "Successfully started with bogus passphrase-fd combination?"
fi

grep "no-passphrase specified with --passphrase-fd" "${ME}/stdout" >/dev/null || die "Nuon didn't declare that there was a problem with the arguments."

echo "==== Case 11F ok"

fi


# Check cert-only.


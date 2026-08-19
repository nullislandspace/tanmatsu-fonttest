#!/usr/bin/env bash
#
# Run a badgelink command, retrying on transient failures.
#
# A fresh badgelink connection intermittently fails with "Invalid sync" when the
# TCP proxy still holds bytes from a previous session: the first attempt reads
# the residue, desyncs, and aborts, while the next attempt succeeds. Retrying
# here keeps an unattended build/install/run/test cycle from dying on it.
#
# Usage: badgelink_retry.sh <connection args...> -- <badgelink args...>
# The connection args are passed through verbatim (--tcp host:port or --port dev).

set -u

RETRIES="${BADGELINK_RETRIES:-4}"
RETRY_DELAY="${BADGELINK_RETRY_DELAY:-1}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Same lookup order the Makefile uses: the checkout made by `make badgelink`
# first, then a component copy.
for candidate in \
	"$repo_root/badgelink/tools/badgelink.sh" \
	"$repo_root/components/badgeteam__badgelink/tools/badgelink.sh" \
	"$repo_root/managed_components/badgeteam__badgelink/tools/badgelink.sh"; do
	if [ -x "$candidate" ]; then
		BL_SH="$candidate"
		break
	fi
done

if [ -z "${BL_SH:-}" ]; then
	echo "badgelink.sh not found, run 'make badgelink' first" >&2
	exit 1
fi

for ((attempt = 1; attempt <= RETRIES; attempt++)); do
	if "$BL_SH" "$@"; then
		exit 0
	fi
	if [ "$attempt" -lt "$RETRIES" ]; then
		echo "badgelink attempt $attempt/$RETRIES failed, retrying..." >&2
		sleep "$RETRY_DELAY"
	fi
done

echo "badgelink failed after $RETRIES attempts: $*" >&2
exit 1

#!/bin/sh
# Run the server test suites against containerized PostgreSQL and MariaDB.
#   scripts/server-tests.sh          # up → test → down
#   scripts/server-tests.sh --keep   # leave the containers running (debugging)
set -eu

repo=$(dirname "$0")/..
compose="docker compose -f $repo/tests/containers/docker-compose.yml"

$compose up -d --wait
status=0
ctest --test-dir "$repo/build" -L server --output-on-failure || status=$?
[ "${1:-}" = "--keep" ] || $compose down -v
exit $status

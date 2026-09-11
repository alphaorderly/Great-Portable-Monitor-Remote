#!/bin/zsh
set -euo pipefail
task_root="${0:A:h:h:h}"
cd "$task_root"
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/remote-native-tests.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT
for test_name in hid_client mac_hid input_codec reconnect keymap; do
  cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -Itests/native/stubs "tests/native/${test_name}_test.c" \
    main/input_codec.c main/keymap.c tests/native/keymap_storage_stub.c \
    -o "$test_dir/$test_name"
  if [[ "$test_name" == hid_client ]]; then
    "$test_dir/$test_name" ../shared/fixtures/remote_report_map.bin
  else
    "$test_dir/$test_name"
  fi
done
"$test_dir/keymap" --defaults > "$test_dir/defaults.hex"
cmp "$test_dir/defaults.hex" ../shared/fixtures/keymap-default.hex
python3 tests/native/test_nimble_identity.py

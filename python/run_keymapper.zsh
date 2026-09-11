#!/bin/zsh
set -e
task_root="${0:A:h}"
if [[ ! -x "$task_root/../.venv-gui/bin/python" ]]; then
  print "키 매핑 실행 환경이 없습니다. python/README.md의 설치 방법을 확인하세요."
  exit 1
fi
exec "$task_root/../.venv-gui/bin/python" "$task_root/run_keymapper.py" "$@"

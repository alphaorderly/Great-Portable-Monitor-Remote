"""Interactive ESP32 flashing for the macOS and Windows shell entry points."""
from pathlib import Path
import subprocess
import sys


def discover_ports():
    # pyserial is already part of the activated ESP-IDF environment.
    from serial.tools import list_ports

    return sorted(list_ports.comports())


def ask(prompt):
    answer = input(prompt).strip().lower()
    if answer == "q":
        raise EOFError
    return answer


def choose_port():
    while True:
        ports = discover_ports()
        print("\n=== 시리얼 포트 선택 ===")
        for index, port in enumerate(ports, 1):
            details = [port.description or "설명 없음"]
            if port.vid is not None and port.pid is not None:
                details.append(f"USB {port.vid:04X}:{port.pid:04X}")
            if port.serial_number:
                details.append(f"S/N {port.serial_number}")
            print(f"  {index}. {port.device} | {' | '.join(details)}")
        if not ports:
            print("포트를 찾지 못했습니다. ESP32를 데이터 통신용 USB 케이블로 연결하세요.")
        print("  r. 다시 탐지    q. 종료")
        while True:
            answer = ask("포트 번호 또는 r/q: ")
            if answer == "r":
                break
            if answer.isdecimal() and 1 <= int(answer) <= len(ports):
                return ports[int(answer) - 1].device
            print("목록의 번호, r 또는 q를 입력하세요.")


def choose(title, options, default):
    print(f"\n=== {title} ===")
    for index, (_, label) in enumerate(options, 1):
        print(f"  {index}. {label}")
    while True:
        answer = ask(f"선택 [Enter={default}, q=종료]: ") or str(default)
        if answer.isdecimal() and 1 <= int(answer) <= len(options):
            return options[int(answer) - 1][0]
        print("목록의 번호 또는 q를 입력하세요.")


def main():
    try:
        print("=== ESP32 펌웨어 플래시 ===")
        print("원래 ESP32 칩용 / ESP-IDF 5.5.5 / 취소: q 또는 Ctrl+C")
        port = choose_port()
        action = choose("플래시 방식", [
            ("flash", "전체 설치: 부트로더 + 파티션 테이블 + 앱"),
            ("app-flash", "앱만 갱신: 기존과 동일한 파티션 레이아웃일 때 사용"),
        ], default=1)
        monitor = choose("완료 후 시리얼 모니터", [
            (False, "열지 않음"),
            (True, "열기 (모니터 종료: Ctrl+])"),
        ], default=1)
        # A port may disappear while the user is navigating the menus.
        if port not in {item.device for item in discover_ports()}:
            print(f"\n선택한 포트 {port}의 연결이 끊어졌습니다. 다시 실행하세요.", file=sys.stderr)
            return 1
        print(f"\n{port}: {'전체 설치' if action == 'flash' else '앱만 갱신'} 빌드 및 플래시 시작", flush=True)
        command = [sys.executable, str(Path(__file__).with_name("build.py")),
                   "-B", "build", "-p", port, action]
        if monitor:
            command.append("monitor")
        return subprocess.call(command)
    except EOFError:
        print("\n취소했습니다.")
        return 0
    except KeyboardInterrupt:
        print("\n중단했습니다.")
        return 130
    except ImportError:
        print("ESP-IDF의 pyserial을 찾지 못했습니다. ESP-IDF 5.5.5 환경을 활성화하세요.", file=sys.stderr)
        return 1
    except OSError as error:
        print(f"시리얼 탐지 또는 실행에 실패했습니다: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

"""Receive ESP32-S3 serial logs only: no build, flash, reset command or UART writes."""
import argparse
import codecs
from contextlib import ExitStack
from pathlib import Path
import sys


def choose_port(list_ports):
    while True:
        ports = sorted(list_ports.comports())
        print("\n=== ESP32-S3 모니터링 포트 ===")
        for index, port in enumerate(ports, 1):
            print(f"  {index}. {port.device} | {port.description or '설명 없음'}")
        if not ports:
            print("포트가 없습니다. ESP32-S3를 데이터 통신용 USB 케이블로 연결하세요.")
        print("  r. 다시 탐지    q. 종료")
        while True:
            answer = input("포트 번호 또는 r/q: ").strip().lower()
            if answer == "q":
                raise EOFError
            if answer == "r":
                break
            if answer.isdecimal() and 1 <= int(answer) <= len(ports):
                return ports[int(answer) - 1].device
            print("목록의 번호, r 또는 q를 입력하세요.")


def receive(serial_module, port, baud, log_path=None):
    # Configure inactive modem lines BEFORE open. Never use Serial(port=...) or
    # miniterm's reset / keyboard-transmit commands. Some USB drivers can still
    # pulse these lines on open; software cannot guarantee no hardware reset.
    with ExitStack() as resources:
        log = resources.enter_context(Path(log_path).open("ab")) if log_path else None
        device = serial_module.Serial(port=None, baudrate=baud, timeout=0.2,
                                      xonxoff=False, rtscts=False, dsrdtr=False)
        resources.callback(device.close)
        device.dtr = False
        device.rts = False
        device.port = port
        device.open()
        decoder = codecs.getincrementaldecoder("utf-8")("replace")
        print(f"\n{port} · {baud} baud · 수신 전용 / 종료: Ctrl+C", flush=True)
        if log_path:
            print(f"로그 추가 저장: {Path(log_path).resolve()}", flush=True)
        try:
            while True:
                chunk = device.read(4096)
                if not chunk:
                    continue
                if log:
                    log.write(chunk)
                    log.flush()
                sys.stdout.write(decoder.decode(chunk))
                sys.stdout.flush()
        finally:
            sys.stdout.write(decoder.decode(b"", final=True))
            sys.stdout.flush()


def positive_baud(value):
    baud = int(value)
    if baud <= 0:
        raise argparse.ArgumentTypeError("baud must be positive")
    return baud


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="COM3 or /dev/cu.usbserial-...; omitted: interactive selection")
    parser.add_argument("--baud", type=positive_baud, default=115200)
    parser.add_argument("--log", type=Path, help="append received raw bytes to this file (optional)")
    args = parser.parse_args(argv)
    try:
        import serial
        from serial.tools import list_ports
        port = args.port or choose_port(list_ports)
        receive(serial, port, args.baud, args.log)
    except ImportError:
        print("pyserial이 필요합니다. ESP-IDF Python을 사용하거나 해당 Python으로 "
              "'python -m pip install pyserial==3.5'를 실행하세요.", file=sys.stderr)
        return 1
    except EOFError:
        print("\n취소했습니다.")
        return 0
    except KeyboardInterrupt:
        print("\n모니터링을 종료했습니다.")
        return 0
    except (OSError, ValueError) as exc:
        print(f"\n모니터링 실패: {exc}\nUSB 연결·포트 이름을 확인하고 다른 시리얼 모니터를 닫아 주세요.",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

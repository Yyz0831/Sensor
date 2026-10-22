import contextlib
import importlib.util
import io
from pathlib import Path
import unittest

PATH = Path(__file__).resolve().parents[1] / "tools/weight_control.py"
SPEC = importlib.util.spec_from_file_location("weight_control", PATH)
control = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(control)


class FakeSerial:
    def __init__(self, fail=False, preset_firmware=False):
        self.pending = bytearray()
        self.writes = []
        self.fail = fail
        self.preset_firmware = preset_firmware

    @property
    def in_waiting(self):
        return min(7, len(self.pending))

    def read(self, count):
        result = bytes(self.pending[:count])
        del self.pending[:count]
        return result

    def reset_input_buffer(self):
        self.pending.clear()

    def flush(self):
        pass

    def write(self, data):
        self.writes.append(data)
        if data == b"\nhelp\n":
            message = ("# 协议=weight_i2c_v1\r\n" if self.preset_firmware else
                       "# 校准仅保存在RAM，复位后重新tare和cal；量程尚未确认。\r\n")
            self.pending.extend(message.encode())
        else:
            message = "# 采集因I2C失败中止；原有零点和校准保留。" if self.fail else (
                "# 去皮完成 | 零点ADC=123.000" if data == b"tare\n" else
                "# 校准完成 | 参考质量=50 g | 系数=100 ADC/g")
            self.pending.extend(("时间=1000 ms | 重量=未校准\r\n" + message + "\r\n").encode())
        return len(data)


class WeightControlTests(unittest.TestCase):
    def test_tare_and_50g_command_with_fragmented_utf8(self):
        for command, expected in [("tare", "去皮完成"), ("cal 50", "参考质量=50 g")]:
            with self.subTest(command=command), contextlib.redirect_stdout(io.StringIO()):
                connection = FakeSerial()
                result = control.run_command(connection, command)
                self.assertIn(expected, result)
                self.assertEqual(connection.writes, [b"\nhelp\n", (command + "\n").encode()])

    def test_board_failure_is_not_reported_as_success(self):
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "I2C失败"):
                control.run_command(FakeSerial(fail=True), "cal 50")

    def test_preset_firmware_handshake(self):
        with contextlib.redirect_stdout(io.StringIO()):
            connection = FakeSerial(preset_firmware=True)
            self.assertIn("去皮完成", control.run_command(connection, "tare"))

    def test_invalid_reference_mass_is_rejected_before_serial_open(self):
        for mass in ["0", "-1", "nan", "inf"]:
            with self.subTest(mass=mass), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as result:
                    control.main(["--port", "fake", "cal", mass])
                self.assertEqual(result.exception.code, 2)

    def test_missing_reply_times_out(self):
        with self.assertRaisesRegex(RuntimeError, "未收到预期回复"):
            control.wait_for(FakeSerial(), "校准完成", 0)


if __name__ == "__main__":
    unittest.main()

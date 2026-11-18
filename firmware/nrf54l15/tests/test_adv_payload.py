import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import adv_payload as adv


class AdvertisingTests(unittest.TestCase):
    def test_ibeacon_wire_layout(self):
        raw = adv.ibeacon("00112233-4455-6677-8899-aabbccddeeff", 0x1234, 0x5678, -59)
        self.assertEqual(raw.hex(), "0201061aff4c00021500112233445566778899aabbccddeeff12345678c5")
        self.assertEqual(len(raw), 30)

    def test_service_uuid_byte_order(self):
        self.assertEqual(adv.service_data("180f", b"\x64").hex(), "02010604160f1864")
        self.assertEqual(adv.service_data("12345678", b"\x01").hex(), "02010606207856341201")
        self.assertEqual(adv.service_data("00112233-4455-6677-8899-aabbccddeeff", b"\x55")[5:],
                         bytes.fromhex("ffeeddccbbaa9988776655443322110055"))

    def test_configuration_limits(self):
        payload = adv.configuration(9, True, "connectable", 250, b"\x02\x01\x06", adv.name_response("GW32"))
        readback = adv.describe_configuration(payload)
        self.assertEqual((readback["revision"], readback["mode"], readback["intervalMs"]), (9, 2, 250))
        with self.assertRaises(ValueError):
            adv.configuration(1, True, "nonconnectable", 250, b"", adv.name_response("GW32"))
        with self.assertRaises(ValueError):
            adv.name_response("网" * 10)
        with self.assertRaises(ValueError):
            adv.service_data("00112233-4455-6677-8899-aabbccddeeff", bytes(11))
        with self.assertRaises(ValueError):
            adv.configuration(1, True, "connectable", 99, b"")
        with self.assertRaises(ValueError):
            adv.validate_ad(b"\x03\x09A")


if __name__ == "__main__":
    unittest.main()

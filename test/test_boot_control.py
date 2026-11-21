"""Preboot host API and firmware input validation contracts."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).parents[1]
SPEC = importlib.util.spec_from_file_location("mac_control", ROOT / "tools/mac_control.py")
mc = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(mc)


class BootControlTests(unittest.TestCase):
    def setUp(self):
        self.client = mc.MacControl.__new__(mc.MacControl)
        self.client.request = Mock()

    def test_card_requires_separate_identity_and_confirmation(self):
        token = "01" * 16
        for action, card, code in (("erase", None, None), ("erase", token, None),
                                    ("erase", token, "wrong"), ("arm", "bad", None)):
            with self.assertRaises(mc.ControlError):
                self.client.boot_card(action, card, code)
        self.client.request.assert_not_called()
        self.client.request.return_value = f"@B2 OK BOOT CARD ARMED {token} aabbccdd"
        self.assertEqual(self.client.boot_card("arm", token)["challenge"], "aabbccdd")
        self.client.request.return_value = "@B2 OK BOOT CARD ERASE"
        self.assertEqual(self.client.boot_card("erase", token, "aabbccdd"), {"ok": True})
        self.client.request.assert_called_with("BOOT CARD ERASE " + token + " aabbccdd", "OK BOOT CARD", timeout=15.0)
        self.client.request.return_value = (f"@B2 OK BOOT CARD STATUS state=inspect mounted=0 format=1 bytes=4294967296 volume_bytes=1073741824 "
            f"token={token} clock_khz=20000 bus_width=4 cluster_bytes=32768 partition_lba=2048 "
            "partition_type=12 layout=6578464154 identity=5344 driver=535049 message=-")
        status = self.client.boot_card("status")
        self.assertEqual((status["bytes"], status["layout"], status["identity"]), (1 << 32, "exFAT", "SD"))

    def test_starter_status_and_actions(self):
        self.client.request.return_value = '@B2 OK BOOT STARTER working 512 2048 524f4d'
        self.assertEqual(self.client.boot_starter('status'), {
            'state': 'working', 'completed': 512, 'total': 2048, 'message': 'ROM'})
        self.client.request.return_value = '@B2 OK BOOT STARTER DOWNLOAD'
        self.assertEqual(self.client.boot_starter('7'), {'ok': True})
        self.client.request.assert_called_with('BOOT STARTER 7', 'OK BOOT STARTER', timeout=15.0)
        self.client.request.return_value = '@B2 OK BOOT STARTER working X 2048 -'
        with self.assertRaises(mc.ControlError):
            self.client.boot_starter('status')

    def test_strings_round_trip_spaces_equals_and_unicode(self):
        value = '/Mac OS 8 = café.hfv'
        self.client.boot_set('disk', value)
        self.client.request.assert_called_once_with(
            'BOOT SET disk ' + value.encode().hex(), 'OK BOOT SET disk', timeout=15.0)
        self.client.request.return_value = '@B2 OK BOOT VALUE disk ' + value.encode().hex()
        self.assertEqual(self.client.boot_get('disk'), value)
        self.client.boot_set('cdrom', '')
        self.assertEqual(self.client.request.call_args.args[0], 'BOOT SET cdrom -')

    def test_settings_validate_before_writing(self):
        for key, value in [('ramsize', '8junk'), ('ramsize', 32), ('ramsize', 12), ('rotation', 90),
                           ('audio', 'yes'), ('audio', 1), ('wifi_ssid', 'é' * 17),
                           ('wifi_pass', 'x' * 64), ('disk', '/x\ny'),
                           ('wifi_pass', '\x00'), ('wifi_password_set', True)]:
            with self.subTest(key=key, value=value):
                with self.assertRaises(mc.ControlError): self.client.boot_set(key, value)
        self.client.request.assert_not_called()
        self.client.boot_set('audio', False)
        self.assertEqual(self.client.request.call_args.args[0], 'BOOT SET audio 66616c7365')
        self.client.boot_set('ramsize', 24)
        self.assertEqual(self.client.request.call_args.args[0], 'BOOT SET ramsize 3234')

    def test_get_typed_values_and_password_redaction(self):
        self.client.request.return_value = '@B2 OK BOOT VALUE audio 74727565'
        self.assertIs(self.client.boot_get('audio'), True)
        self.client.request.return_value = '@B2 OK BOOT VALUE ramsize 3136'
        self.assertEqual(self.client.boot_get('ramsize'), 16)
        with self.assertRaises(mc.ControlError): self.client.boot_get('wifi_pass')
        self.assertNotIn('wifi_pass', mc.BOOT_READ_SETTINGS)
        self.assertIn('wifi_password_set', mc.BOOT_READ_SETTINGS)

    def test_status_and_paged_media(self):
        self.client.request.return_value = '@B2 OK BOOT STATUS preboot settings 0'
        self.assertEqual(self.client.boot_status(), {'phase': 'preboot', 'screen': 'settings', 'restart_pending': False})
        self.client.request.side_effect = ['@B2 OK BOOT ITEM 2 2f412e64736b', '@B2 OK BOOT ITEM 2 2f422e64736b']
        self.assertEqual(self.client.boot_list('disk'), ['/A.dsk', '/B.dsk'])
        self.client.request.side_effect = None
        self.client.request.return_value = '@B2 OK BOOT ITEM 0 -'
        self.assertEqual(self.client.boot_list('cdrom'), [])

    def test_preboot_touch_hit_and_panel_verify(self):
        self.client.request.return_value = '@B2 OK BOOT HIT start 514 309 104 24'
        self.assertEqual(self.client.boot_hit('start'),
                         {'x': 514, 'y': 309, 'w': 104, 'h': 24, 'center': [566, 321]})
        self.client.request.assert_called_with('BOOT HIT start', 'OK BOOT HIT start', timeout=15.0)
        with self.assertRaises(mc.ControlError): self.client.boot_hit('two words')
        self.client.request.side_effect = ['@B2 OK BOOT HIT start 514 309 104 24',
                                           '@B2 OK TOUCH TAP 566 321']
        self.client.tap_control('start')
        self.client.request.assert_called_with('TOUCH TAP 566 321', 'OK TOUCH TAP', timeout=10.0)
        self.client.request.side_effect = None
        self.client.touch('down', 1, 2)
        self.client.request.assert_called_with('TOUCH DOWN 1 2', 'OK TOUCH DOWN', timeout=10.0)
        self.client.touch('tap', 3, 4, hold_ms=250)
        self.client.request.assert_called_with('TOUCH TAP 3 4 250', 'OK TOUCH TAP', timeout=10.0)
        self.client.touch('up')
        self.client.request.assert_called_with('TOUCH UP', 'OK TOUCH UP', timeout=5.0)
        with self.assertRaises(mc.ControlError): self.client.touch('tap')
        with self.assertRaises(mc.ControlError): self.client.touch('swipe', 1, 2)
        self.client.request.return_value = '@B2 OK PANEL checked=921600 mismatched=3 first=10,20'
        self.assertEqual(self.client.panel_verify(),
                         {'checked': 921600, 'mismatched': 3, 'first': [10, 20]})
        self.client.request.return_value = '@B2 OK PANEL checked=9'
        with self.assertRaises(mc.ControlError): self.client.panel_verify()
        self.client.request.side_effect = ['@B2 OK PANEL checked=921600 mismatched=0 first=-',
                                           '@B2 OK PANEL checked=921600 mismatched=0 first=-1,-1']
        self.assertEqual(self.client.panel_verify(),
                         {'checked': 921600, 'mismatched': 0, 'first': [-1, -1]})
        self.client.request.side_effect = None
        tools = {tool['name'] for tool in mc.MCP_TOOLS}
        self.assertTrue({'mac_touch', 'mac_boot_hit', 'mac_panel_verify', 'mac_screenshot_panel'} <= tools)
        args = mc.build_parser().parse_args(['touch', 'tap', '5', '6', '--hold', '90'])
        self.assertEqual((args.action, args.x, args.y, args.hold), ('tap', 5, 6, 90))
        self.assertTrue(mc.build_parser().parse_args(['screenshot', 'x.png', '--panel']).panel)

    def test_malformed_responses_fail(self):
        for line in ('@B2 OK BOOT ITEM nope -', '@B2 OK BOOT ITEM 33 -', '@B2 OK BOOT ITEM 1 ff'):
            self.client.request.return_value = line
            with self.assertRaises(mc.ControlError): self.client.boot_list('disk')
        self.client.request.return_value = '@B2 OK BOOT VALUE audio 796573'
        with self.assertRaises(mc.ControlError): self.client.boot_get('audio')

    def test_enter_rehandshakes_after_session_change_without_repeating_enter(self):
        self.client.boot_status = Mock(side_effect=[
            {'phase': 'emulator', 'screen': 'none', 'restart_pending': True},
            mc.ControlError('ERR session_changed'),
            {'phase': 'preboot', 'screen': 'settings', 'restart_pending': False}])
        self.client.connect = Mock()
        with patch.object(mc.time, 'sleep'):
            self.assertEqual(self.client.boot_enter()['phase'], 'preboot')
        self.client.request.assert_called_once_with('BOOT ENTER', 'OK BOOT ENTER', timeout=15.0)
        self.client.connect.assert_called_once()

    def test_password_timeout_does_not_expose_the_encoded_secret(self):
        control = mc.MacControl.__new__(mc.MacControl)
        control.protocol_version = 4
        control.session_id = '12345678'
        control._write = Mock()
        control._protocol_lines = Mock(return_value=iter(()))
        secret = 'secret password'.encode().hex()
        with self.assertRaises(mc.ControlError) as result:
            control.request('BOOT SET wifi_pass ' + secret, 'OK BOOT SET wifi_pass')
        self.assertNotIn(secret, str(result.exception))
        self.assertIn('<redacted>', str(result.exception))

    def test_transient_reboot_handshake_failure_keeps_waiting(self):
        self.client.boot_status = Mock(side_effect=[mc.ControlError('rebooting'),
            mc.ControlError('session_changed'),
            {'phase': 'preboot', 'screen': 'settings', 'restart_pending': False}])
        self.client.connect = Mock(side_effect=[mc.ControlError('still rebooting'), None])
        with patch.object(mc.time, 'sleep'):
            self.assertEqual(self.client.boot_enter()['phase'], 'preboot')
        self.assertEqual(self.client.connect.call_count, 2)

    def test_cli_and_mcp_have_equivalent_controls(self):
        args = mc.build_parser().parse_args(['boot-set', 'ramsize', '16'])
        self.assertEqual((args.key, args.value), ('ramsize', '16'))
        tools = {tool['name'] for tool in mc.MCP_TOOLS}
        self.assertTrue({'mac_boot_status', 'mac_boot_get', 'mac_boot_set', 'mac_boot_list', 'mac_boot_action'} <= tools)
        self.client.boot_get = Mock(return_value={'audio': True})
        result = mc.call_mcp_tool(self.client, 'mac_boot_get', {})
        self.assertEqual(json.loads(result[0]['text']), {'audio': True})

    def test_firmware_value_parser_rejects_injection_truncation_and_invalid_choices(self):
        code = r'''
#include "boot_control_values.h"
#include "boot_media.h"
#include <cassert>
#include <cstdio>
#include <cstring>
int main() {
    char disk[64] = "";
    assert(BootSelectDisk({}, disk, sizeof(disk)) == -1 && !disk[0]);
    const std::vector<std::string> files = {"/Current.dsk", "/Other.dsk"};
    assert(BootSelectDisk(files, disk, sizeof(disk)) == 0 && !strcmp(disk, "/Current.dsk"));
    strcpy(disk, "/Missing.dsk");
    assert(BootSelectDisk(files, disk, sizeof(disk)) == -1 && !strcmp(disk, "/Missing.dsk"));
    strcpy(disk, "/Other.dsk");
    assert(BootSelectDisk(files, disk, sizeof(disk)) == 1);
    const auto usable = [](const char *path, size_t min, size_t max) {
        size_t n = !strcmp(path, "/Q650.ROM") ? 1024*1024 : !strcmp(path, "/Current.dsk") ? 512 : 0;
        return n >= min && n <= max;
    };
    assert(BootHasMedia(false, "/Current.dsk", "", usable));
    assert(!BootHasMedia(false, "/Missing.dsk", "", usable));
    assert(!BootHasMedia(true, "/Current.dsk", "", usable));
    assert(BootHasMedia(true, "/Missing.dsk", "/Current.dsk", usable));
    char out[4]; bool b = false; int n = 0;
    assert(BootControlDecode("612062", out, sizeof(out)) && !strcmp(out, "a b"));
    assert(BootControlDecode("-", out, sizeof(out)) && !out[0]);
    for (auto s : {"", "1", "zz", "0001", "0a", "0d", "7f", "61626364"})
        assert(!BootControlDecode(s, out, sizeof(out)));
    assert(BootControlBool("true", &b) && b);
    assert(!BootControlBool("True", &b));
    for (int mb : {8, 16, 20, 24}) {
        char text[4]; snprintf(text, sizeof(text), "%d", mb);
        assert(BootControlChoice(text, true, &n) && n == mb);
    }
    assert(BootControlChoice("180", false, &n) && n == 180);
    assert(BootControlChoice("0", false, &n) && n == 0);
    for (auto s : {"", "8x", "08", "-8", " 8", "32", "4", "12", "0", "99999999999"})
        assert(!BootControlChoice(s, true, &n));
    assert(!BootControlChoice("24", false, &n));
    // Sizes saved by older firmware round up; garbage gets the default.
    assert(BootControlSavedRam(4) == 8 && BootControlSavedRam(8) == 8);
    assert(BootControlSavedRam(12) == 16 && BootControlSavedRam(16) == 16);
    assert(BootControlSavedRam(24) == 24 && BootControlSavedRam(64) == 24);
    assert(BootControlSavedRam(0) == kBootRamDefaultMB && BootControlSavedRam(-4) == kBootRamDefaultMB);
}
'''
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / 'values.cpp'
            source.write_text('#include <initializer_list>\n' + code)
            binary = Path(temp) / 'values'
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'src/basilisk/include'), '-I', str(ROOT / 'src/basilisk'),
                            str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__': unittest.main()

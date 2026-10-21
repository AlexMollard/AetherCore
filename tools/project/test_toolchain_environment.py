"""Rust home recovery is scoped, read-only and preserves process overrides."""

from contextlib import contextmanager
import os
import unittest
from unittest import mock

import toolchain_environment as environment


class Registry:
    HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE = 'user', 'machine'
    REG_SZ, REG_EXPAND_SZ = 1, 2

    def __init__(self, values):
        self.values = values

    @contextmanager
    def OpenKey(self, hive, key):
        yield hive

    def QueryValueEx(self, hive, name):
        if (hive, name) not in self.values:
            raise FileNotFoundError(name)
        return self.values[hive, name]

    def ExpandEnvironmentStrings(self, value):
        return value.replace('%USERPROFILE%', r'C:\Users\Rust user')


class ToolchainEnvironmentTests(unittest.TestCase):
    def recover(self, inherited, values):
        with mock.patch.dict(os.environ, inherited, clear=True), mock.patch.object(environment, 'winreg', Registry(values)):
            result = environment.child_environment()
            self.assertEqual(dict(os.environ), inherited)
            return result

    def test_user_homes_override_machine_defaults_without_importing_other_variables(self):
        actual = self.recover({'PATH': 'inherited', 'AETHERCORE_CARGO': 'explicit-cargo'}, {
            ('user', 'RUSTUP_HOME'): ('D:\\Rust cache & \u4e16\u754c', 1),
            ('machine', 'RUSTUP_HOME'): ('machine-rust', 1),
            ('machine', 'CARGO_HOME'): ('machine-cargo', 1),
            ('user', 'PATH'): ('must-not-replace-path', 1)})
        self.assertEqual(actual, dict(PATH='inherited', AETHERCORE_CARGO='explicit-cargo',
            RUSTUP_HOME='D:\\Rust cache & \u4e16\u754c', CARGO_HOME='machine-cargo'))

    def test_explicit_process_values_including_empty_are_preserved(self):
        self.assertEqual(self.recover({'RUSTUP_HOME': '', 'CARGO_HOME': 'explicit', 'RUSTUP_TOOLCHAIN': 'pinned'}, {
            ('user', 'RUSTUP_HOME'): ('persisted', 1), ('user', 'CARGO_HOME'): ('persisted', 1)}),
            dict(RUSTUP_HOME='', CARGO_HOME='explicit', RUSTUP_TOOLCHAIN='pinned'))

    def test_expandable_values_use_windows_expansion(self):
        self.assertEqual(self.recover({}, {('user', 'CARGO_HOME'): (r'%USERPROFILE%\cargo & cache', 2)}),
            dict(CARGO_HOME=r'C:\Users\Rust user\cargo & cache'))

    def test_unavailable_or_invalid_settings_leave_normal_defaults_in_control(self):
        self.assertEqual(self.recover({'PATH': 'original'}, {('user', 'RUSTUP_HOME'): (b'binary', 3),
            ('machine', 'CARGO_HOME'): ('', 1)}), {'PATH': 'original'})
        with mock.patch.dict(os.environ, {'PATH': 'original'}, clear=True), mock.patch.object(environment, 'winreg', None):
            self.assertEqual(environment.child_environment(), {'PATH': 'original'})

    def test_inaccessible_registry_does_not_prevent_csharp_or_default_rust_launches(self):
        registry = Registry({})
        with mock.patch.object(registry, 'OpenKey', side_effect=PermissionError), \
                mock.patch.object(environment, 'winreg', registry), mock.patch.dict(os.environ, {'PATH': 'original'}, clear=True):
            self.assertEqual(environment.child_environment(), {'PATH': 'original'})


if __name__ == '__main__':
    unittest.main()

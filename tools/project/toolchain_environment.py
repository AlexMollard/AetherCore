"""Recover missing Rust installation homes from Windows' persisted environment."""

import os

try:
    import winreg
except ImportError:
    winreg = None


def _persisted_home(name):
    if winreg is None:
        return None
    for hive, key in ((winreg.HKEY_CURRENT_USER, 'Environment'),
                      (winreg.HKEY_LOCAL_MACHINE, r'SYSTEM\CurrentControlSet\Control\Session Manager\Environment')):
        try:
            with winreg.OpenKey(hive, key) as handle:
                value, kind = winreg.QueryValueEx(handle, name)
            if kind not in (winreg.REG_SZ, winreg.REG_EXPAND_SZ) or not isinstance(value, str) or not value:
                continue
            return winreg.ExpandEnvironmentStrings(value) if kind == winreg.REG_EXPAND_SZ else value
        except OSError:
            continue
    return None


def child_environment():
    """Keep explicit process overrides; fill only missing homes, never global PATH."""
    environment = dict(os.environ)
    for name in ('RUSTUP_HOME', 'CARGO_HOME'):
        if name not in environment:
            value = _persisted_home(name)
            if value is not None:
                environment[name] = value
    return environment

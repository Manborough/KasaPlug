#!/usr/bin/env python3
"""Host regression tests against KasaPlug's own sources.

KasaPlug.cpp cannot be compiled whole on a PC because it pulls in ESP32 WiFi
types, so the methods under test are lifted out by text and compiled into test
programs that supply their own stand-ins. A complete ESP32 sketch compile
separately verifies the packaged translation unit.

Requires Python 3, a C++17 compiler and ArduinoJson 7 (found through
arduino-cli, or passed with --arduino-json). No plug is contacted.
Run from anywhere: python3 tests/run_tests.py
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

parser = argparse.ArgumentParser()
parser.add_argument(
    '--arduino-json', type=Path,
    help='directory containing ArduinoJson.h (overrides Arduino CLI discovery)')
args = parser.parse_args()

def functions(source, names):
    """Lift named function bodies out of a .cpp as raw text. This is
    brace-counting, not parsing, so it is fragile by nature."""
    result = []
    for name in names:
        # Functions have braces in comments/strings; counting still balances in
        # these sources. Real ESP32 builds separately verify the complete sketch.
        start = source.index(name + '(')
        start = source.rfind('\n', 0, start) + 1
        brace = source.index('{', start)
        depth, end = 1, brace + 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        result.append(source[start:end])
    return '\n'.join(result)

kasa = (ROOT / 'src/KasaPlug.cpp').read_text()
if args.arduino_json:
    json_header = args.arduino_json
else:
    config = json.loads(subprocess.check_output(
        ['arduino-cli', 'config', 'dump', '--format', 'json']))
    libs = Path(config.get('config', config)['directories']['user']) / 'libraries'
    try:
        json_header = next(libs.glob('*/src/ArduinoJson.h')).parent
    except StopIteration as error:
        raise SystemExit(
            'ArduinoJson.h was not found. Install ArduinoJson 7 or pass '
            '--arduino-json /path/to/its/include/directory.') from error
with tempfile.TemporaryDirectory(prefix='kasaplug-tests-') as tmp:
    tmp = Path(tmp)
    (tmp / 'kasa.inc').write_text(functions(kasa, [
        'KasaPlug::watchdogRemainingMs', 'KasaPlug::writeWatchdogRule', 'KasaPlug::verifyWatchdog',
        'KasaPlug::allErrCodesZero', 'KasaPlug::armWatchdog', 'KasaPlug::refreshWatchdog',
        'KasaPlug::clearWatchdog', 'KasaPlug::on', 'KasaPlug::off', 'KasaPlug::readState']))
    (tmp / 'kasa_retry.inc').write_text(functions(kasa, ['KasaPlug::query']))
    (tmp / 'kasa_codec.inc').write_text(functions(kasa, [
        'xorEncrypt', 'xorDecrypt', 'KasaPlug::normalizeMac', 'KasaPlug::macFromSysinfo']))
    (tmp / 'kasa_transport.inc').write_text(functions(kasa, [
        'xorEncrypt', 'xorDecrypt', 'readExactly', 'KasaPlug::sendOnce']))
    for name, needs_json in [('kasa_test', True), ('kasa_codec_test', False),
                             ('kasa_transport_test', False), ('kasa_retry_test', True)]:
        output = tmp / name
        includes = ['-I', str(tmp)] + (['-I', str(json_header)] if needs_json else [])
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', *includes,
                        str(ROOT / 'tests' / f'{name}.cpp'), '-o', str(output)], check=True)
        subprocess.run([str(output)], check=True)

# KasaPlug

An ESP32 client for legacy TP-Link Kasa plugs (HS100, HS103,
HS110, KP105). It speaks the local protocol on TCP 9999 directly: no cloud, no
credentials, no MQTT broker, and no second machine to keep alive.

Newer Tapo-generation hardware uses KLAP/AES and will not work with this.

## Origin

Written to fill a gap: no existing Arduino library speaks the legacy Kasa local
protocol with a verified fail-off countdown, which is what makes it usable for
anything that must not be left switched on.

It implements TP-Link's publicly documented local protocol on TCP 9999 (a 4-byte
big-endian length header followed by an XOR autokey stream with initialisation
vector 171) and the device's own JSON command set (`system.set_relay_state`,
`count_down.*`). Those are properties of the device, not of any one
implementation. This is an independent C++ implementation, not a translation of
the GPL-licensed `python-kasa` package. It was written with AI assistance and is
released under the MIT licence.

## Install

Requires **ArduinoJson 7** and the ESP32 Arduino core. Nothing else.

Clone this repository into your sketchbook's `libraries/` directory, or add it
as a git submodule alongside your sketch and pass `--libraries <parent-dir>` to
`arduino-cli compile`.

The class is deliberately concrete and has no base class, so it imposes no
interface on your code. If your controller talks to switches through an
abstraction of its own, wrap a `KasaPlug` in a small adapter on your side.

## Use

Address the plug by MAC, which never changes:

```cpp
KasaPlug heater("B0:4E:26:C8:48:E0");

heater.discover();               // find its current IP by broadcast
heater.setWatchdogSeconds(300);  // optional; 0 disables
heater.on();
heater.off();

bool isOn;
if (heater.readState(isOn)) { /* ... */ }
```

Or by IP, if you would rather pin it:

```cpp
KasaPlug heater(IPAddress(192, 168, 0, 187));
```

A failed `on()` or `off()` must be treated as an unknown relay state. Stop
requesting heat and retry OFF until acknowledged. A failed `refreshWatchdog()`
may be retried while `watchdogRemainingMs()` has sufficient reserve, as described
below. Never assume that a lost reply means the plug did nothing.

## Discovery

`discover()` broadcasts on UDP 9999 and adopts the address of the plug whose
MAC matches. Separators and case in the MAC are ignored.

Reads, OFF commands and OFF countdown edits retry the known address once after
100 ms. This handles a missed reply even when broadcast discovery is filtered.
If both attempts fail and a MAC is configured, the client discovers for one
second and makes one final attempt. ON, timer creation and arbitrary raw commands
are not replayed after an uncertain acknowledgement; the caller reconciles OFF.

Call `discover()` once in `setup()` to avoid a slow first command. Use
`setRediscoverOnFailure(false)` to turn discovery recovery off. The bounded
same-address retry remains enabled for the safe operations above.

## Watchdog

`setWatchdogSeconds(n)` enables an OFF countdown. `on()` switches on, installs
or edits the named `esp32 watchdog` rule, and reads it back to confirm an active
OFF countdown. A failure triggers a best-effort OFF and returns false.

While heat is still requested, call `refreshWatchdog()` at an interval well
below the timer duration (the main controller uses 60 seconds / 300 seconds).
It makes two requests: edit the previously verified rule by its cached ID, then
read relay status and timer together. It never sends ON, creates a rule, or
deletes the previous cutoff during renewal.

`watchdogRemainingMs()` is a conservative deadline from the last valid readback,
with request latency and a one-second rounding allowance deducted. A transport
failure retains that old deadline; it never assumes the edit extended it.
An invalid timer or relay-OFF readback invalidates the deadline immediately.
The controller retries missed refreshes every ten seconds while more than
30 seconds remain (demo: two-second retries, 15-second reserve). Once the reserve
is reached, it requests OFF and uses the ordinary fault/restart path. Temperature
and sensor cutoffs remain active during retries. Successful renewal restores the
normal once-per-minute schedule. Initial ON still requires four requests and
verified timer setup, with no grace for an unsuccessful start.

`off()` retains the rule; an OFF action against an already-off relay is harmless.
`clearWatchdog()` is an explicit maintenance operation and must not be used to
refresh a timer while heating. A differently named countdown is not overwritten:
remove it deliberately with the heater disconnected before using this controller.

ON and countdown setup are separate network operations. Some Kasa firmware
cancels countdowns on relay transitions, so the timer is armed after ON. This is
not an atomic safety interlock; the pad's independent thermostat is the backstop.
Verify countdown expiry and repeated refresh on the actual plug firmware.

## Example

`examples/FermentChamber` is a tempeh incubator controller: discovery by MAC,
hysteresis, a minimum relay dwell time, an upper abort limit, and fail-off on
sensor error. Supply your own sensor in `readChamberTemperature()`.

## Tests

`tests/` holds host-side tests that exercise the real control and countdown
functions, the codec, TCP framing, fragmented and malformed replies, and
simulated command failures. They lift the function bodies out of `KasaPlug.cpp`
and compile them natively, so the logic under test is the logic that gets
flashed. No plug is contacted.

The harness that drives them lives in the project this library was written for,
at `firmware/tests/run_tests.py` in
<https://github.com/Manborough/AutoIoT-tempeh>; run `python3
firmware/tests/run_tests.py` from that repository's root.

// Tempeh fermentation chamber: bang-bang heat control over a Kasa HS100.
//
// Plug in your own sensor at readChamberTemperature(). Everything else is the
// control loop: hysteresis to stop the relay chattering, a minimum dwell time
// to protect its contacts, and fail-off behaviour when things go wrong.

#include <WiFi.h>

#include "KasaPlug.h"

const char* kSsid = "YOUR_SSID";
const char* kPassword = "YOUR_PASSWORD";

// Addressed by MAC, not IP: the plug is found by broadcast at startup and
// re-found automatically if its address ever changes. No DHCP reservation
// needed. Read the MAC off the plug's label, or from `uv run kasa discover`.
KasaPlug heater("B0:4E:26:C8:48:E0");

// Rhizopus is happy around 30 C and starts to suffer past about 35 C.
const float kTurnOnBelowC = 29.5f;
const float kTurnOffAboveC = 31.0f;

// Refuse to heat at all above this. A runaway reading or a stuck relay should
// end the batch, not cook it.
const float kAbortAboveC = 35.0f;

const uint32_t kLoopIntervalMs = 10000;
const uint32_t kMinDwellMs = 5 * 60 * 1000;  // relay contact life

// Dead-man's switch, in seconds: the plug turns itself off this long after the
// last successful on(). Comfortably longer than kLoopIntervalMs so ordinary
// jitter or a brief WiFi blip does not trip it. Set to 0 to disable — a mat
// with its own thermal cutoff makes this batch insurance, not fire insurance.
const uint32_t kWatchdogSeconds = 300;

bool heaterOn = false;
bool heaterStateKnown = false;
uint32_t lastSwitchMs = 0;

// TODO: replace with a real reading (DS18B20, SHT31, ...). Measure chamber air,
// away from both the mat and the bean cake.
float readChamberTemperature() {
  return NAN;
}

void setHeater(bool wantOn) {
  // Resolve uncertain state to OFF before allowing another heating cycle.
  if (!heaterStateKnown) wantOn = false;
  if (wantOn && heaterOn) {
    if (heater.refreshWatchdog()) return;
    heaterStateKnown = false;
    wantOn = false;
  }
  if (!wantOn && heaterStateKnown && !heaterOn) return;
  // OFF is never held up by contact-protection dwell.
  if (wantOn && millis() - lastSwitchMs < kMinDwellMs) return;
  const bool ok = wantOn ? heater.on() : heater.off();
  if (!ok) {
    heaterStateKnown = false;
    Serial.println("plug command failed - relay state unknown");
    if (wantOn) setHeater(false);
    return;
  }
  heaterOn = wantOn;
  heaterStateKnown = true;
  lastSwitchMs = millis();
  Serial.printf("heater %s\n", heaterOn ? "ON" : "OFF");
}

void setup() {
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(kSsid, kPassword);
  heater.setWatchdogSeconds(kWatchdogSeconds);
  // No startup wait. Commands discover by MAC and retry once per loop while
  // heating stays inhibited until the first OFF acknowledgement.

  // Start from a known state rather than whatever the last run left behind.
  setHeater(false);  // confirmed OFF followed by the normal dwell
}

void loop() {
  const float tempC = readChamberTemperature();

  if (!heaterStateKnown || !isfinite(tempC)) {
    Serial.println("sensor read failed - heating off");
    setHeater(false);
  } else if (tempC >= kAbortAboveC) {
    Serial.printf("%.2f C over abort limit - heating off\n", tempC);
    setHeater(false);
  } else if (tempC < kTurnOnBelowC) {
    setHeater(true);
  } else if (tempC > kTurnOffAboveC) {
    setHeater(false);
  } else {
    setHeater(heaterOn);  // hold state and refresh the timer while heating
  }

  Serial.printf("%.2f C, heater %s\n", tempC, !heaterStateKnown ? "unknown" : (heaterOn ? "on" : "off"));
  delay(kLoopIntervalMs);
}

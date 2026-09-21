// Minimal client for legacy TP-Link Kasa devices (HS100/HS103/HS110/KP105...).
//
// These devices listen on TCP 9999 and speak JSON wrapped in an XOR autokey
// cipher with a 4-byte big-endian length prefix. Discovery is the same cipher
// over a UDP broadcast to the same port, minus the length prefix. No cloud, no
// credentials.
//
// Newer Tapo-generation hardware uses KLAP/AES instead and will NOT work here.

#ifndef KASA_PLUG_H
#define KASA_PLUG_H

#include <Arduino.h>
#include <IPAddress.h>

class KasaPlug {
 public:
  // Largest response we will accept. get_sysinfo on an HS100 is well under 1 kB.
  static const size_t kMaxPayload = 2048;

  // Address the plug by IP. Fine when you control DHCP, but an IP can move.
  explicit KasaPlug(IPAddress host, uint16_t port = 9999);

  // Address the plug by MAC, which never changes. Separators and case are
  // ignored, so "B0:4E:26:C8:48:E0" and "b04e26c848e0" are equivalent.
  // Call discover() before the first command to find its current IP.
  explicit KasaPlug(const char* mac, uint16_t port = 9999);

  // Milliseconds to wait for the TCP connect and for the reply. Defaults 3000.
  void setTimeout(uint32_t connectMs, uint32_t readMs);

  // Broadcasts on UDP 9999 and adopts the address of the plug matching our MAC.
  // Returns false if no match answered in time, leaving the current host alone.
  // Cheap enough to call periodically; it is one broadcast and a short listen.
  bool discover(uint32_t timeoutMs = 5000);

  // Reads, OFF and countdown edits first retry the known address once. If that
  // fails and a MAC is known, re-discover for one second and retry once more.
  // ON, timer creation and arbitrary raw commands are not replayed after an
  // uncertain result. Default true when constructed with a MAC.
  void setRediscoverOnFailure(bool enabled);

  // Seed the address we expect the plug at, skipping the broadcast round trip.
  // The MAC is kept, so if the plug has moved the first failed command still
  // triggers a rediscover. Broadcast discovery is the part most likely to be
  // filtered by an access point, so knowing the IP is the more robust path.
  void setHost(IPAddress host) { host_ = host; }

  IPAddress host() const { return host_; }
  bool hasHost() const { return static_cast<uint32_t>(host_) != 0; }

  // Dead-man's switch. The plug turns ITSELF off this many seconds after the
  // last verified on()/refreshWatchdog(), so a disconnected ESP32 cannot leave the heater
  // running. 0 (the default) disables it.
  //
  // Set this well above your control-loop period: if the loop runs every 10 s,
  // 300 s tolerates a good number of missed cycles before it trips.
  void setWatchdogSeconds(uint32_t seconds);
  uint32_t watchdogSeconds() const { return watchdogSeconds_; }

  // ON succeeds only after the OFF countdown is armed and read back.
  // A false result means state UNKNOWN even though a best-effort OFF is sent.
  // Initial ON + countdown are separate network operations, not an atomic
  // safety interlock. An independent pad thermostat remains the backstop.
  bool on();
  bool off();

  // Renew the existing OFF timer without sending ON or deleting the timer.
  // Returns false if the relay is off, unreachable, or timer verification fails.
  // Two requests: edit the cached rule, then read relay + timer together.
  // On failure, watchdogRemainingMs() determines whether bounded retry is valid.
  bool refreshWatchdog();
  // Conservative time left on the last read-back OFF timer. A transport-only
  // refresh failure retains this deadline; an invalid/off readback cancels it.
  uint32_t watchdogRemainingMs() const;

  // Reads relay_state from get_sysinfo. Returns false if the device could not
  // be reached or the field was missing; isOn is untouched in that case.
  bool readState(bool& isOn);

  // Clears any pending countdown rule without touching the relay.
  bool clearWatchdog();

  // Escape hatch: send a raw JSON request and receive the raw JSON reply.
  bool query(const String& request, String& response);

 private:
  bool sendOnce(const String& request, String& response);
  bool armWatchdog();
  bool writeWatchdogRule();
  bool verifyWatchdog(const String& response, uint32_t requestStarted, bool checkRelay);
  static bool allErrCodesZero(const String& response);
  // Writes 12 uppercase hex chars plus a terminator into out (13 bytes).
  static bool normalizeMac(const char* mac, char* out);
  static bool macFromSysinfo(const String& response, char* out);

  IPAddress host_;
  uint16_t port_;
  uint32_t connectMs_;
  uint32_t readMs_;
  uint32_t watchdogSeconds_;
  String watchdogRuleId_;
  uint32_t watchdogVerifiedAt_ = 0;
  uint32_t watchdogVerifiedForMs_ = 0;
  char mac_[13];
  bool rediscoverOnFailure_;
  bool inRetry_;  // guards against recursing through query()
};

#endif  // KASA_PLUG_H

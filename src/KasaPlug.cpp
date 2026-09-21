#include "KasaPlug.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>

namespace {

const uint8_t kInitializationVector = 171;

// cipher[i] = key ^ plain[i]; key = cipher[i]
void xorEncrypt(const String& plain, uint8_t* out) {
  uint8_t key = kInitializationVector;
  for (size_t i = 0; i < plain.length(); i++) {
    key = key ^ static_cast<uint8_t>(plain[i]);
    out[i] = key;
  }
}

// plain[i] = key ^ cipher[i]; key = cipher[i]
void xorDecrypt(uint8_t* buf, size_t len) {
  uint8_t key = kInitializationVector;
  for (size_t i = 0; i < len; i++) {
    uint8_t cipher = buf[i];
    buf[i] = key ^ cipher;
    key = cipher;
  }
}

// Reads exactly len bytes, or returns false if the deadline passes first.
bool readExactly(WiFiClient& client, uint8_t* dest, size_t len, uint32_t deadline) {
  size_t got = 0;
  while (got < len) {
    if (static_cast<int32_t>(millis() - deadline) >= 0) return false;
    int n = client.read(dest + got, len - got);
    if (n > 0) {
      got += n;
    } else if (!client.connected() && client.available() == 0) {
      return false;
    } else {
      delay(1);
    }
  }
  return true;
}

}  // namespace

KasaPlug::KasaPlug(IPAddress host, uint16_t port)
    : host_(host),
      port_(port),
      connectMs_(3000),
      readMs_(3000),
      watchdogSeconds_(0),
      rediscoverOnFailure_(false),
      inRetry_(false) {
  mac_[0] = '\0';
}

KasaPlug::KasaPlug(const char* mac, uint16_t port)
      // Not 0u: an integer literal with value zero is also a null pointer
      // constant, which makes the call ambiguous against the pointer-taking
      // IPAddress constructors in ESP32 core 3.x.
    : host_(static_cast<uint32_t>(0)),
      port_(port),
      connectMs_(3000),
      readMs_(3000),
      watchdogSeconds_(0),
      rediscoverOnFailure_(true),
      inRetry_(false) {
  if (!normalizeMac(mac, mac_)) mac_[0] = '\0';
}

void KasaPlug::setRediscoverOnFailure(bool enabled) {
  rediscoverOnFailure_ = enabled;
}

void KasaPlug::setTimeout(uint32_t connectMs, uint32_t readMs) {
  connectMs_ = connectMs;
  readMs_ = readMs;
}

void KasaPlug::setWatchdogSeconds(uint32_t seconds) {
  watchdogSeconds_ = seconds;
  watchdogVerifiedForMs_ = 0;
  watchdogRuleId_ = "";
}

bool KasaPlug::sendOnce(const String& request, String& response) {
  const size_t len = request.length();
  if (len == 0 || len > kMaxPayload) return false;
  if (!hasHost()) return false;

  WiFiClient client;
  client.setTimeout(connectMs_ / 1000 > 0 ? connectMs_ / 1000 : 1);
  if (!client.connect(host_, port_, connectMs_)) return false;
  client.setNoDelay(true);

  uint8_t header[4] = {
      static_cast<uint8_t>((len >> 24) & 0xFF), static_cast<uint8_t>((len >> 16) & 0xFF),
      static_cast<uint8_t>((len >> 8) & 0xFF), static_cast<uint8_t>(len & 0xFF)};

  uint8_t* body = static_cast<uint8_t*>(malloc(len));
  if (body == nullptr) {
    client.stop();
    return false;
  }
  xorEncrypt(request, body);

  bool sent = client.write(header, 4) == 4 && client.write(body, len) == len;
  free(body);
  if (!sent) {
    client.stop();
    return false;
  }
  // write() already sends the bytes. Older ESP32 cores implement flush() as
  // an RX discard, which can throw away a fast plug acknowledgement.

  const uint32_t deadline = millis() + readMs_;

  uint8_t replyHeader[4];
  if (!readExactly(client, replyHeader, 4, deadline)) {
    client.stop();
    return false;
  }
  const size_t replyLen = (static_cast<size_t>(replyHeader[0]) << 24) |
                          (static_cast<size_t>(replyHeader[1]) << 16) |
                          (static_cast<size_t>(replyHeader[2]) << 8) |
                          static_cast<size_t>(replyHeader[3]);
  if (replyLen == 0 || replyLen > kMaxPayload) {
    client.stop();
    return false;
  }

  uint8_t* reply = static_cast<uint8_t*>(malloc(replyLen + 1));
  if (reply == nullptr) {
    client.stop();
    return false;
  }
  if (!readExactly(client, reply, replyLen, deadline)) {
    free(reply);
    client.stop();
    return false;
  }
  client.stop();

  xorDecrypt(reply, replyLen);
  reply[replyLen] = '\0';
  response = String(reinterpret_cast<char*>(reply));
  free(reply);
  return true;
}

// Require a well-formed acknowledgement for every command in the response.
bool KasaPlug::allErrCodesZero(const String& response) {
  JsonDocument reply;
  if (deserializeJson(reply, response.c_str()) || !reply.is<JsonObject>()) return false;
  bool found = false;
  for (JsonPair module : reply.as<JsonObject>()) {
    if (!module.value().is<JsonObject>()) return false;
    for (JsonPair command : module.value().as<JsonObject>()) {
      if (!command.value().is<JsonObject>()) return false;
      JsonVariant code = command.value()["err_code"];
      if (!code.is<int>() || code.as<int>() != 0) return false;
      found = true;
    }
  }
  return found;
}

uint32_t KasaPlug::watchdogRemainingMs() const {
  const uint32_t elapsed = static_cast<uint32_t>(millis()) - watchdogVerifiedAt_;
  return elapsed < watchdogVerifiedForMs_ ? watchdogVerifiedForMs_ - elapsed : 0;
}

bool KasaPlug::writeWatchdogRule() {
  JsonDocument request;
  const char* operation = watchdogRuleId_.length() ? "edit_rule" : "add_rule";
  JsonObject rule = request["count_down"][operation].to<JsonObject>();
  if (watchdogRuleId_.length()) rule["id"] = watchdogRuleId_.c_str();
  rule["enable"] = 1;
  rule["delay"] = watchdogSeconds_;
  rule["act"] = 0;
  rule["name"] = "esp32 watchdog";
  char command[384];
  if (measureJson(request) >= sizeof(command)) return false;
  serializeJson(request, command, sizeof(command));
  String response;
  if (!query(String(command), response)) return false;
  if (!allErrCodesZero(response)) {
    // An explicit rejection may mean the cached rule was removed/replaced.
    watchdogVerifiedForMs_ = 0;
    return false;
  }
  return true;
}

bool KasaPlug::verifyWatchdog(const String& response, uint32_t requestStarted,
                              bool checkRelay) {
  // Only a complete, valid readback establishes or extends the deadline.
  watchdogVerifiedForMs_ = 0;
  JsonDocument reply;
  if (deserializeJson(reply, response.c_str())) return false;
  if (checkRelay) {
    JsonObject info = reply["system"]["get_sysinfo"];
    if (!info["err_code"].is<int>() || info["err_code"].as<int>() != 0 ||
        !info["relay_state"].is<int>() || info["relay_state"].as<int>() != 1)
      return false;
  }
  JsonObject rules = reply["count_down"]["get_rules"];
  if (!rules["err_code"].is<int>() || rules["err_code"].as<int>() != 0 ||
      !rules["rule_list"].is<JsonArray>()) return false;
  JsonArray list = rules["rule_list"];
  if (list.size() != 1) return false;
  JsonObject active = list[0];
  const char* id = active["id"];
  const char* name = active["name"];
  if (!id || !*id || !name || strcmp(name, "esp32 watchdog") != 0 ||
      (watchdogRuleId_.length() && watchdogRuleId_ != id)) return false;
  const bool verified = active["enable"].is<int>() && active["enable"].as<int>() == 1 &&
      active["act"].is<int>() && active["act"].as<int>() == 0 &&
      active["delay"].is<uint32_t>() && active["delay"].as<uint32_t>() == watchdogSeconds_ &&
      active["remain"].is<uint32_t>() && active["remain"].as<uint32_t>() > 1 &&
      active["remain"].as<uint32_t>() >= watchdogSeconds_ / 2 &&
      active["remain"].as<uint32_t>() <= watchdogSeconds_ &&
      active["remain"].as<uint32_t>() <= UINT32_MAX / 1000;
  if (!verified) {
    Serial.println("Kasa: countdown not armed as requested; reply follows");
    Serial.println(response);
    return false;
  }
  watchdogRuleId_ = id;
  // Start before the read request (including retries), not after the reply.
  // Also subtract one second for the plug's integer-second reporting.
  watchdogVerifiedAt_ = requestStarted;
  watchdogVerifiedForMs_ = (active["remain"].as<uint32_t>() - 1) * 1000;
  return watchdogRemainingMs() > 0;
}

bool KasaPlug::armWatchdog() {
  watchdogVerifiedForMs_ = 0;
  watchdogRuleId_ = "";
  String response;
  if (!query("{\"count_down\":{\"get_rules\":{}}}", response)) return false;
  JsonDocument reply;
  if (deserializeJson(reply, response.c_str())) return false;
  JsonObject rules = reply["count_down"]["get_rules"];
  if (!rules["err_code"].is<int>() || rules["err_code"].as<int>() != 0 ||
      !rules["rule_list"].is<JsonArray>()) return false;
  JsonArray list = rules["rule_list"];
  if (list.size() > 1) return false;
  if (list.size()) {
    const char* id = list[0]["id"];
    const char* name = list[0]["name"];
    if (!id || !*id || !name || strcmp(name, "esp32 watchdog") != 0) return false;
    watchdogRuleId_ = id;
  }
  if (!writeWatchdogRule()) return false;
  const uint32_t started = millis();
  return query("{\"count_down\":{\"get_rules\":{}}}", response) &&
         verifyWatchdog(response, started, false);
}

bool KasaPlug::refreshWatchdog() {
  if (watchdogSeconds_ == 0) {
    bool isOn = false;
    return readState(isOn) && isOn;
  }
  // Cached ID removes the preliminary timer read. Never create a timer or
  // send ON during refresh, and never renew after the verified deadline.
  if (!watchdogRuleId_.length() || watchdogRemainingMs() == 0) return false;
  if (!writeWatchdogRule()) return false;
  String response;
  const uint32_t started = millis();
  // One read verifies both the actual relay and the renewed OFF countdown.
  // A lost reply retains only the OLD deadline, never an assumed extension.
  return query("{\"system\":{\"get_sysinfo\":{}},\"count_down\":{\"get_rules\":{}}}", response) &&
         verifyWatchdog(response, started, true);
}

bool KasaPlug::clearWatchdog() {
  watchdogVerifiedForMs_ = 0;
  String response;
  return query("{\"count_down\":{\"delete_all_rules\":{}}}", response) &&
         allErrCodesZero(response);
}

bool KasaPlug::on() {
  watchdogVerifiedForMs_ = 0;
  String response;
  if (!query("{\"system\":{\"set_relay_state\":{\"state\":1}}}", response) ||
      !allErrCodesZero(response)) {
    // An acknowledgement can be lost after the relay changed.
    off();
    return false;
  }
  // Some Kasa firmware cancels countdowns on relay transitions: arm after ON.
  // This is not atomic. If arming/verification fails, immediately attempt OFF;
  // the caller must also treat any false return as an UNKNOWN relay state.
  if (watchdogSeconds_ > 0 && !armWatchdog()) {
    off();
    return false;
  }
  return true;
}

bool KasaPlug::off() {
  watchdogVerifiedForMs_ = 0;
  String response;
  if (!query("{\"system\":{\"set_relay_state\":{\"state\":0}}}", response) ||
      !allErrCodesZero(response)) {
    return false;
  }
  // Leave the OFF countdown in place. It is harmless when off and can be
  // renewed without deleting the old protection on the next heating cycle.
  return true;
}

bool KasaPlug::readState(bool& isOn) {
  String response;
  if (!query("{\"system\":{\"get_sysinfo\":{}}}", response)) return false;

  JsonDocument reply;
  if (deserializeJson(reply, response.c_str())) return false;
  JsonVariant state = reply["system"]["get_sysinfo"]["relay_state"];
  if (!state.is<int>() || (state.as<int>() != 0 && state.as<int>() != 1)) return false;
  isOn = state.as<int>() == 1;
  return true;
}

// Accepts "B0:4E:26:C8:48:E0", "b0-4e-26-c8-48-e0" or "b04e26c848e0".
bool KasaPlug::normalizeMac(const char* mac, char* out) {
  if (mac == nullptr) return false;
  size_t n = 0;
  for (const char* p = mac; *p != '\0'; p++) {
    const char c = *p;
    char hex;
    if (c >= '0' && c <= '9') hex = c;
    else if (c >= 'a' && c <= 'f') hex = c - 'a' + 'A';
    else if (c >= 'A' && c <= 'F') hex = c;
    else continue;  // skip ':', '-', spaces
    if (n >= 12) return false;
    out[n++] = hex;
  }
  out[n] = '\0';
  return n == 12;
}

// HS100 reports "mac"; some siblings report "mic_mac" without separators.
bool KasaPlug::macFromSysinfo(const String& response, char* out) {
  const char* keys[] = {"\"mac\":\"", "\"mic_mac\":\""};
  for (int k = 0; k < 2; k++) {
    const int at = response.indexOf(keys[k]);
    if (at < 0) continue;
    const int start = at + strlen(keys[k]);
    const int end = response.indexOf("\"", start);
    if (end < 0 || end - start > 32) continue;
    char raw[33];
    for (int i = 0; i < end - start; i++) raw[i] = response[start + i];
    raw[end - start] = '\0';
    if (normalizeMac(raw, out)) return true;
  }
  return false;
}

bool KasaPlug::discover(uint32_t timeoutMs) {
  if (mac_[0] == '\0') return false;

  WiFiUDP udp;
  if (!udp.begin(0)) return false;  // any free local port

  const String request = "{\"system\":{\"get_sysinfo\":{}}}";
  const size_t len = request.length();
  uint8_t* body = static_cast<uint8_t*>(malloc(len));
  if (body == nullptr) {
    udp.stop();
    return false;
  }
  xorEncrypt(request, body);

  // Discovery omits the length prefix that the TCP framing uses.
  bool sent = udp.beginPacket(IPAddress(255, 255, 255, 255), port_) == 1 &&
              udp.write(body, len) == len && udp.endPacket() == 1;
  free(body);
  if (!sent) {
    udp.stop();
    return false;
  }

  uint8_t* buf = static_cast<uint8_t*>(malloc(kMaxPayload + 1));
  if (buf == nullptr) {
    udp.stop();
    return false;
  }

  const uint32_t deadline = millis() + timeoutMs;
  bool found = false;
  while (static_cast<int32_t>(millis() - deadline) < 0) {
    const int size = udp.parsePacket();
    if (size <= 0) {
      delay(10);
      continue;
    }
    if (static_cast<size_t>(size) > kMaxPayload) continue;  // not ours
    const int n = udp.read(buf, size);
    if (n <= 0) continue;

    xorDecrypt(buf, n);
    buf[n] = '\0';
    const String reply(reinterpret_cast<char*>(buf));

    char replyMac[13];
    // Several plugs may answer one broadcast; keep listening until ours does.
    if (macFromSysinfo(reply, replyMac) && strcmp(replyMac, mac_) == 0) {
      host_ = udp.remoteIP();
      found = true;
      break;
    }
  }

  free(buf);
  udp.stop();
  return found;
}

bool KasaPlug::query(const String& request, String& response) {
  if (sendOnce(request, response)) return true;

  // Do not replay arbitrary commands after a lost reply: add_rule may already
  // have created a timer. Only repeat the reads and idempotent writes we use.
  JsonDocument command;
  if (deserializeJson(command, request.c_str())) return false;
  const bool retrySafe =
      request == "{\"system\":{\"get_sysinfo\":{}}}" ||
      request == "{\"count_down\":{\"get_rules\":{}}}" ||
      request == "{\"system\":{\"get_sysinfo\":{}},\"count_down\":{\"get_rules\":{}}}" ||
      request == "{\"system\":{\"set_relay_state\":{\"state\":0}}}" ||
      (command.size() == 1 && command["count_down"].size() == 1 &&
       command["count_down"]["edit_rule"].is<JsonObject>() &&
       command["count_down"]["edit_rule"]["act"].is<int>() &&
       command["count_down"]["edit_rule"]["act"].as<int>() == 0);
  if (!retrySafe) {
    Serial.println("Kasa: request failed; not replaying uncertain command");
    return false;
  }

  // A dropped TCP reply does not imply a changed IP. Retry the known address
  // once before depending on broadcast discovery, which many APs filter.
  delay(100);
  if (sendOnce(request, response)) {
    Serial.println("Kasa: recovered on same-address retry");
    return true;
  }

  // One rediscovery per failed command: the plug may have changed address.
  // inRetry_ stops a rediscovered-but-still-broken plug from looping.
  if (!rediscoverOnFailure_ || mac_[0] == '\0' || inRetry_) {
    Serial.println("Kasa: TCP request failed twice");
    return false;
  }

  inRetry_ = true;
  const bool ok = discover(1000) && sendOnce(request, response);
  inRetry_ = false;
  if (!ok) Serial.println("Kasa: TCP retry and rediscovery failed");
  return ok;
}

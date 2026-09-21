// Host regression tests for the Kasa fail-off watchdog implementation.
#include <ArduinoJson.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
struct { template <class T> void println(const T&) {} } Serial;
uint32_t now = 10000;
uint32_t millis() { return now; }
struct String : std::string {
  using std::string::string;
  String(const std::string& s) : std::string(s) {}
  int indexOf(const char* needle, int from = 0) const {
    auto pos = find(needle, from);
    return pos == npos ? -1 : static_cast<int>(pos);
  }
};
struct KasaPlug {
  uint32_t watchdogSeconds_ = 300;
  String watchdogRuleId_;
  uint32_t watchdogVerifiedAt_ = 0, watchdogVerifiedForMs_ = 0;
  uint32_t watchdogRemainingMs() const;
  bool writeWatchdogRule(), verifyWatchdog(const String&, uint32_t, bool);
  bool armWatchdog(), refreshWatchdog(), clearWatchdog(), on(), off(), readState(bool&);
  static bool allErrCodesZero(const String&);
  bool query(const String&, String&);
};
bool relay = false, timer = false;
int remaining = 300, getCalls = 0;
uint32_t responseDelay = 0;
bool badTimer = false;
std::string fail, timerName = "esp32 watchdog";
std::vector<std::string> commands;
bool KasaPlug::query(const String& req, String& response) {
  now += responseDelay;
  JsonDocument command;
  assert(!deserializeJson(command, req.c_str()));
  std::string op;
  if (command["system"]["set_relay_state"].is<JsonObject>()) {
    relay = command["system"]["set_relay_state"]["state"].as<int>() == 1;
    op = relay ? "on" : "off";
  } else if (command["system"]["get_sysinfo"].is<JsonObject>())
    op = command["count_down"]["get_rules"].is<JsonObject>() ? "verify_refresh" : "state";
  else {
    for (JsonPair pair : command["count_down"].as<JsonObject>()) op = pair.key().c_str();
  }
  commands.push_back(op);
  if (op == "get_rules") ++getCalls;
  // Simulate a lost acknowledgement after relay action and failures of timer operations.
  if (op == fail || (fail == "verify" && op == "get_rules" && getCalls == 2)) return false;
  if (op == "get_rules" || op == "verify_refresh") {
    JsonDocument result;
    if (op == "verify_refresh") {
      result["system"]["get_sysinfo"]["err_code"] = 0;
      result["system"]["get_sysinfo"]["relay_state"] = relay ? 1 : 0;
    }
    JsonObject rules = result["count_down"]["get_rules"].to<JsonObject>();
    rules["err_code"] = 0;
    JsonArray list = rules["rule_list"].to<JsonArray>();
    if (timer) {
      JsonObject rule = list.add<JsonObject>();
      rule["id"] = "abc123"; rule["name"] = timerName;
      rule["enable"] = badTimer ? 0 : 1; rule["act"] = 0;
      rule["delay"] = 300; rule["remain"] = remaining;
    }
    std::string text; serializeJson(result, text); response = text;
    return true;
  }
  if (op == "state") {
    response = relay ? R"({"system":{"get_sysinfo":{"relay_state":1}}})"
                     : R"({"system":{"get_sysinfo":{"relay_state":0}}})";
    return true;
  }
  if (op == "add_rule" || op == "edit_rule") {
    timer = true;
    assert(command["count_down"][op]["act"].as<int>() == 0);
    if (fail != "stale") remaining = 300;
  }
  if (op == "delete_all_rules") timer = false;
  response = R"({"system":{"result":{"err_code":0}}})";
  return true;
}
#include "kasa.inc"
void reset() {
  now = 10000; responseDelay = 0; badTimer = false;
  relay = timer = false; remaining = 300; getCalls = 0;
  fail.clear(); timerName = "esp32 watchdog"; commands.clear();
}
bool called(const char* op) {
  for (const auto& c : commands) if (c == op) return true;
  return false;
}
int main() {
  KasaPlug plug;
  assert(KasaPlug::allErrCodesZero(R"({"system":{"set_relay_state":{"err_code":0}}})"));
  assert(!KasaPlug::allErrCodesZero(R"({"system":{"set_relay_state":{"err_code":"0"}}})"));
  assert(!KasaPlug::allErrCodesZero(R"({"system":{"set_relay_state":{"err_code":0.5}}})"));
  assert(!KasaPlug::allErrCodesZero(R"({"system":{"set_relay_state":{"err_code":0}})"));
  assert(!KasaPlug::allErrCodesZero(R"({"system":{"set_relay_state":{"err_code":-1}}})"));
  reset(); assert(plug.on() && relay && timer);
  assert(called("add_rule") && !called("delete_all_rules"));
  commands.clear(); getCalls = 0;
  assert(plug.refreshWatchdog());
  assert(commands == std::vector<std::string>({"edit_rule", "verify_refresh"}));
  assert(called("edit_rule") && !called("on") && !called("delete_all_rules"));
  assert(plug.off() && !relay && timer);
  commands.clear(); assert(!plug.refreshWatchdog() && !called("on"));
  for (const auto* failure : {"on", "get_rules", "add_rule", "verify"}) {
    reset(); fail = failure;
    assert(!plug.on() && !relay && called("off"));
  }
  reset(); timer = true; fail = "edit_rule";
  assert(!plug.on() && !relay && timer && !called("delete_all_rules"));
  reset(); timer = true; fail = "stale"; remaining = 2;
  assert(!plug.on() && !relay);  // ACK alone is not timer verification.
  reset(); timer = true; timerName = "someone else's timer";
  assert(!plug.on() && !relay && !called("edit_rule"));
  reset(); relay = timer = true; fail = "edit_rule";
  assert(!plug.refreshWatchdog() && timer && !called("on") && !called("delete_all_rules"));

  // Lost edit or verification replies never extend the last proven deadline.
  for (const auto* failure : {"edit_rule", "verify_refresh"}) {
    reset(); plug = KasaPlug(); assert(plug.on());
    now += 60000; const uint32_t oldRemaining = plug.watchdogRemainingMs();
    fail = failure; responseDelay = 1000;
    assert(!plug.refreshWatchdog());
    assert(plug.watchdogRemainingMs() <= oldRemaining - 1000);
    assert(plug.watchdogRemainingMs() > 0 && relay);
    now += 300000; commands.clear();
    assert(!plug.refreshWatchdog() && commands.empty());
  }
  // Explicit OFF or disabled/foreign timer readbacks remove all grace.
  for (int invalid = 0; invalid < 3; ++invalid) {
    reset(); plug = KasaPlug(); assert(plug.on());
    if (invalid == 0) relay = false;
    if (invalid == 1) badTimer = true;
    if (invalid == 2) timerName = "replacement";
    assert(!plug.refreshWatchdog() && plug.watchdogRemainingMs() == 0);
    assert(!called("delete_all_rules"));
  }
  // Network latency is subtracted, and uint32_t millis rollover is safe.
  reset(); plug = KasaPlug(); responseDelay = 2000; assert(plug.on());
  assert(plug.watchdogRemainingMs() == 297000);
  reset(); plug = KasaPlug(); now = UINT32_MAX - 1000; assert(plug.on());
  now += 5000; assert(plug.watchdogRemainingMs() == 294000);
  assert(plug.off() && plug.watchdogRemainingMs() == 0);
  std::puts("Kasa countdown/acknowledgement regressions passed");
}

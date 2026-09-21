// Host regression tests for Kasa transport retry and rediscovery behaviour.
#include <ArduinoJson.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
struct String : std::string {
  using std::string::string;
  String(const std::string& s) : std::string(s) {}
};
struct { template <class T> void println(const T&) {} } Serial;
unsigned waited = 0;
void delay(unsigned ms) { waited += ms; }
struct KasaPlug {
  bool rediscoverOnFailure_ = true, inRetry_ = false;
  char mac_[13] = "B04E26C848E0";
  unsigned calls = 0, discoveries = 0, discoverTimeout = 0;
  bool discoveryWorks = true;
  std::vector<bool> replies;
  std::vector<std::string> requests;
  bool query(const String&, String&);
  bool sendOnce(const String& req, String& response) {
    requests.push_back(req);
    assert(calls < replies.size());
    bool ok = replies[calls++];
    if (ok) response = "reply";
    return ok;
  }
  bool discover(unsigned timeout) {
    ++discoveries; discoverTimeout = timeout;
    return discoveryWorks;
  }
};
#include "kasa_retry.inc"
int main() {
  String response;
  const std::vector<String> safe = {
    R"({"system":{"get_sysinfo":{}}})",
    R"({"count_down":{"get_rules":{}}})",
    R"({"system":{"get_sysinfo":{}},"count_down":{"get_rules":{}}})",
    R"({"system":{"set_relay_state":{"state":0}}})",
    R"({"count_down":{"edit_rule":{"id":"abc","enable":1,"act":0,"delay":300}}})"
  };
  for (const auto& request : safe) {
    KasaPlug p; p.replies = {false, true};
    assert(p.query(request, response));
    assert(p.calls == 2 && p.discoveries == 0);
    assert(p.requests[0] == p.requests[1]);
    // A filtered discovery broadcast must not prevent direct-address recovery.
    p = KasaPlug(); p.discoveryWorks = false; p.replies = {false, true};
    assert(p.query(request, response) && p.discoveries == 0);
    p = KasaPlug(); p.replies = {false, false, true};
    assert(p.query(request, response));
    assert(p.calls == 3 && p.discoveries == 1 && p.discoverTimeout == 1000);
    p = KasaPlug(); p.replies = {false, false, false};
    assert(!p.query(request, response) && p.calls == 3 && !p.inRetry_);
    p = KasaPlug(); p.discoveryWorks = false; p.replies = {false, false};
    assert(!p.query(request, response) && p.calls == 2 && p.discoveries == 1);
    p = KasaPlug(); p.rediscoverOnFailure_ = false; p.replies = {false, false};
    assert(!p.query(request, response) && p.calls == 2 && p.discoveries == 0);
  }
  // Lost ON/add acknowledgement may follow a successful physical action.
  // No duplicate ON, timer creation, or arbitrary raw operation is sent.
  for (const String& request : std::vector<String>{
      R"({"system":{"set_relay_state":{"state":1}}})",
      R"({"count_down":{"add_rule":{"act":0,"delay":300}}})",
      R"({"count_down":{"edit_rule":{"act":1,"delay":300}}})",
      R"({"count_down":{"edit_rule":{"act":0},"add_rule":{"act":1}}})",
      R"({"system":{"reboot":{}}})", "malformed"}) {
    KasaPlug p; p.replies = {false};
    assert(!p.query(request, response) && p.calls == 1 && p.discoveries == 0);
  }
  KasaPlug p; p.replies = {true};
  assert(p.query(safe[0], response) && p.calls == 1 && p.discoveries == 0);
  std::puts("Kasa bounded retry regressions passed");
}

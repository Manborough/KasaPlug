// Host regression tests for framed Kasa TCP transport.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct String : std::string {
  using std::string::string;
  String(const std::string& value) : std::string(value) {}
};

uint32_t now = 1000;
uint32_t millis() { return now; }
void delay(unsigned milliseconds) { now += milliseconds; }

struct IPAddress {
  explicit IPAddress(uint32_t value = 0) : value(value) {}
  operator uint32_t() const { return value; }
  uint32_t value;
};

struct ClientScript {
  bool connectOK = true;
  bool connected = true;
  int failWriteCall = -1;
  size_t maxRead = SIZE_MAX;
  unsigned writeCalls = 0;
  std::vector<uint8_t> incoming;
  std::vector<uint8_t> outgoing;
};

ClientScript script;

struct WiFiClient {
  void setTimeout(uint32_t) {}
  bool connect(IPAddress, uint16_t, uint32_t) { return script.connectOK; }
  void setNoDelay(bool) {}
  size_t write(const uint8_t* data, size_t length) {
    const int call = static_cast<int>(script.writeCalls++);
    if (call == script.failWriteCall) return length ? length - 1 : 0;
    script.outgoing.insert(script.outgoing.end(), data, data + length);
    return length;
  }
  int read(uint8_t* destination, size_t length) {
    if (script.incoming.empty()) return 0;
    const size_t count = std::min({length, script.maxRead, script.incoming.size()});
    std::copy_n(script.incoming.begin(), count, destination);
    script.incoming.erase(script.incoming.begin(), script.incoming.begin() + count);
    return static_cast<int>(count);
  }
  bool connected() const { return script.connected; }
  int available() const { return static_cast<int>(script.incoming.size()); }
  void stop() { script.connected = false; }
};

struct KasaPlug {
  static const size_t kMaxPayload = 2048;
  IPAddress host_{0x01020304};
  uint16_t port_ = 9999;
  uint32_t connectMs_ = 20;
  uint32_t readMs_ = 20;
  bool hasHost() const { return static_cast<uint32_t>(host_) != 0; }
  bool sendOnce(const String& request, String& response);
};

const uint8_t kInitializationVector = 171;
#include "kasa_transport.inc"

void reset() {
  now = 1000;
  script = ClientScript();
}

void setReply(const std::string& plain) {
  std::vector<uint8_t> body(plain.size());
  xorEncrypt(String(plain), body.data());
  const uint32_t size = static_cast<uint32_t>(body.size());
  script.incoming = {
      static_cast<uint8_t>(size >> 24), static_cast<uint8_t>(size >> 16),
      static_cast<uint8_t>(size >> 8), static_cast<uint8_t>(size)};
  script.incoming.insert(script.incoming.end(), body.begin(), body.end());
}

int main() {
  KasaPlug plug;
  String response;

  reset();
  setReply(R"({"system":{"get_sysinfo":{"relay_state":0}}})");
  script.maxRead = 1;  // TCP may split both the header and body arbitrarily.
  const String request = R"({"system":{"get_sysinfo":{}}})";
  assert(plug.sendOnce(request, response));
  assert(response == R"({"system":{"get_sysinfo":{"relay_state":0}}})");
  assert(script.outgoing.size() == request.length() + 4);
  assert(script.outgoing[0] == 0 && script.outgoing[1] == 0);
  assert(script.outgoing[2] == (request.length() >> 8));
  assert(script.outgoing[3] == (request.length() & 0xff));
  std::vector<uint8_t> sent(script.outgoing.begin() + 4, script.outgoing.end());
  xorDecrypt(sent.data(), sent.size());
  assert(std::string(sent.begin(), sent.end()) == request);

  reset(); plug.host_ = IPAddress(0); setReply("{}");
  assert(!plug.sendOnce("{}", response) && script.outgoing.empty());
  plug.host_ = IPAddress(0x01020304);
  reset(); assert(!plug.sendOnce("", response));
  reset(); assert(!plug.sendOnce(String(KasaPlug::kMaxPayload + 1, 'x'), response));
  reset(); script.connectOK = false; assert(!plug.sendOnce("{}", response));
  for (int failedWrite : {0, 1}) {
    reset(); setReply("{}"); script.failWriteCall = failedWrite;
    assert(!plug.sendOnce("{}", response));
  }

  reset(); script.incoming = {0, 0, 0, 0};
  assert(!plug.sendOnce("{}", response));
  reset(); script.incoming = {0, 0, 8, 1};
  assert(!plug.sendOnce("{}", response));
  reset(); setReply("response"); script.incoming.pop_back(); script.connected = false;
  assert(!plug.sendOnce("{}", response));
  reset(); script.connected = true;
  assert(!plug.sendOnce("{}", response));
  assert(now >= 1000 + plug.readMs_);

  std::puts("Kasa TCP framing regressions passed");
}

// Host regression tests for Kasa protocol encoding and address normalization.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct String : std::string {
  using std::string::string;
  String(const std::string& value) : std::string(value) {}
  int indexOf(const char* needle, int from = 0) const {
    const auto position = find(needle, static_cast<size_t>(from));
    return position == npos ? -1 : static_cast<int>(position);
  }
};

struct KasaPlug {
  static bool normalizeMac(const char* mac, char* out);
  static bool macFromSysinfo(const String& response, char* out);
};

const uint8_t kInitializationVector = 171;
#include "kasa_codec.inc"

int main() {
  const String plain = R"({"system":{"get_sysinfo":{}}})";
  std::vector<uint8_t> encrypted(plain.length());
  xorEncrypt(plain, encrypted.data());
  assert(encrypted != std::vector<uint8_t>(plain.begin(), plain.end()));
  xorDecrypt(encrypted.data(), encrypted.size());
  assert(std::string(encrypted.begin(), encrypted.end()) == plain);

  char mac[13];
  assert(KasaPlug::normalizeMac("B0:4E:26:C8:48:E0", mac));
  assert(std::strcmp(mac, "B04E26C848E0") == 0);
  assert(KasaPlug::normalizeMac("b0-4e-26-c8-48-e0", mac));
  assert(std::strcmp(mac, "B04E26C848E0") == 0);
  assert(KasaPlug::normalizeMac(" b04e26c848e0 ", mac));
  assert(std::strcmp(mac, "B04E26C848E0") == 0);
  assert(!KasaPlug::normalizeMac(nullptr, mac));
  assert(!KasaPlug::normalizeMac("B0:4E:26:C8:48", mac));
  assert(!KasaPlug::normalizeMac("B0:4E:26:C8:48:E0:11", mac));

  assert(KasaPlug::macFromSysinfo(
      R"({"system":{"get_sysinfo":{"mac":"b0:4e:26:c8:48:e0"}}})", mac));
  assert(std::strcmp(mac, "B04E26C848E0") == 0);
  assert(KasaPlug::macFromSysinfo(
      R"({"system":{"get_sysinfo":{"mic_mac":"b04e26c848e0"}}})", mac));
  assert(std::strcmp(mac, "B04E26C848E0") == 0);
  assert(!KasaPlug::macFromSysinfo(R"({"system":{"get_sysinfo":{}}})", mac));
  assert(!KasaPlug::macFromSysinfo(R"({"mac":"not-a-mac"})", mac));
  assert(!KasaPlug::macFromSysinfo(R"({"mac":"123456789012345678901234567890123"})", mac));

  std::puts("Kasa codec and MAC regressions passed");
}

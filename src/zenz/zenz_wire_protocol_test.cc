#include "zenz/zenz_wire_protocol.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include "testing/gunit.h"

namespace mozc {
namespace zenz {
namespace {

TEST(ZenzWireProtocolTest, ConstantsRemainStable) {
  EXPECT_EQ(kZenzWireMagic, 0x315A4E5A);
  EXPECT_EQ(kZenzWireVersion, 1);
  EXPECT_EQ(kZenzWireKindRequest, 1);
  EXPECT_EQ(kZenzWireKindResponse, 2);
  EXPECT_EQ(kZenzWireStatusOk, 0);
  EXPECT_EQ(kZenzWireStatusError, 1);
  EXPECT_EQ(kZenzWireStatusTimeout, 2);
}

TEST(ZenzWireProtocolTest, RequestHeaderLayoutRemainsStable) {
  EXPECT_EQ(sizeof(ZenzWireRequestHeader), 24);
  EXPECT_EQ(offsetof(ZenzWireRequestHeader, magic), 0);
  EXPECT_EQ(offsetof(ZenzWireRequestHeader, version), 4);
  EXPECT_EQ(offsetof(ZenzWireRequestHeader, kind), 6);
  EXPECT_EQ(offsetof(ZenzWireRequestHeader, generation), 8);
  EXPECT_EQ(offsetof(ZenzWireRequestHeader, timeout_msec), 12);
  EXPECT_EQ(offsetof(ZenzWireRequestHeader, max_output_chars), 16);
  EXPECT_EQ(offsetof(ZenzWireRequestHeader, prompt_size), 20);
}

TEST(ZenzWireProtocolTest, ResponseHeaderLayoutRemainsStable) {
  EXPECT_EQ(sizeof(ZenzWireResponseHeader), 28);
  EXPECT_EQ(offsetof(ZenzWireResponseHeader, magic), 0);
  EXPECT_EQ(offsetof(ZenzWireResponseHeader, version), 4);
  EXPECT_EQ(offsetof(ZenzWireResponseHeader, kind), 6);
  EXPECT_EQ(offsetof(ZenzWireResponseHeader, generation), 8);
  EXPECT_EQ(offsetof(ZenzWireResponseHeader, status), 12);
  EXPECT_EQ(offsetof(ZenzWireResponseHeader, latency_msec), 16);
  EXPECT_EQ(offsetof(ZenzWireResponseHeader, value_size), 20);
  EXPECT_EQ(offsetof(ZenzWireResponseHeader, debug_size), 24);
}

TEST(ZenzWireProtocolTest, DeviceRequestVersionAndTrailerRemainStable) {
  EXPECT_EQ(kZenzWireVersionWithDevice, 2);
  EXPECT_EQ(kZenzWireMaxBackendDeviceBytes, 128);
  EXPECT_EQ(sizeof(ZenzWireDeviceTrailer), 4);
}

bool IsValidDevice(const std::string& device) {
  return IsValidZenzWireBackendDevice(device.data(),
                                      static_cast<uint32_t>(device.size()));
}

TEST(ZenzWireProtocolTest, AcceptsOnlyPlainDeviceIdentifiers) {
  EXPECT_TRUE(IsValidDevice("MTL0"));
  EXPECT_TRUE(IsValidDevice("CUDA0"));
  EXPECT_TRUE(IsValidDevice("none"));
  EXPECT_TRUE(IsValidDevice("Vulkan0.dev_1-a"));
  EXPECT_FALSE(IsValidDevice(""));
  EXPECT_FALSE(IsValidDevice("-MTL0"));
  EXPECT_FALSE(IsValidDevice("--host=0.0.0.0"));
  EXPECT_FALSE(IsValidDevice("MTL0 --port"));
  EXPECT_FALSE(IsValidDevice(std::string("MTL\0", 4)));
  EXPECT_FALSE(IsValidDevice("GPU/0"));
  EXPECT_TRUE(IsValidDevice(std::string(128, 'a')));
  EXPECT_FALSE(IsValidDevice(std::string(129, 'a')));
}

}  // namespace
}  // namespace zenz
}  // namespace mozc

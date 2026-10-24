/**
 * @file tests/unit/platform/test_wayland_hdr.cpp
 * @brief Test src/platform/linux/wayland_hdr.h and src/platform/linux/sway_ipc.*.
 */
#include "../../tests_common.h"

#ifdef SUNSHINE_BUILD_WAYLAND
  #include <src/platform/linux/sway_ipc.h>
  #include <src/platform/linux/wayland_hdr.h>

namespace {
  constexpr auto outputs_json = R"([
    {"name": "HEADLESS-1", "active": true, "hdr": true},
    {"name": "HEADLESS-2", "active": true, "hdr": false},
    {"name": "DP-1", "active": true}
  ])";
}  // namespace

TEST(SwayIpcOutputHdr, ReportsHdrPerOutput) {
  EXPECT_EQ(sway_ipc::output_hdr_from_json(outputs_json, "HEADLESS-1"), std::optional<bool> {true});
  EXPECT_EQ(sway_ipc::output_hdr_from_json(outputs_json, "HEADLESS-2"), std::optional<bool> {false});
}

TEST(SwayIpcOutputHdr, UnknownWhenFieldMissing) {
  EXPECT_EQ(sway_ipc::output_hdr_from_json(outputs_json, "DP-1"), std::nullopt);
}

TEST(SwayIpcOutputHdr, UnknownWhenOutputMissing) {
  EXPECT_EQ(sway_ipc::output_hdr_from_json(outputs_json, "HEADLESS-3"), std::nullopt);
  EXPECT_EQ(sway_ipc::output_hdr_from_json("[]", "HEADLESS-1"), std::nullopt);
}

TEST(SwayIpcOutputHdr, UnknownWhenMalformed) {
  EXPECT_EQ(sway_ipc::output_hdr_from_json("", "HEADLESS-1"), std::nullopt);
  EXPECT_EQ(sway_ipc::output_hdr_from_json("{not json", "HEADLESS-1"), std::nullopt);
  EXPECT_EQ(sway_ipc::output_hdr_from_json(R"({"name": "HEADLESS-1", "hdr": true})", "HEADLESS-1"), std::nullopt);
  EXPECT_EQ(sway_ipc::output_hdr_from_json(R"([{"name": "HEADLESS-1", "hdr": "yes"}])", "HEADLESS-1"), std::nullopt);
}

TEST(WaylandHdrFormat, TenBitFourccs) {
  EXPECT_TRUE(wl::hdr::is_10bit_fourcc(wl::hdr::fourcc('X', 'B', '3', '0')));
  EXPECT_TRUE(wl::hdr::is_10bit_fourcc(wl::hdr::fourcc('X', 'R', '3', '0')));
  EXPECT_TRUE(wl::hdr::is_10bit_fourcc(wl::hdr::fourcc('A', 'B', '3', '0')));
  EXPECT_TRUE(wl::hdr::is_10bit_fourcc(wl::hdr::fourcc('A', 'R', '3', '0')));
}

TEST(WaylandHdrFormat, EightBitFourccsAreNotHdr) {
  EXPECT_FALSE(wl::hdr::is_10bit_fourcc(wl::hdr::fourcc('X', 'R', '2', '4')));  // XRGB8888
  EXPECT_FALSE(wl::hdr::is_10bit_fourcc(wl::hdr::fourcc('A', 'R', '2', '4')));  // ARGB8888
  EXPECT_FALSE(wl::hdr::is_10bit_fourcc(wl::hdr::fourcc('X', 'B', '2', '4')));  // XBGR8888
  EXPECT_FALSE(wl::hdr::is_10bit_fourcc(0));
}

TEST(WaylandHdrModifiers, DropsInvalidAndDuplicates) {
  const std::vector<std::uint64_t> advertised {0x0300000000606014ULL, wl::hdr::MODIFIER_INVALID, 0x0, 0x0300000000606014ULL};
  const std::vector<std::uint64_t> expected {0x0300000000606014ULL, 0x0};
  EXPECT_EQ(wl::hdr::filter_modifiers(advertised), expected);
}

TEST(WaylandHdrModifiers, KeepsAdvertisedOrderAndHandlesEmpty) {
  const std::vector<std::uint64_t> advertised {3, 1, 2};
  EXPECT_EQ(wl::hdr::filter_modifiers(advertised), advertised);
  EXPECT_TRUE(wl::hdr::filter_modifiers({}).empty());
  EXPECT_TRUE(wl::hdr::filter_modifiers({wl::hdr::MODIFIER_INVALID}).empty());
}

TEST(WaylandHdrMetadata, UnitConversion) {
  EXPECT_EQ(wl::hdr::chromaticity_units(0.708), 35400);
  EXPECT_EQ(wl::hdr::chromaticity_units(0.3127), 15635);
  EXPECT_EQ(wl::hdr::min_luminance_units(0.005), 50);
}

TEST(WaylandHdrMetadata, Bt2020D65Defaults) {
  SS_HDR_METADATA metadata;
  wl::hdr::fill_metadata(metadata);

  EXPECT_EQ(metadata.displayPrimaries[0].x, 35400);  // red
  EXPECT_EQ(metadata.displayPrimaries[0].y, 14600);
  EXPECT_EQ(metadata.displayPrimaries[1].x, 8500);  // green
  EXPECT_EQ(metadata.displayPrimaries[1].y, 39850);
  EXPECT_EQ(metadata.displayPrimaries[2].x, 6550);  // blue
  EXPECT_EQ(metadata.displayPrimaries[2].y, 2300);
  EXPECT_EQ(metadata.whitePoint.x, 15635);
  EXPECT_EQ(metadata.whitePoint.y, 16450);
  EXPECT_EQ(metadata.maxDisplayLuminance, 1000);
  EXPECT_EQ(metadata.minDisplayLuminance, 50);
  EXPECT_EQ(metadata.maxContentLightLevel, 1000);
  EXPECT_EQ(metadata.maxFrameAverageLightLevel, 400);
}
#endif

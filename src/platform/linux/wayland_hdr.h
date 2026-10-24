/**
 * @file src/platform/linux/wayland_hdr.h
 * @brief Pure helpers for HDR and DMA-BUF modifier handling in Wayland capture.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// lib includes
#include <moonlight-common-c/src/Limelight.h>

namespace wl::hdr {
  constexpr std::uint32_t fourcc(char a, char b, char c, char d) {
    return (std::uint32_t) a | ((std::uint32_t) b << 8) | ((std::uint32_t) c << 16) | ((std::uint32_t) d << 24);
  }

  // drm_fourcc.h values for the 10-bit-per-channel RGB formats
  constexpr std::uint32_t FORMAT_XRGB2101010 = fourcc('X', 'R', '3', '0');
  constexpr std::uint32_t FORMAT_XBGR2101010 = fourcc('X', 'B', '3', '0');
  constexpr std::uint32_t FORMAT_ARGB2101010 = fourcc('A', 'R', '3', '0');
  constexpr std::uint32_t FORMAT_ABGR2101010 = fourcc('A', 'B', '3', '0');

  constexpr std::uint64_t MODIFIER_INVALID = 0x00ffffffffffffffULL;

  /**
   * @brief Check whether a DRM fourcc is a 10-bit RGB format, which sway only uses for HDR outputs.
   */
  constexpr bool is_10bit_fourcc(std::uint32_t format) {
    return format == FORMAT_XRGB2101010 || format == FORMAT_XBGR2101010 ||
           format == FORMAT_ARGB2101010 || format == FORMAT_ABGR2101010;
  }

  /**
   * @brief Drop invalid and duplicate modifiers, keeping the advertised order.
   */
  inline std::vector<std::uint64_t> filter_modifiers(const std::vector<std::uint64_t> &advertised) {
    std::vector<std::uint64_t> filtered;
    for (auto modifier : advertised) {
      if (modifier != MODIFIER_INVALID && std::find(filtered.begin(), filtered.end(), modifier) == filtered.end()) {
        filtered.push_back(modifier);
      }
    }
    return filtered;
  }

  // Mastering metadata sway doesn't expose for a client. Adjust here if needed.
  constexpr double PRIMARY_RED_X = 0.708;  ///< BT.2020
  constexpr double PRIMARY_RED_Y = 0.292;
  constexpr double PRIMARY_GREEN_X = 0.170;
  constexpr double PRIMARY_GREEN_Y = 0.797;
  constexpr double PRIMARY_BLUE_X = 0.131;
  constexpr double PRIMARY_BLUE_Y = 0.046;
  constexpr double WHITE_POINT_X = 0.3127;  ///< D65
  constexpr double WHITE_POINT_Y = 0.3290;
  constexpr double MAX_DISPLAY_LUMINANCE_NITS = 1000.0;
  constexpr double MIN_DISPLAY_LUMINANCE_NITS = 0.005;
  constexpr std::uint16_t MAX_CLL_NITS = 1000;
  constexpr std::uint16_t MAX_FALL_NITS = 400;

  /**
   * @brief Chromaticity coordinate to the 0.00002 units used by KMS and SS_HDR_METADATA.
   */
  constexpr std::uint16_t chromaticity_units(double value) {
    return (std::uint16_t) (value / 0.00002 + 0.5);
  }

  /**
   * @brief Min luminance to the 0.0001 cd/m^2 units used by KMS and SS_HDR_METADATA.
   */
  constexpr std::uint16_t min_luminance_units(double nits) {
    return (std::uint16_t) (nits / 0.0001 + 0.5);
  }

  /**
   * @brief Fill HDR metadata with the fixed values above, in the same units as the KMS path.
   */
  inline void fill_metadata(SS_HDR_METADATA &metadata) {
    std::memset(&metadata, 0, sizeof(metadata));

    metadata.displayPrimaries[0].x = chromaticity_units(PRIMARY_RED_X);
    metadata.displayPrimaries[0].y = chromaticity_units(PRIMARY_RED_Y);
    metadata.displayPrimaries[1].x = chromaticity_units(PRIMARY_GREEN_X);
    metadata.displayPrimaries[1].y = chromaticity_units(PRIMARY_GREEN_Y);
    metadata.displayPrimaries[2].x = chromaticity_units(PRIMARY_BLUE_X);
    metadata.displayPrimaries[2].y = chromaticity_units(PRIMARY_BLUE_Y);
    metadata.whitePoint.x = chromaticity_units(WHITE_POINT_X);
    metadata.whitePoint.y = chromaticity_units(WHITE_POINT_Y);
    metadata.maxDisplayLuminance = (std::uint16_t) MAX_DISPLAY_LUMINANCE_NITS;
    metadata.minDisplayLuminance = min_luminance_units(MIN_DISPLAY_LUMINANCE_NITS);
    metadata.maxContentLightLevel = MAX_CLL_NITS;
    metadata.maxFrameAverageLightLevel = MAX_FALL_NITS;
  }
}  // namespace wl::hdr

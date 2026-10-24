/**
 * @file tests/unit/platform/test_sway_hdr.cpp
 * @brief Test src/platform/linux/sway_hdr.h.
 */
#include "../../tests_common.h"

#ifdef __linux__
  #include <src/platform/linux/sway_hdr.h>

using namespace platf::sway_hdr;

namespace {
  const std::vector<render_node_t> two_gpus {
    {"/dev/dri/renderD128", "nvidia"},
    {"/dev/dri/renderD129", "amdgpu"},
  };

  std::string var(const compositor_env_t &env, const std::string &name) {
    for (const auto &[key, value] : env.vars) {
      if (key == name) return value;
    }
    return "<unset>";
  }
}

TEST(SwayHdrDevice, AdapterNameWins) {
  EXPECT_EQ(select_render_device("/dev/dri/renderD129", "nvenc", two_gpus), "/dev/dri/renderD129");
}

TEST(SwayHdrDevice, NvidiaPreferredForNvenc) {
  EXPECT_EQ(select_render_device("", "nvenc", two_gpus), "/dev/dri/renderD128");
  EXPECT_EQ(select_render_device("", "", two_gpus), "/dev/dri/renderD128");
}

TEST(SwayHdrDevice, ForeignAdapterNameIgnored) {
  EXPECT_EQ(select_render_device("Some GPU", "nvenc", two_gpus), "/dev/dri/renderD128");
}

TEST(SwayHdrDevice, UnsetOtherwise) {
  for (const char *encoder : {"vaapi", "quicksync", "amdvce", "software", "videotoolbox"}) {
    EXPECT_EQ(select_render_device("", encoder, two_gpus), "") << encoder;
  }
  EXPECT_EQ(select_render_device("", "nvenc", {{"/dev/dri/renderD129", "amdgpu"}}), "");
  EXPECT_EQ(select_render_device("", "", {}), "");
  EXPECT_EQ(select_render_device("", "", {{"/dev/dri/renderD129", "amdgpu"}}), "");
}

TEST(SwayHdrEnv, DefaultsToVulkan) {
  auto env = select_compositor_env("", "", "", "nvenc", two_gpus, false);
  EXPECT_EQ(var(env, "WLR_RENDERER"), "vulkan");
  EXPECT_EQ(var(env, "WLR_RENDER_DRM_DEVICE"), "/dev/dri/renderD128");
}

TEST(SwayHdrEnv, GlesFallback) {
  auto env = select_compositor_env("", "", "", "nvenc", two_gpus, true);
  EXPECT_EQ(var(env, "WLR_RENDERER"), "gles2");
}

TEST(SwayHdrEnv, UserSettingsRespected) {
  auto env = select_compositor_env("gles2", "/dev/dri/renderD129", "", "nvenc", two_gpus, false);
  EXPECT_EQ(var(env, "WLR_RENDERER"), "<unset>");
  EXPECT_EQ(var(env, "WLR_RENDER_DRM_DEVICE"), "<unset>");
}

TEST(SwayHdrCommand, OnOff) {
  EXPECT_EQ(hdr_command("HEADLESS-2", true), (std::vector<std::string> {"output", "HEADLESS-2", "hdr", "on"}));
  EXPECT_EQ(hdr_command("HEADLESS-2", false), (std::vector<std::string> {"output", "HEADLESS-2", "hdr", "off"}));
}

TEST(SwayHdrOutputs, ParsesFlag) {
  const std::string json = R"([{"name":"HEADLESS-1","hdr":true},{"name":"HEADLESS-2","hdr":false},{"name":"HEADLESS-3"}])";
  EXPECT_EQ(output_hdr_enabled(json, "HEADLESS-1"), true);
  EXPECT_EQ(output_hdr_enabled(json, "HEADLESS-2"), false);
  EXPECT_EQ(output_hdr_enabled(json, "HEADLESS-3"), false);
  EXPECT_EQ(output_hdr_enabled(json, "HEADLESS-9"), std::nullopt);
}

TEST(SwayHdrOutputs, BadJson) {
  EXPECT_EQ(output_hdr_enabled("not json", "HEADLESS-1"), std::nullopt);
  EXPECT_EQ(output_hdr_enabled(R"({"name":"HEADLESS-1"})", "HEADLESS-1"), std::nullopt);
  EXPECT_EQ(output_hdr_enabled("[]", "HEADLESS-1"), std::nullopt);
}
#endif

# Sway for Helios virtual displays: wlroots headless outputs patched to
# advertise BT.2020 primaries and PQ, so `output NAME hdr on` can work (it also
# needs the Vulkan renderer, WLR_RENDERER=vulkan).
{
  sway,
  sway-unwrapped,
  wlroots_0_20,
}:
sway.override {
  sway-unwrapped = sway-unwrapped.override {
    wlroots_0_20 = wlroots_0_20.overrideAttrs (old: {
      patches = (old.patches or [ ]) ++ [ ./patches/wlroots-headless-hdr.patch ];
    });
  };
}

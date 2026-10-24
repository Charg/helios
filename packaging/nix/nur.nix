{
  pkgs ? import <nixpkgs> { },
}:
{
  helios = pkgs.callPackage ./package.nix { };
  sway = pkgs.callPackage ./sway.nix { };
  nixosModules.helios = import ./module.nix;
}

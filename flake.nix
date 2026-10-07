{
  description = "bdex-framegen: frame generation for Vulkan games, as a Vulkan layer";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in {
      packages = forAll (pkgs: {
        default = pkgs.callPackage ./packaging/nix/package.nix { };
      } // nixpkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isx86_64 {
        # The 32-bit layer alone (for 32-bit games): its manifest is
        # VK_LAYER_BDEX_framegen_32.json, beside the 64-bit one.
        layer32 = pkgs.pkgsi686Linux.callPackage ./packaging/nix/package.nix { withPanel = false; };
      });
      nixosModules.default = import ./packaging/nix/module.nix self;
    };
}

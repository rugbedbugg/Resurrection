{
  description = "Project development tools for NixOS; application dependencies retain their own lockfiles";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/26f4107e47e3ad0ffd0db220469ba83b6f6d96e8";
  outputs = { self, nixpkgs, ... }@inputs:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      criu = pkgs.criu;
    in {
      devShells.${system}.default = pkgs.mkShell {
        packages = with pkgs; [ cmake ninja gnumake gcc gdb pkg-config sqlite criu python313 util-linux ];
        shellHook = ''
          export CMAKE_PREFIX_PATH="${criu.dev}:${criu}''${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
        '';
      };
    };
}

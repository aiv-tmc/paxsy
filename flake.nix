{
  # flake.nix - Nix build for the paxsy frontend toolchain.
  # Exposes a package for x86_64-linux and aarch64-linux built with the
  # standard C toolchain; use `nix build` (or `nix run`) at the flake
  # root, or add the flake as an input to consume `packages.default`.

  description = "Frontend toolchain for the Paxsy systems programming language";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      packages = forAllSystems (system:
        let
          stdenv = nixpkgs.legacyPackages.${system}.stdenv;
        in
        {
          default = stdenv.mkDerivation {
            pname = "paxsy";
            version = "1.0.0";
            src = self;

            installPhase = ''
              install -Dm755 paxsy $out/bin/paxsy
              install -Dm644 docs/cli.md $out/share/doc/paxsy/cli.md
              install -Dm644 editors/vim/ftdetect/px.vim \
                $out/share/vim/vimfiles/ftdetect/px.vim
              install -Dm644 editors/vim/syntax/px.vim \
                $out/share/vim/vimfiles/syntax/px.vim
            '';
          };
        });

      apps = forAllSystems (system: {
        default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/paxsy";
        };
      });
    };
}

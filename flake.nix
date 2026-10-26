{
  description = "swayimg - image viewer for Sway/Wayland (fork)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs {
          inherit system;
          # Allow unfree for some codecs if needed
          config = {
            allowUnfree = true;
          };
        };

        # All libraries/packages needed to build swayimg
        buildInputs = with pkgs; [
          wayland
          wayland-protocols
          libxkbcommon
          exiv2
          fontconfig
          freetype
          giflib
          libheif
          libjpeg
          libwebp
          libtiff
          librsvg
          libpng
          libjxl
          libavif
          libsixel
          libraw
          libdrm
          luajit
          openexr
          openjpeg
          minizip-ng
          bash-completion
        ];

        nativeBuildInputs = with pkgs; [
          meson
          ninja
          pkg-config
          wayland-scanner
          python3
          gtest
        ];
      in
      {
        packages.swayimg = pkgs.stdenv.mkDerivation (finalAttrs: {
          pname = "swayimg";
          version = "5.6";

          src = self;

          depsBuildBuild = [ pkgs.pkg-config ];

          nativeBuildInputs = [
            pkgs.meson
            pkgs.ninja
            pkgs.pkg-config
            pkgs.wayland-scanner
            pkgs.python3
          ];

          buildInputs = buildInputs;

          mesonFlags = [
            (pkgs.lib.mesonOption "version" finalAttrs.version)
          ];

          meta = {
            homepage = "https://github.com/artemsen/swayimg";
            description = "Image viewer for Sway/Wayland";
            license = pkgs.lib.licenses.mit;
            platforms = pkgs.lib.platforms.linux;
            mainProgram = "swayimg";
          };
        });

        packages.default = self.packages.${system}.swayimg;

        devShells.default = pkgs.mkShell {
          nativeBuildInputs = nativeBuildInputs;
          buildInputs = buildInputs;

          # Make Wayland and other env vars available in the shell
          shellHook = ''
            echo "swayimg dev shell ready"
            echo "Use: meson setup build && meson compile -C build"
          '';
        };
      }
    );
}

{
  description = "MesenCE - multi-system emulator (dev shell + native core build)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "aarch64-darwin"
        "x86_64-darwin"
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (
        pkgs:
        let
          inherit (pkgs) lib stdenv;
        in
        {
          # Native emulation core (MesenCore.so / MesenCore.dylib) built with the
          # upstream makefile. The Avalonia UI is built with `mise run build`
          # inside the dev shell (it needs NuGet network access).
          core = pkgs.clangStdenv.mkDerivation {
            pname = "mesen-core";
            version = "unstable-${self.shortRev or self.dirtyShortRev or "dirty"}";

            src = lib.cleanSourceWith {
              src = ./.;
              filter =
                path: type:
                let
                  rel = lib.removePrefix (toString ./. + "/") (toString path);
                in
                !(lib.hasPrefix "UI" rel || lib.hasPrefix "bin" rel || lib.hasSuffix ".o" rel);
            };

            enableParallelBuilding = true;

            makeFlags = [
              "core"
              # Static libstdc++ / LTO are for portable release binaries; not needed in Nix.
              "STATICLINK=false"
              "LTO=false"
              # Upstream adds -Werror for clang on Linux; newer nixpkgs clang adds warnings.
              "MESENFLAGS="
            ];

            # MESENFLAGS= above drops -m64/-O3 too; restore them.
            env.NIX_CFLAGS_COMPILE = toString (
              [ "-O3" ] ++ lib.optionals stdenv.hostPlatform.isx86_64 [ "-m64" ]
            );

            installPhase = ''
              runHook preInstall
              install -Dm755 InteropDLL/obj.*/MesenCore${stdenv.hostPlatform.extensions.sharedLibrary} \
                -t $out/lib
              runHook postInstall
            '';

            meta = {
              description = "MesenCE native emulation core";
              homepage = "https://github.com/nesdev-org/MesenCE";
              license = lib.licenses.gpl3Plus;
              platforms = systems;
            };
          };

          default = self.packages.${pkgs.stdenv.hostPlatform.system}.core;
        }
      );

      devShells = forAllSystems (
        pkgs:
        let
          inherit (pkgs) lib stdenv;
          dotnet = pkgs.dotnet-sdk_10;
          # Libraries the app loads at runtime (SDL2 via dlopen for audio; on Linux
          # also the X11/GL/font stack that Avalonia/Skia dlopen).
          runtimeLibs = [
            pkgs.SDL2
          ]
          ++ lib.optionals stdenv.hostPlatform.isLinux (
            with pkgs;
            [
              libGL
              libx11
              libice
              libsm
              libxcursor
              libxrandr
              libxi
              libxext
              fontconfig
              libevdev
            ]
          );
        in
        {
          default = pkgs.mkShell.override { stdenv = pkgs.clangStdenv; } {
            packages = [
              dotnet
              pkgs.gnumake
              pkgs.pkg-config
              pkgs.llvmPackages.clang-tools # clang-format, clangd
              pkgs.ccache
              pkgs.tlaplus # tlc / tlasany for docs/research/tla
              pkgs.nlohmann_json # tools/nes-cpu-singlestep
              pkgs.cc65 # ca65/ld65 for test ROMs under docs/research/tla/rom and tests/
              pkgs.wla-dx # wla-z80/wlalink for the SMS test ROMs under tests/SMS
            ]
            ++ runtimeLibs;

            DOTNET_ROOT = "${dotnet}/share/dotnet";

            # Directory holding the Nix SDL2 / runtime libs. Kept in a non-DYLD_ variable
            # because macOS SIP strips DYLD_* when going through /bin/sh; `mise run run`
            # re-exports it as DYLD_FALLBACK_LIBRARY_PATH / LD_LIBRARY_PATH right before exec.
            MESEN_RUNTIME_LIBRARY_PATH = lib.makeLibraryPath runtimeLibs;
          };
        }
      );

      formatter = forAllSystems (pkgs: pkgs.nixfmt);
    };
}

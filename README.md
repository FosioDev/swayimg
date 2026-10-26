## ⚠️ AI-Generated Code

This fork was developed entirely through an AI coding agent (pi coding agent, model `laguna-s-2.1`). The code has not been read by a human. Use at your own risk.

## What This Fork Does Differently

This fork includes optimizations and changes that differentiate it from the upstream swayimg:

### Memory Optimizations (Animated Images)

- **Animated AVIF streaming**: Instead of decoding all frames into memory at once (DecodeAll), frames are decoded on demand via `avifDecoderNextImage`. Only one active frame is kept in RAM at a time; previous frames are freed as new ones are decoded.
- **Animated WebP streaming**: Same approach for WebP using `WebPAnimDecoderGetNext`. The decoder is kept alive for on-demand frame decoding, with old frames freed from RAM.
- **Animated GIF streaming**: GIF frames are decoded on demand via the low-level gif_lib API (`DGifGetLine`), avoiding `DGifSlurp` which decompresses all frames into `SavedImages[].RasterBits` at once. A compositing canvas maintains the state between frames for correct disposal mode handling.

For example, viewing large animated files (339 AVIF + 171 WebP frames at 1320×1090):
- **Upstream swayimg**: ~3.7 GB RAM (all frames decoded and stored simultaneously)
- **This fork**: ~150 MB RAM (one active frame per image + decoder state)

### Removed Functionality

- **Video support removed**: The fork no longer supports video files. This includes removal of video-related code, settings, tests, and Lua config entries.
- **Animation control removed**: Frame-by-frame seeking and play/pause controls for animated images have been removed. Only automatic playback remains for animated images.

### Gallery Mode

- **Gallery::Aspect::Keep** is now the default aspect ratio for thumbnails, replacing `Gallery::Aspect::Fill`. Thumbnails preserve the original image aspect ratio instead of being cropped to squares.

## Installation on NixOS

1. Add the following to your system `flake.nix` inputs:
```nix
swayimg = {
  url = "github:fosiodev/swayimg";
  inputs.nixpkgs.follows = "nixpkgs";
};
```
2. Add `swayimg` to your `environment.systemPackages`:
```nix
environment.systemPackages = [
  inputs.swayimg.packages.${pkgs.stdenv.hostPlatform.system}.default
];
```

## Build on NixOS

```sh
nix develop
meson setup build -Dvideo=disabled && ninja -C build
./build/swayimg image1.png image2.avif
```

# Swayimg: image viewer for Wayland and DRM

Swayimg is a fully customizable image viewer with [Lua scripting](CONFIG.md) support.

- Support for the most popular image formats:
  - JPEG (via [libjpeg](https://libjpeg.sourceforge.net)),
  - JPEG XL (via [libjxl](https://github.com/libjxl/libjxl));
  - JPEG 2000 (via [OpenJPEG](https://www.openjpeg.org));
  - PNG (via [libpng](https://www.libpng.org));
  - GIF (via [giflib](https://giflib.sourceforge.net));
  - SVG (via [librsvg](https://gitlab.gnome.org/GNOME/librsvg));
  - WebP (via [libwebp](https://chromium.googlesource.com/webm/libwebp));
  - HEIF/HEIC (via [libheif](https://github.com/strukturag/libheif));
  - AV1F/AVIFS (via [libavif](https://github.com/AOMediaCodec/libavif));
  - TIFF (via [libtiff](https://libtiff.gitlab.io/libtiff));
  - Sixel (via [libsixel](https://github.com/saitoha/libsixel));
  - Raw: CRW/CR2, NEF, RAF, etc (via [libraw](https://www.libraw.org));
  - EXR (via [OpenEXR](https://openexr.com));
  - Krita (via [minizip-ng](https://github.com/zlib-ng/minizip-ng) and [libpng](https://www.libpng.org));
  - BMP (built-in);
  - PNM (built-in);
  - TGA (built-in);
  - XBM (built-in);
  - QOI (built-in);
  - DICOM (built-in);
  - Farbfeld (built-in).
- Support for non-image previews:
  - Font: TTF/OTF/WOFF (via [FreeType](https://freetype.org)).
- Native support for Wayland and DRM output;
- Gallery and viewer modes with slideshow;
- Image animation support;
- EXIF, XMP and IPTC tags support;
- Loading images from files, pipes and external programs;
- Preload images in a separate thread;
- Support for the Sway and Hyprland window managers, including an overlay
  feature that simulates displaying an image in the currently active window
  (e.g. a terminal);
- Fully customizable keyboard bindings, colors, and many other parameters via
  [Lua bindings](CONFIG.md).

![Viewer mode](.github/viewer.png)
![Gallery mode](.github/gallery.png)

## Usage

`swayimg [OPTIONS]... [FILE]...`

Examples:
- View multiple files:
  ```
  swayimg photo.jpg logo.png
  ```
- View using pipes:
  ```
  wget -qO- https://www.kernel.org/theme/images/logos/tux.png | swayimg -
  ```
- Loading stdout from external commands:
  ```
  swayimg "exec://wget -qO- https://www.kernel.org/theme/images/logos/tux.png" \
          "exec://curl -so- https://www.kernel.org/theme/images/logos/tux.png"
  ```

See full [documentation](USAGE.md) for details.

## Configuration

The Swayimg configuration file is a Lua script. The full list of available
functions is described in the [documentation](CONFIG.md) and in the
[Lua source](extra/swayimg.lua) file. Some tips and trick can be found [here](TIPS.md).

[Example](extra/example.lua) of the configuration file is available after
installation in `/usr/share/swayimg/example.lua`.

## Install

[![Packaging status](https://repology.org/badge/tiny-repos/swayimg.svg)](https://repology.org/project/swayimg/versions)

List of supported distributives can be found on the
[Repology page](https://repology.org/project/swayimg/versions).

Arch users can install the program from the [extra](https://archlinux.org/packages/extra/x86_64/swayimg)
repository or from [AUR](https://aur.archlinux.org/packages/swayimg-git) package.

## Build

The project uses Meson build system:
```
meson setup my_build_dir
meson compile -C my_build_dir
meson install -C my_build_dir
```

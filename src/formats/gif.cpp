// SPDX-License-Identifier: MIT
// GIF image format with streaming animation support.
// Copyright (C) 2020 Artem Senichev <artemsen()gmail.com>

#include "../imageformat.hpp"

#include <gif_lib.h>

#include <cassert>
#include <cstring>
#include <iterator>
#include <vector>

namespace {

class ImageFormatGif : public ImageFormat {
public:
    ImageFormatGif() noexcept
        : ImageFormat(Priority::Normal, "gif")
    {
    }

    /**
     * GIF image with frame streaming.
     *
     * Only one frame (the active one) is kept in memory at a time.
     * Frames are decoded on demand via the low-level gif_lib API
     * (DGifGetLine), avoiding the memory cost of DGifSlurp which
     * decompresses all frames into SavedImages[].RasterBits.
     */
    class ImageGif : public Image {
    public:
        ImageGif(const Data& data)
            : raw_data(data.data, data.data + data.size)
            , reader(raw_data)
        {
        }

        ~ImageGif() override
        {
            if (gif_file) {
                DGifCloseFile(gif_file, nullptr);
            }
        }

        bool open()
        {
            int err;
            gif_file = DGifOpen(&reader, &BufferReader::read, &err);
            if (!gif_file) {
                return false;
            }

            // DGifOpen already reads the screen descriptor
            if (gif_file->SWidth <= 0 || gif_file->SHeight <= 0) {
                return false;
            }

            if (!collect_metadata()) {
                DGifCloseFile(gif_file, nullptr);
                gif_file = nullptr;
                return false;
            }

            // Close and reopen for streaming decode
            DGifCloseFile(gif_file, nullptr);
            gif_file = nullptr;
            reader.position = 0;

            err = 0;
            gif_file = DGifOpen(&reader, &BufferReader::read, &err);
            if (!gif_file) {
                return false;
            }
            // DGifOpen already reads the screen descriptor

            canvas.create(Pixmap::ARGB, gif_file->SWidth,
                          gif_file->SHeight);

            return true;
        }

        bool decode_first_frame()
        {
            if (!decode_streaming_frame(0)) {
                return false;
            }
            active_frame = 0;
            return frames[0].pm.width() > 0;
        }

        void switch_frame(const size_t index) override
        {
            if (!gif_file || index >= frames.size()) {
                return;
            }

            if (active_frame < frames.size() && active_frame != index) {
                frames[active_frame].pm.free();
            }

            bool ok = false;

            if (index == active_frame + 1) {
                ok = decode_streaming_frame(index);
            } else {
                DGifCloseFile(gif_file, nullptr);
                gif_file = nullptr;
                reader.position = 0;

                int err;
                gif_file = DGifOpen(&reader, &BufferReader::read, &err);
                if (gif_file) {
                    ok = true;
                    for (size_t i = 0; i <= index; ++i) {
                        if (!decode_streaming_frame(i)) {
                            ok = false;
                            break;
                        }
                        // Free intermediate frames to save memory
                        if (i < index && i < frames.size()) {
                            frames[i].pm.free();
                        }
                    }
                }
            }

            if (ok) {
                apply_xform();
                active_frame = index;
            }
        }

        void set_orientation(const int orientation)
        {
            exif_orientation = orientation;
        }

        void flip_vertical() override
        {
            Image::flip_vertical();
            flip_vertical_flag = !flip_vertical_flag;
        }

        void flip_horizontal() override
        {
            Image::flip_horizontal();
            flip_horizontal_flag = !flip_horizontal_flag;
        }

        void rotate(const size_t angle) override
        {
            Image::rotate(angle);
            rotation = (rotation + angle) % 360;
        }

    private:
        /** Per-frame metadata collected during the initial scan. */
        struct FrameMeta {
            size_t duration = 100;     ///< Duration in ms
            int disposal_mode = 0;     ///< GIF disposal method
            int transparent_color = -1;///< Transparent color index (-1 = none)
            int left = 0;              ///< Frame position on canvas
            int top = 0;
            int width = 0;
            int height = 0;
        };

        struct BufferReader {
            BufferReader(const std::vector<uint8_t>& raw)
                : data(raw)
            {
            }

            static int read(GifFileType* gif, GifByteType* dst, int sz)
            {
                BufferReader* reader =
                    reinterpret_cast<BufferReader*>(gif->UserData);
                if (reader && sz >= 0 &&
                    reader->position + sz <= reader->data.size()) {
                    std::memcpy(dst,
                                reader->data.data() + reader->position, sz);
                    reader->position += sz;
                    return sz;
                }
                return -1;
            }

            const std::vector<uint8_t>& data;
            size_t position = 0;
        };

        /**
         * First pass: iterate through all records to collect frame
         * count, durations, disposal modes, and frame geometry.
         * Does not store decompressed pixel data.
         */
        bool collect_metadata()
        {
            size_t frame_index = 0;
            constexpr size_t max_frames = 65536;

            FrameMeta current_gcb; // GCB from the most recent extension

            while (frame_index < max_frames) {
                GifRecordType record_type;
                if (DGifGetRecordType(gif_file, &record_type) != GIF_OK) {
                    break;
                }

                if (record_type == TERMINATE_RECORD_TYPE) {
                    break;
                }

                if (record_type == EXTENSION_RECORD_TYPE) {
                    int ext_code;
                    GifByteType* ext_data;
                    if (DGifGetExtension(gif_file, &ext_code, &ext_data) ==
                        GIF_OK) {
                        // Check for Graphics Control Extension (0xF9)
                        if (ext_code == GRAPHICS_EXT_FUNC_CODE && ext_data) {
                            // ext_data[0] = block size (should be 4)
                            // ext_data[1] = packed: disposal(2-4), transp(0)
                            // ext_data[2..3] = delay time (little-endian)
                            // ext_data[4] = transparent color index
                            current_gcb.disposal_mode =
                                (ext_data[1] >> 2) & 0x07;
                            current_gcb.transparent_color =
                                (ext_data[1] & 0x01)
                                    ? static_cast<int>(ext_data[4])
                                    : NO_TRANSPARENT_COLOR;
                            current_gcb.duration =
                                static_cast<size_t>(
                                    static_cast<uint16_t>(ext_data[2]) |
                                    (static_cast<uint16_t>(ext_data[3]) << 8))
                                * 10;
                        }
                        do {
                            DGifGetExtensionNext(gif_file, &ext_data);
                        } while (ext_data != nullptr);
                    }
                    continue;
                }

                if (record_type == IMAGE_DESC_RECORD_TYPE) {
                    if (DGifGetImageDesc(gif_file) != GIF_OK) {
                        return false;
                    }

                    if (frame_index >= frame_metadata.size()) {
                        frame_metadata.resize(frame_index + 256);
                    }

                    FrameMeta& meta = frame_metadata[frame_index];
                    meta.left = gif_file->Image.Left;
                    meta.top = gif_file->Image.Top;
                    meta.width = gif_file->Image.Width;
                    meta.height = gif_file->Image.Height;
                    meta.duration = current_gcb.duration;
                    meta.disposal_mode = current_gcb.disposal_mode;
                    meta.transparent_color = current_gcb.transparent_color;

                    // Store duration in the frames vector for the viewer
                    if (frame_index >= frames.size()) {
                        frames.resize(frame_index + 1);
                    }
                    frames[frame_index].duration = current_gcb.duration;

                    // Skip raster data (read and discard)
                    const int row_size = meta.width > 0 ? meta.width : 1;
                    std::vector<GifPixelType> row(row_size);
                    for (int y = 0; y < gif_file->Image.Height; ++y) {
                        DGifGetLine(gif_file, row.data(), row_size);
                    }

                    ++frame_index;
                }
            }

            if (frame_index == 0) {
                return false;
            }

            frame_metadata.resize(frame_index);
            frames.resize(frame_index);
            return true;
        }

        /**
         * Decode a single frame using the low-level API.
         * @param index frame index to decode
         * @return true on success
         */
        bool decode_streaming_frame(const size_t index)
        {
            GifRecordType record_type;
            bool image_found = false;

            FrameMeta current_gcb; // GCB from extensions before this frame

            while (!image_found) {
                if (DGifGetRecordType(gif_file, &record_type) != GIF_OK) {
                    return false;
                }

                if (record_type == TERMINATE_RECORD_TYPE) {
                    return false;
                }

                if (record_type == EXTENSION_RECORD_TYPE) {
                    int ext_code;
                    GifByteType* ext_data;
                    if (DGifGetExtension(gif_file, &ext_code, &ext_data) ==
                        GIF_OK) {
                        if (ext_code == GRAPHICS_EXT_FUNC_CODE && ext_data) {
                            current_gcb.disposal_mode =
                                (ext_data[1] >> 2) & 0x07;
                            current_gcb.transparent_color =
                                (ext_data[1] & 0x01)
                                    ? static_cast<int>(ext_data[4])
                                    : NO_TRANSPARENT_COLOR;
                            current_gcb.duration =
                                static_cast<size_t>(
                                    static_cast<uint16_t>(ext_data[2]) |
                                    (static_cast<uint16_t>(ext_data[3]) << 8))
                                * 10;
                        }
                        do {
                            DGifGetExtensionNext(gif_file, &ext_data);
                        } while (ext_data != nullptr);
                    }
                    continue;
                }

                if (record_type == IMAGE_DESC_RECORD_TYPE) {
                    if (DGifGetImageDesc(gif_file) != GIF_OK) {
                        return false;
                    }
                    image_found = true;
                }
            }

            if (!image_found) {
                return false;
            }

            // Use stored metadata for disposal and transparency
            const FrameMeta& meta =
                index < frame_metadata.size() ? frame_metadata[index]
                                              : FrameMeta{};

            // Apply disposal mode from previous frame
            if (index > 0 && index - 1 < frame_metadata.size()) {
                const FrameMeta& prev_meta = frame_metadata[index - 1];

                if (prev_meta.disposal_mode == DISPOSE_BACKGROUND) {
                    const Rectangle clear_rect(
                        static_cast<ssize_t>(prev_meta.left),
                        static_cast<ssize_t>(prev_meta.top),
                        static_cast<size_t>(prev_meta.width),
                        static_cast<size_t>(prev_meta.height));
                    canvas.fill(clear_rect, argb_t { 0, 0, 0, 0 });
                }
                // DISPOSE_PREVIOUS and DISPOSE_DO_NOT: keep current canvas
            }

            const GifImageDesc& img_desc = gif_file->Image;
            const ColorMapObject* color_map =
                img_desc.ColorMap ? img_desc.ColorMap : gif_file->SColorMap;
            if (!color_map) {
                return false;
            }

            frame_row.resize(img_desc.Width > 0 ? img_desc.Width : 1);

            for (int y = 0; y < img_desc.Height; ++y) {
                DGifGetLine(gif_file, frame_row.data(), img_desc.Width);

                const int left = img_desc.Left;
                const int top = img_desc.Top;
                const int canvas_w = static_cast<int>(canvas.width());
                const int canvas_h = static_cast<int>(canvas.height());

                for (int x = 0; x < img_desc.Width; ++x) {
                    const int cx = left + x;
                    const int cy = top + y;
                    if (cx < 0 || cy < 0 ||
                        cx >= canvas_w || cy >= canvas_h) {
                        continue;
                    }

                    const GifPixelType color =
                        frame_row[static_cast<size_t>(x)];
                    const bool transparent =
                        (color == meta.transparent_color &&
                         meta.transparent_color != NO_TRANSPARENT_COLOR);
                    if (!transparent &&
                        color < color_map->ColorCount) {
                        const GifColorType* rgb =
                            &color_map->Colors[color];
                        argb_t& pixel = canvas.at(
                            static_cast<size_t>(cx),
                            static_cast<size_t>(cy));
                        pixel.a = argb_t::max;
                        pixel.r = rgb->Red;
                        pixel.g = rgb->Green;
                        pixel.b = rgb->Blue;
                    }
                }
            }

            copy_canvas_to_frame(index);
            return true;
        }

        void copy_canvas_to_frame(const size_t index)
        {
            if (index >= frames.size()) {
                return;
            }

            Pixmap& pm = frames[index].pm;
            pm.create(Pixmap::ARGB, canvas.width(), canvas.height());
            std::memcpy(pm.ptr(0, 0), canvas.ptr(0, 0),
                        pm.stride() * pm.height());
        }

        void apply_xform()
        {
            if (active_frame >= frames.size()) {
                return;
            }
            Pixmap& pm = frames[active_frame].pm;
            if (pm.width() == 0 || pm.height() == 0) {
                return;
            }

            if (exif_orientation > 0) {
                ImageFormat::fix_orientation(pm, exif_orientation);
            }
            if (rotation) {
                pm.rotate(rotation);
            }
            if (flip_horizontal_flag) {
                pm.flip_horizontal();
            }
            if (flip_vertical_flag) {
                pm.flip_vertical();
            }
        }

    private:
        std::vector<uint8_t> raw_data;
        BufferReader reader;
        GifFileType* gif_file = nullptr;
        Pixmap canvas;
        std::vector<GifPixelType> frame_row;
        std::vector<FrameMeta> frame_metadata;
        int exif_orientation = 0;
        bool flip_horizontal_flag = false;
        bool flip_vertical_flag = false;
        size_t rotation = 0;
        size_t active_frame = 0;
    };

    [[nodiscard]] ImagePtr decode(const Data& data) const override
    {
        if (!check_signature(data, { 'G', 'I', 'F' })) {
            return nullptr;
        }

        auto image = std::make_shared<ImageGif>(data);

        if (!image->open()) {
            return nullptr;
        }

        if (!image->decode_first_frame()) {
            return nullptr;
        }

        image->format = "GIF";
        if (image->frames.size() > 1) {
            image->format += " animation";
        }

        return image;
    }

    void fix_orientation(ImagePtr& image,
                         const int orientation = -1) const override
    {
        ImageFormat::fix_orientation(image, orientation);

        if (auto gif_img = std::dynamic_pointer_cast<ImageGif>(image)) {
            int exif_orient = orientation;
            if (exif_orient < 0) {
                const auto& it = image->meta.find("Exif.Image.Orientation");
                if (it != image->meta.end()) {
                    exif_orient =
                        std::strtol(it->second.c_str(), nullptr, 10);
                }
            }
            gif_img->set_orientation(exif_orient);
        }
    }
};

ImageFormatGif format_gif;

} // anonymous namespace

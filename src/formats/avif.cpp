// SPDX-License-Identifier: MIT
// AV1 (AVIF/AVIFS) image format.
// Copyright (C) 2023 Artem Senichev <artemsen@gmail.com>

#include "../imageformat.hpp"

#include <avif/avif.h>

#include <cstring>
#include <format>
#include <vector>

namespace {

class ImageFormatAvif : public ImageFormat {
public:
    ImageFormatAvif() noexcept
        : ImageFormat(Priority::High, "avif")
    {
    }

    /**
     * AVIF image with frame streaming.
     *
     * Only one frame (the active one) is kept in memory at a time.
     * Frames are decoded on demand via switch_frame().
     */
    class ImageAvif : public Image {
    public:
        ImageAvif(const Data& data)
            : raw_data(data.data, data.data + data.size)
        {
        }

        ~ImageAvif() override
        {
            if (decoder) {
                avifDecoderDestroy(decoder);
            }
        }

        /**
         * Open and parse the AVIF data stream.
         * @return true if parsing succeeded
         */
        bool open()
        {
            decoder = avifDecoderCreate();
            if (!decoder) {
                return false;
            }
            decoder->maxThreads = 1;
            const avifResult rc =
                avifDecoderSetIOMemory(decoder, raw_data.data(),
                                       raw_data.size());
            if (rc != AVIF_RESULT_OK) {
                return false;
            }
            const avifResult parse_rc = avifDecoderParse(decoder);
            if (parse_rc != AVIF_RESULT_OK) {
                return false;
            }
            return decoder->imageCount > 0;
        }

        /**
         * Decode frame 0 during initial load.
         * @return true if the first frame was decoded successfully
         */
        bool decode_first_frame()
        {
            // Reset decoder to clean state before first decode
            [[maybe_unused]] const avifResult reset_rc =
                avifDecoderReset(decoder);
            const avifResult rc = avifDecoderNextImage(decoder);
            if (rc != AVIF_RESULT_OK) {
                return false;
            }
            decode_frame(decoder, frames[0].pm);
            active_frame = 0;
            return frames[0].pm.width() > 0;
        }

        /**
         * Switch to a different frame: frees the previously decoded
         * frame from RAM and decodes the requested one.
         * @param index frame index to activate
         */
        void switch_frame(const size_t index) override
        {
            if (!decoder || index >= frames.size()) {
                return;
            }

            // Free the currently decoded frame to reclaim RAM
            if (active_frame < frames.size() && active_frame != index) {
                frames[active_frame].pm.free();
            }

            // Decode the requested frame. Use avifDecoderNextImage for
            // sequential playback (the common case for animation), and
            // fall back to avifDecoderNthImage for random access (e.g.
            // when looping back to the first frame).
            avifResult rc;
            if (index == active_frame + 1) {
                rc = avifDecoderNextImage(decoder);
            } else {
                rc = avifDecoderNthImage(decoder,
                                         static_cast<uint32_t>(index));
            }
            if (rc != AVIF_RESULT_OK) {
                return;
            }

            decode_frame(decoder, frames[index].pm);
            apply_xform();
            active_frame = index;
        }

        /**
         * Store EXIF orientation for application on future frames.
         * @param orientation EXIF orientation code (0 = none)
         */
        void set_orientation(const int orientation)
        {
            exif_orientation = orientation;
        }

        void flip_vertical() override
        {
            // Base class flips only the currently decoded frame (others are
            // empty and flip is a no-op on them)
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

        avifDecoder* decoder = nullptr; ///< AV1 decoder (owned)

    private:
        /**
         * Decode the current decoder image into a pixmap.
         * @param dec decoder instance (with image already decoded)
         * @param pm destination pixmap
         */
        static void decode_frame(avifDecoder* dec, Pixmap& pm)
        {
            avifRGBImage rgb;
            avifRGBImageSetDefaults(&rgb, dec->image);

            rgb.depth = 8;
            rgb.format = AVIF_RGB_FORMAT_BGRA;

            const avifResult rc = avifRGBImageAllocatePixels(&rgb);
            if (rc != AVIF_RESULT_OK) {
                return;
            }

            const avifResult convert_rc = avifImageYUVToRGB(dec->image, &rgb);
            if (convert_rc == AVIF_RESULT_OK) {
                pm.create(dec->alphaPresent ? Pixmap::ARGB : Pixmap::RGB,
                          rgb.width, rgb.height);
                std::memcpy(&pm.at(0, 0), rgb.pixels,
                            static_cast<size_t>(rgb.width) * rgb.height *
                                sizeof(argb_t));
            }

            avifRGBImageFreePixels(&rgb);
        }

        /**
         * Apply stored EXIF orientation and user-initiated transform
         * to the currently decoded frame.
         */
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
        std::vector<uint8_t> raw_data; ///< Copy of source data
        int exif_orientation = 0;      ///< EXIF orientation (0 = none)
        bool flip_horizontal_flag = false;
        bool flip_vertical_flag = false;
        size_t rotation = 0;           ///< Applied rotation in degrees
        size_t active_frame = 0;       ///< Index of currently decoded frame
    };

    [[nodiscard]] ImagePtr decode(const Data& data) const override
    {
        if (!check_signature(data, { 'f', 't', 'y', 'p' }, 4)) {
            return nullptr;
        }

        // create image with streaming support
        auto image = std::make_shared<ImageAvif>(data);

        if (!image->open()) {
            return nullptr;
        }

        // store frame count and durations
        const uint32_t frame_count = image->decoder->imageCount;
        image->frames.resize(frame_count);

        // timing.duration is already in seconds; convert to milliseconds.
        // (timing.durationInTimescales / timing.timescale == timing.duration)
        for (size_t i = 0; i < frame_count; ++i) {
            avifImageTiming timing;
            const avifResult rc =
                avifDecoderNthImageTiming(image->decoder,
                                          static_cast<uint32_t>(i), &timing);
            if (rc != AVIF_RESULT_OK) {
                image->frames[i].duration = 100; // fallback
            } else {
                image->frames[i].duration =
                    static_cast<size_t>(timing.duration * 1000.0);
            }
        }

        // decode only the first frame
        if (!image->decode_first_frame()) {
            return nullptr;
        }

        image->format =
            std::format("AV1 {}bpc {}", image->decoder->image->depth,
                        avifPixelFormatToString(image->decoder->image->yuvFormat));

        return image;
    }

    void fix_orientation(ImagePtr& image,
                         const int orientation = -1) const override
    {
        // Let the base class resolve the EXIF orientation and apply it
        // to the currently decoded frame.
        ImageFormat::fix_orientation(image, orientation);

        // Store the orientation in the streaming image so that future
        // frames (decoded on demand) also get the correct orientation.
        if (auto avif_img = std::dynamic_pointer_cast<ImageAvif>(image)) {
            int exif_orient = orientation;
            if (exif_orient < 0) {
                const auto& it = image->meta.find("Exif.Image.Orientation");
                if (it != image->meta.end()) {
                    exif_orient =
                        std::strtol(it->second.c_str(), nullptr, 10);
                }
            }
            avif_img->set_orientation(exif_orient);
        }
    }
};

// register format in factory
ImageFormatAvif format_avif;

} // anonymous namespace

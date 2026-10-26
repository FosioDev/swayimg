// SPDX-License-Identifier: MIT
// WebP image format.
// Copyright (C) 2020 Artem Senichev <artemsen@gmail.com>

#include "../imageformat.hpp"

#include <webp/demux.h>

#include <cstring>
#include <vector>

namespace {

class ImageFormatWebp : public ImageFormat {
public:
    ImageFormatWebp() noexcept
        : ImageFormat(Priority::High, "webp")
    {
    }

    /**
     * WebP image with frame streaming.
     *
     * Only one frame (the active one) is kept in memory at a time.
     * Frames are decoded on demand via switch_frame().
     */
    class ImageWebp : public Image {
    public:
        ImageWebp(const Data& data)
            : raw_data(data.data, data.data + data.size)
        {
        }

        ~ImageWebp() override
        {
            if (decoder) {
                WebPAnimDecoderDelete(decoder);
            }
        }

        /**
         * Open and parse the WebP data stream.
         * @return true if parsing succeeded
         */
        bool open()
        {
            // get image properties (header only, no pixel decoding)
            WebPBitstreamFeatures webp_prop;
            if (WebPGetFeatures(raw_data.data(), raw_data.size(),
                                &webp_prop) != VP8_STATUS_OK) {
                return false;
            }
            canvas_width = webp_prop.width;
            canvas_height = webp_prop.height;
            has_alpha = webp_prop.has_alpha;
            webp_format = webp_prop.format;
            has_animation = webp_prop.has_animation;

            // setup decoder
            WebPAnimDecoderOptions webp_opts;
            WebPAnimDecoderOptionsInit(&webp_opts);
            webp_opts.color_mode = MODE_BGRA;
            webp_opts.use_threads = false;

            const WebPData webp_data = {
                .bytes = raw_data.data(), .size = raw_data.size()
            };
            decoder = WebPAnimDecoderNew(&webp_data, &webp_opts);
            if (!decoder) {
                return false;
            }

            WebPAnimInfo webp_info;
            if (!WebPAnimDecoderGetInfo(decoder, &webp_info)) {
                return false;
            }

            if (webp_info.frame_count == 0) {
                return false;
            }

            // use canvas dimensions from the animation info if the static
            // features report 0 (happens for some animated-only files)
            if (canvas_width == 0 && webp_info.canvas_width > 0) {
                canvas_width = webp_info.canvas_width;
            }
            if (canvas_height == 0 && webp_info.canvas_height > 0) {
                canvas_height = webp_info.canvas_height;
            }

            return true;
        }

        /**
         * Decode frame 0 during initial load.
         * @return true if the first frame was decoded successfully
         */
        bool decode_first_frame()
        {
            uint8_t* buffer = nullptr;
            int timestamp = 0;
            if (!WebPAnimDecoderGetNext(decoder, &buffer, &timestamp)) {
                return false;
            }
            copy_to_frame(buffer, 0);
            active_frame = 0;
            return frames[0].pm.width() > 0;
        }

        /**
         * Populate frame durations from the demuxer (metadata only,
         * no pixel decoding).
         */
        void populate_durations()
        {
            const WebPDemuxer* demuxer =
                WebPAnimDecoderGetDemuxer(decoder);
            if (!demuxer) {
                return;
            }

            WebPIterator iter;
            if (!WebPDemuxGetFrame(demuxer, 1, &iter)) {
                return;
            }

            size_t index = 0;
            do {
                if (index < frames.size()) {
                    frames[index].duration =
                        static_cast<size_t>(iter.duration);
                }
                ++index;
            } while (WebPDemuxNextFrame(&iter) &&
                     index < frames.size());

            WebPDemuxReleaseIterator(&iter);
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

            bool ok = false;

            if (index == active_frame + 1) {
                // Sequential forward playback: use WebPAnimDecoderGetNext
                uint8_t* buffer = nullptr;
                int timestamp = 0;
                if (WebPAnimDecoderGetNext(decoder, &buffer, &timestamp)) {
                    copy_to_frame(buffer, index);
                    ok = true;
                }
            } else {
                // Random access (e.g. loop-back or seek): reset and
                // decode sequentially up to the desired frame
                WebPAnimDecoderReset(decoder);
                for (size_t i = 0; i <= index; ++i) {
                    uint8_t* buffer = nullptr;
                    int timestamp = 0;
                    if (!WebPAnimDecoderGetNext(decoder, &buffer,
                                                 &timestamp)) {
                        break;
                    }
                    if (i == index) {
                        copy_to_frame(buffer, index);
                        ok = true;
                    }
                }
            }

            if (ok) {
                apply_xform();
                active_frame = index;
            }
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

        WebPAnimDecoder* decoder = nullptr; ///< WebP animation decoder (owned)

        // Public for access from outer class decode()
        int webp_format = 0;      ///< 0 = undefined, 1 = lossy, 2 = lossless
        bool has_alpha = false;   ///< Alpha channel present
        bool has_animation = false; ///< Animation flag

    private:
        /**
         * Copy decoded frame buffer into the pixmap.
         * @param buffer buffer returned by WebPAnimDecoderGetNext
         * @param index frame index to write to
         */
        void copy_to_frame(uint8_t* buffer, const size_t index)
        {
            Pixmap& pm = frames[index].pm;
            pm.create(has_alpha ? Pixmap::ARGB : Pixmap::RGB,
                      canvas_width, canvas_height);
            std::memcpy(pm.ptr(0, 0), buffer,
                        pm.stride() * pm.height());
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
        uint32_t canvas_width = 0;     ///< Canvas width
        uint32_t canvas_height = 0;    ///< Canvas height
        int exif_orientation = 0;      ///< EXIF orientation (0 = none)
        bool flip_horizontal_flag = false;
        bool flip_vertical_flag = false;
        size_t rotation = 0;           ///< Applied rotation in degrees
        size_t active_frame = 0;       ///< Index of currently decoded frame
    };

    [[nodiscard]] ImagePtr decode(const Data& data) const override
    {
        if (!check_signature(data, { 'R', 'I', 'F', 'F' })) {
            return nullptr;
        }

        // create image with streaming support
        auto image = std::make_shared<ImageWebp>(data);

        if (!image->open()) {
            return nullptr;
        }

        // store frame count
        const WebPAnimInfo info = [&] {
            WebPAnimInfo i;
            WebPAnimDecoderGetInfo(image->decoder, &i);
            return i;
        }();
        image->frames.resize(info.frame_count);

        // populate frame durations (metadata only, no pixel decoding)
        image->populate_durations();

        // decode only the first frame
        if (!image->decode_first_frame()) {
            return nullptr;
        }

        // set format description
        image->format = "WebP";
        if (image->webp_format == 1) {
            image->format += " lossy";
        } else if (image->webp_format == 2) {
            image->format += " lossless";
        }
        if (image->has_alpha) {
            image->format += ", alpha";
        }
        if (image->has_animation) {
            image->format += ", animation";
        }

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
        if (auto webp_img = std::dynamic_pointer_cast<ImageWebp>(image)) {
            int exif_orient = orientation;
            if (exif_orient < 0) {
                const auto& it = image->meta.find("Exif.Image.Orientation");
                if (it != image->meta.end()) {
                    exif_orient =
                        std::strtol(it->second.c_str(), nullptr, 10);
                }
            }
            webp_img->set_orientation(exif_orient);
        }
    }
};

// register format in factory
ImageFormatWebp format_webp;

} // anonymous namespace

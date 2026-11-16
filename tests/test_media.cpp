#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <marching_cubes/media.hpp>

using namespace marching_cubes;
namespace fs = std::filesystem;

namespace {
void require(bool condition, char const *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct Temporary {
    fs::path path;
    Temporary() {
        path = fs::temp_directory_path() /
               ("mc_media_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(fs::create_directory(path), "create temporary test directory");
    }
    ~Temporary() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
} // namespace

int main() {
    try {
        constexpr size_t w = 64, h = 64;
        std::vector<unsigned char> scene(w * h * 3, 0);
        for (size_t i = 0; i < w * h; ++i) {
            scene[3 * i] = 30;
            scene[3 * i + 1] = 90;
            scene[3 * i + 2] = 180;
        }
        RenderOptions render;
        render.width = w;
        render.height = h;
        render.camera.scale = 2;
        nlohmann::json const frame = {
            {"time", 5.0},
            {"surfaces", nlohmann::json::array({{{"fraction", 0.1},
                                                 {"isovalue", 2.5},
                                                 {"opacity", 0.4},
                                                 {"color", {0.1, 0.2, 0.8}}},
                                                {{"fraction", 0.01},
                                                 {"isovalue", 0.25},
                                                 {"opacity", 0.1},
                                                 {"color", {0.8, 0.1, 0.2}}}})}};
        AnnotationConfig config;
        config.initial_period_seconds = 10;
        config.time_unit_seconds = 120;
        config.length_unit_cm = 1e8;
        config.density_unit_g_cm3 = 10;
        config.ruler_length_cm = 1e8;
        auto const dimensions = annotated_dimensions(w, h);
        AnnotatedImage const image = annotate(scene, w, h, render, frame, config);
        require(image.width == dimensions[0] && image.height == dimensions[1] &&
                    image.width % 2 == 0 && image.height % 2 == 0,
                "preflight annotated dimensions");
        require(image.rgb == annotate(scene, w, h, render, frame, config).rgb,
                "annotations must be deterministic");
        auto const &metadata = image.metadata;
        require(metadata["time_seconds"] == 600 && metadata["time_minutes"] == 10 &&
                    metadata["time_over_initial_period"] == 60,
                "time and orbital unit conversions");
        require(metadata["ruler_pixels"] == 16 && metadata["ruler_exact_pixels"] == 16 &&
                    metadata["ruler_cm"] == 1e8,
                "ruler geometry and length conversion");
        require(std::abs(metadata["ruler_solar_radii"].get<double>() - 1e8 / 6.957e10) < 1e-15,
                "solar radius conversion");
        require(metadata["surfaces"][0]["density_g_cm3"] == 25 &&
                    metadata["surfaces"][1]["density_g_cm3"] == 2.5,
                "density unit conversions");
        size_t const x = metadata["scene_x"], y = metadata["scene_y"];
        size_t const offset = 3 * ((y + 10) * image.width + x + 10);
        require(image.rgb[offset] == 30 && image.rgb[offset + 1] == 90 &&
                    image.rgb[offset + 2] == 180,
                "annotations must preserve the scene RGB pixels");
        size_t const key_x = x + w + 23 + 9, key_y = 144 - 12 + 9;
        size_t const key_offset = 3 * (key_y * image.width + key_x);
        require(image.rgb[key_offset] == 26 && image.rgb[key_offset + 1] == 51 &&
                    image.rgb[key_offset + 2] == 204,
                "legend swatch must retain RGB channel order");
        AnnotationConfig auto_config = config;
        auto_config.ruler_length_cm = 0;
        auto const automatic = annotate(scene, w, h, render, frame, auto_config);
        require(automatic.metadata["ruler_pixels"].get<int>() >= 10 &&
                    automatic.metadata["ruler_pixels"].get<int>() <= 32,
                "automatic ruler fits scene");
        bool rejected = false;
        try {
            AnnotationConfig bad = config;
            bad.initial_period_seconds = 0;
            validate_annotation_config(bad);
        } catch (std::invalid_argument const &) {
            rejected = true;
        }
        require(rejected, "invalid initial period must be rejected");
        validate_annotation_layout(render, config);
        rejected = false;
        try {
            AnnotationConfig oversized = config;
            oversized.ruler_length_cm = 1e300;
            validate_annotation_layout(render, oversized);
        } catch (std::invalid_argument const &) {
            rejected = true;
        }
        require(rejected, "oversized physical ruler must fail preflight");
        rejected = false;
        try {
            VideoOptions bad_rate;
            bad_rate.fps = 1e-6;
            validate_video_options(bad_rate);
        } catch (std::invalid_argument const &) {
            rejected = true;
        }
        require(rejected, "unrepresentable frame rate must fail preflight");
        rejected = false;
        try {
            auto bad = frame;
            bad["time"] = nullptr;
            annotate(scene, w, h, render, bad, config);
        } catch (std::invalid_argument const &) {
            rejected = true;
        }
        require(rejected, "unknown time must be rejected");
        Temporary temporary;
        fs::path const first = temporary.path / "frame_0.png",
                       second = temporary.path / "frame_1.png";
        write_png(image, first.string());
        write_png(image, second.string());
        AnnotatedImage const read = read_png(first.string());
        require(read.width == image.width && read.height == image.height && read.rgb == image.rgb,
                "PNG RGB round trip");
        if (!avcodec_find_encoder_by_name("libx264")) {
            std::cout << "SKIP encoding: libx264 not available\n";
        } else {
            fs::path const path = temporary.path / "test.mp4";
            VideoOptions video;
            video.fps = 4;
            video.bitrate = 1000000;
            size_t progress = 0;
            video.progress = [&](size_t done, size_t total) {
                require(total == 2, "video frame total");
                progress = done;
            };
            encode_mp4({first.string(), second.string()}, path.string(), video);
            require(progress == 2 && fs::file_size(path) > 100, "MP4 creation and progress");
            AVFormatContext *input = nullptr;
            require(avformat_open_input(&input, path.string().c_str(), nullptr, nullptr) >= 0,
                    "open MP4");
            require(avformat_find_stream_info(input, nullptr) >= 0, "read MP4 stream info");
            int const stream = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            require(stream >= 0 &&
                        input->streams[stream]->codecpar->width == static_cast<int>(image.width) &&
                        input->streams[stream]->codecpar->height == static_cast<int>(image.height),
                    "MP4 stream dimensions");
            AVPacket *packet = av_packet_alloc();
            require(packet != nullptr, "allocate MP4 packet");
            int packets = 0;
            while (av_read_frame(input, packet) >= 0) {
                if (packet->stream_index == stream) {
                    ++packets;
                }
                av_packet_unref(packet);
            }
            av_packet_free(&packet);
            avformat_close_input(&input);
            require(packets == 2, "encoded MP4 must contain both frames");
            rejected = false;
            try {
                encode_mp4({first.string()}, path.string(), video);
            } catch (std::runtime_error const &) {
                rejected = true;
            }
            require(rejected, "encoder must not overwrite existing output");
            rejected = false;
            try {
                video.cancelled = [] { return true; };
                encode_mp4({first.string()}, (temporary.path / "cancelled.mp4").string(), video);
            } catch (std::runtime_error const &) {
                rejected = true;
            }
            require(rejected, "cancelled encoding must report failure");
        }
        std::cout << "media tests passed\n";
        return 0;
    } catch (std::exception const &e) {
        std::cerr << "media test failed: " << e.what() << '\n';
        return 1;
    }
}

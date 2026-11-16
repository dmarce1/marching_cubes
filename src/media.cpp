#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <marching_cubes/media.hpp>

namespace marching_cubes {
namespace {

constexpr size_t left_margin = 24;
constexpr size_t top_margin = 70;
constexpr size_t right_margin = 320;
constexpr size_t bottom_margin = 90;

double positive(double value, char const *name) {
    if (!std::isfinite(value) || value <= 0) {
        throw std::invalid_argument(std::string(name) + " must be finite and positive");
    }
    return value;
}

double product(double a, double b, char const *name) {
    long double const v = static_cast<long double>(a) * b;
    if (!std::isfinite(v) || std::abs(v) > std::numeric_limits<double>::max()) {
        throw std::invalid_argument(std::string(name) + " exceeds numeric range");
    }
    return static_cast<double>(v);
}

std::string scientific(double x, int precision = 3) {
    std::ostringstream out;
    out << std::scientific << std::setprecision(precision) << x;
    return out.str();
}

std::string number(double x, int precision = 4) {
    std::ostringstream out;
    out << std::setprecision(precision) << x;
    return out.str();
}

double nice_length(double target) {
    positive(target, "automatic ruler length");
    double const exponent = std::floor(std::log10(target));
    double const base = std::pow(10.0, exponent);
    if (!std::isfinite(base) || base == 0) {
        return target;
    }
    double best = base;
    for (double factor : {1.0, 2.0, 5.0, 10.0}) {
        double const candidate = factor * base;
        if (std::isfinite(candidate) && std::abs(candidate - target) < std::abs(best - target)) {
            best = candidate;
        }
    }
    return best;
}

struct RulerLayout {
    double scene_width_cm;
    double ruler_cm;
    double exact_pixels;
    int pixels;
};

RulerLayout ruler_layout(RenderOptions const &render, AnnotationConfig const &config) {
    validate_annotation_config(config);
    annotated_dimensions(render.width, render.height);
    positive(render.camera.scale, "camera scale");
    double const scene_width_cm =
        product(product(2.0, render.camera.scale, "scene width"),
                product(config.length_unit_cm,
                        static_cast<double>(render.width) / static_cast<double>(render.height),
                        "scene width"),
                "scene width");
    positive(scene_width_cm, "scene width");
    double const ruler =
        config.ruler_length_cm == 0 ? nice_length(scene_width_cm * 0.3) : config.ruler_length_cm;
    positive(ruler, "ruler length");
    double const exact_pixels = ruler / scene_width_cm * static_cast<double>(render.width);
    if (!std::isfinite(exact_pixels) || exact_pixels < 1 ||
        exact_pixels > static_cast<double>(render.width) - 8 ||
        !std::isfinite(ruler / config.solar_radius_cm)) {
        throw std::invalid_argument("ruler length does not fit in the scene or physical scale");
    }
    return {scene_width_cm, ruler, exact_pixels, static_cast<int>(std::lround(exact_pixels))};
}

std::string av_error(int code) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, message, sizeof message);
    return message;
}

void av_check(int code, std::string const &what) {
    if (code < 0) {
        throw std::runtime_error(what + ": " + av_error(code));
    }
}

struct FormatOwner {
    AVFormatContext *format = nullptr;
    ~FormatOwner() {
        if (format) {
            if (format->pb) {
                avio_closep(&format->pb);
            }
            avformat_free_context(format);
        }
    }
};
struct CodecDeleter {
    void operator()(AVCodecContext *p) const { avcodec_free_context(&p); }
};
struct FrameDeleter {
    void operator()(AVFrame *p) const { av_frame_free(&p); }
};
struct PacketDeleter {
    void operator()(AVPacket *p) const { av_packet_free(&p); }
};
struct SwsDeleter {
    void operator()(SwsContext *p) const { sws_freeContext(p); }
};

void put_text(cv::Mat &canvas, std::string const &text, int x, int y, double scale,
              cv::Scalar color, int max_width) {
    int baseline = 0;
    int const measured = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, scale, 1, &baseline).width;
    if (measured > max_width && measured > 0) {
        scale *= static_cast<double>(max_width) / measured;
    }
    cv::putText(canvas, text, {x, y}, cv::FONT_HERSHEY_SIMPLEX, scale, color, 1, cv::LINE_AA);
}

} // namespace

std::array<size_t, 2> annotated_dimensions(size_t w, size_t h) {
    size_t const limit = static_cast<size_t>(std::numeric_limits<int>::max());
    if (w < 16 || h < 16 || w > limit - left_margin - right_margin - 1 ||
        h > limit - top_margin - bottom_margin - 1) {
        throw std::invalid_argument("invalid scene dimensions for annotations");
    }
    size_t width = left_margin + w + right_margin;
    size_t height = std::max<size_t>(400, top_margin + h + bottom_margin);
    width += width % 2;
    height += height % 2;
    if (width > std::numeric_limits<size_t>::max() / height ||
        width * height > std::numeric_limits<size_t>::max() / 3) {
        throw std::invalid_argument("annotated image dimensions overflow");
    }
    return {width, height};
}

void validate_annotation_config(AnnotationConfig const &c) {
    positive(c.initial_period_seconds, "initial period");
    positive(c.time_unit_seconds, "time unit");
    positive(c.length_unit_cm, "length unit");
    positive(c.density_unit_g_cm3, "density unit");
    positive(c.solar_radius_cm, "solar radius");
    if (!std::isfinite(c.ruler_length_cm) || c.ruler_length_cm < 0) {
        throw std::invalid_argument("ruler length must be finite and nonnegative");
    }
    if (c.title.find_first_of("\r\n") != std::string::npos) {
        throw std::invalid_argument("title must fit on one line");
    }
}

void validate_annotation_layout(RenderOptions const &render, AnnotationConfig const &config) {
    (void)ruler_layout(render, config);
}

AnnotatedImage annotate(std::vector<unsigned char> const &rgb, size_t w, size_t h,
                        RenderOptions const &render, nlohmann::json const &frame,
                        AnnotationConfig const &config) {
    validate_annotation_config(config);
    auto const dimensions = annotated_dimensions(w, h);
    if (w > std::numeric_limits<size_t>::max() / h ||
        w * h > std::numeric_limits<size_t>::max() / 3 || rgb.size() != w * h * 3 ||
        w != render.width || h != render.height) {
        throw std::invalid_argument("scene pixel dimensions do not match render options");
    }
    positive(render.camera.scale, "camera scale");
    if (!frame.contains("time") || !frame["time"].is_number()) {
        throw std::invalid_argument("annotation requires a known finite frame time");
    }
    double const raw_time = frame.at("time").get<double>();
    if (!std::isfinite(raw_time)) {
        throw std::invalid_argument("frame time is nonfinite");
    }
    double const time_seconds = product(raw_time, config.time_unit_seconds, "time");
    double const phase = time_seconds / config.initial_period_seconds;
    if (!std::isfinite(phase)) {
        throw std::invalid_argument("time in orbits exceeds numeric range");
    }
    auto const &surfaces = frame.at("surfaces");
    if (!surfaces.is_array() || surfaces.empty() || surfaces.size() > 4) {
        throw std::invalid_argument("annotations require one to four isosurfaces");
    }
    RulerLayout const layout = ruler_layout(render, config);
    double const scene_width_cm = layout.scene_width_cm;
    double const ruler = layout.ruler_cm;
    double const exact_pixels = layout.exact_pixels;
    int const ruler_pixels = layout.pixels;
    cv::setNumThreads(1);
    cv::Mat canvas(static_cast<int>(dimensions[1]), static_cast<int>(dimensions[0]), CV_8UC3,
                   cv::Scalar(5, 7, 13));
    cv::Mat scene(static_cast<int>(h), static_cast<int>(w), CV_8UC3,
                  const_cast<unsigned char *>(rgb.data()));
    scene.copyTo(canvas(cv::Rect(static_cast<int>(left_margin), static_cast<int>(top_margin),
                                 static_cast<int>(w), static_cast<int>(h))));
    cv::Scalar const white(235, 239, 245), muted(160, 174, 190), accent(113, 180, 226);
    std::string const title = config.title.empty() ? "DENSITY EVOLUTION" : config.title;
    put_text(canvas, title, 24, 27, 0.62, white, static_cast<int>(dimensions[0] / 2 - 30));
    std::string const timing =
        "t / P0 = " + number(phase, 6) + "     " + number(time_seconds / 60.0, 6) + " min";
    put_text(canvas, timing, 24, 53, 0.50, accent, static_cast<int>(dimensions[0] - 48));
    int const key_x = static_cast<int>(left_margin + w + 23);
    put_text(canvas, "ISODENSITY SURFACES", key_x, 90, 0.49, white, 270);
    put_text(canvas, "relative to initial reference", key_x, 110, 0.37, muted, 270);
    nlohmann::json keys = nlohmann::json::array();
    for (size_t i = 0; i < surfaces.size(); ++i) {
        auto const &surface = surfaces[i];
        double const iso = surface.at("isovalue").get<double>();
        double const fraction = surface.at("fraction").get<double>();
        double const opacity = surface.at("opacity").get<double>();
        auto const color = surface.at("color").get<std::array<double, 3>>();
        if (!std::isfinite(iso) || iso < 0 || !std::isfinite(fraction) || fraction <= 0 ||
            !std::isfinite(opacity) || opacity < 0 || opacity > 1 ||
            std::any_of(color.begin(), color.end(),
                        [](double c) { return !std::isfinite(c) || c < 0 || c > 1; })) {
            throw std::invalid_argument("invalid isosurface annotation metadata");
        }
        double const density = product(iso, config.density_unit_g_cm3, "isodensity");
        int const y = 144 + static_cast<int>(i) * 61;
        cv::Scalar const swatch(std::lround(color[0] * 255), std::lround(color[1] * 255),
                                std::lround(color[2] * 255));
        cv::rectangle(canvas, cv::Rect(key_x, y - 12, 19, 19), swatch, cv::FILLED, cv::LINE_8);
        std::string const state =
            surface.contains("triangles") && surface.at("triangles").get<size_t>() == 0
                ? " (absent)"
                : "";
        put_text(canvas, number(fraction, 4) + " x reference" + state, key_x + 28, y + 2, 0.43,
                 white, 235);
        put_text(canvas, scientific(density, 3) + " g/cm^3  a=" + number(opacity, 2), key_x + 28,
                 y + 22, 0.39, muted, 235);
        keys.push_back({{"fraction", fraction},
                        {"density_g_cm3", density},
                        {"opacity", opacity},
                        {"color", color}});
    }
    int const x0 = static_cast<int>(left_margin + 4), x1 = x0 + ruler_pixels;
    int const ybar = static_cast<int>(top_margin + h + 27);
    cv::line(canvas, {x0, ybar}, {x1, ybar}, white, 2, cv::LINE_AA);
    cv::line(canvas, {x0, ybar - 5}, {x0, ybar + 5}, white, 2, cv::LINE_AA);
    cv::line(canvas, {x1, ybar - 5}, {x1, ybar + 5}, white, 2, cv::LINE_AA);
    std::string const barlabel =
        scientific(ruler, 3) + " cm   =   " + number(ruler / config.solar_radius_cm, 5) + " R_sun";
    put_text(canvas, barlabel, x0, ybar + 26, 0.43, muted,
             static_cast<int>(dimensions[0] - left_margin - 30));
    AnnotatedImage result;
    result.width = dimensions[0];
    result.height = dimensions[1];
    result.rgb.assign(canvas.data, canvas.data + dimensions[0] * dimensions[1] * 3);
    result.metadata = {{"scene_x", left_margin},
                       {"scene_y", top_margin},
                       {"scene_width", w},
                       {"scene_height", h},
                       {"width", dimensions[0]},
                       {"height", dimensions[1]},
                       {"time_seconds", time_seconds},
                       {"time_minutes", time_seconds / 60.0},
                       {"time_over_initial_period", phase},
                       {"time_unit_seconds", config.time_unit_seconds},
                       {"initial_period_seconds", config.initial_period_seconds},
                       {"length_unit_cm", config.length_unit_cm},
                       {"density_unit_g_cm3", config.density_unit_g_cm3},
                       {"solar_radius_cm", config.solar_radius_cm},
                       {"ruler_cm", ruler},
                       {"ruler_solar_radii", ruler / config.solar_radius_cm},
                       {"ruler_exact_pixels", exact_pixels},
                       {"ruler_pixels", ruler_pixels},
                       {"ruler_rendered_cm", ruler_pixels * (scene_width_cm / w)},
                       {"ruler_x", x0},
                       {"ruler_y", ybar},
                       {"title", title},
                       {"surfaces", keys}};
    return result;
}

void write_png(AnnotatedImage const &image, std::string const &path) {
    if (path.empty() || image.width == 0 || image.height == 0 ||
        image.width > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        image.height > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        image.width > std::numeric_limits<size_t>::max() / image.height ||
        image.width * image.height > std::numeric_limits<size_t>::max() / 3 ||
        image.rgb.size() != image.width * image.height * 3) {
        throw std::invalid_argument("invalid PNG image or path");
    }
    cv::setNumThreads(1);
    cv::Mat rgb(static_cast<int>(image.height), static_cast<int>(image.width), CV_8UC3,
                const_cast<unsigned char *>(image.rgb.data()));
    cv::Mat bgr;
    cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
    if (!cv::imwrite(path, bgr)) {
        throw std::runtime_error("failed writing PNG: " + path);
    }
}

AnnotatedImage read_png(std::string const &path) {
    cv::setNumThreads(1);
    cv::Mat bgr = cv::imread(path, cv::IMREAD_COLOR);
    if (bgr.empty()) {
        throw std::runtime_error("failed reading PNG: " + path);
    }
    cv::Mat rgb;
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    AnnotatedImage result;
    result.width = static_cast<size_t>(rgb.cols);
    result.height = static_cast<size_t>(rgb.rows);
    result.rgb.assign(rgb.data, rgb.data + result.width * result.height * 3);
    return result;
}

void validate_video_options(VideoOptions const &options) {
    if (!std::isfinite(options.fps) || options.fps <= 0 || options.fps > 1000 ||
        options.bitrate <= 0 || options.bitrate > std::numeric_limits<int>::max() / 2) {
        throw std::invalid_argument("invalid MP4 frame rate or bitrate");
    }
    AVRational const rate = av_d2q(options.fps, 100000);
    if (rate.num <= 0 || rate.den <= 0) {
        throw std::invalid_argument("frame rate cannot be represented");
    }
    if (!avcodec_find_encoder_by_name("libx264")) {
        throw std::runtime_error("FFmpeg libx264 encoder is unavailable");
    }
}

void encode_mp4(std::vector<std::string> const &png_files, std::string const &output,
                VideoOptions const &options) {
    validate_video_options(options);
    if (png_files.empty() || output.empty()) {
        throw std::invalid_argument("invalid MP4 sequence or output");
    }
    if (std::filesystem::exists(output)) {
        throw std::runtime_error("MP4 output already exists: " + output);
    }
    cv::setNumThreads(1);
    cv::Mat first = cv::imread(png_files.front(), cv::IMREAD_COLOR);
    if (first.empty()) {
        throw std::runtime_error("failed reading video frame: " + png_files.front());
    }
    if (first.cols % 2 || first.rows % 2) {
        throw std::invalid_argument("MP4 frame width and height must be even");
    }
    AVCodec const *encoder = avcodec_find_encoder_by_name("libx264");
    if (!encoder) {
        throw std::runtime_error("FFmpeg libx264 encoder is unavailable");
    }
    FormatOwner output_format;
    av_check(avformat_alloc_output_context2(&output_format.format, nullptr, "mp4", output.c_str()),
             "allocate MP4 muxer");
    if (!output_format.format) {
        throw std::runtime_error("MP4 muxer unavailable");
    }
    std::unique_ptr<AVCodecContext, CodecDeleter> codec(avcodec_alloc_context3(encoder));
    if (!codec) {
        throw std::runtime_error("allocate video encoder");
    }
    AVRational const rate = av_d2q(options.fps, 100000);
    if (rate.num <= 0 || rate.den <= 0) {
        throw std::invalid_argument("frame rate cannot be represented");
    }
    codec->width = first.cols;
    codec->height = first.rows;
    codec->pix_fmt = AV_PIX_FMT_YUV420P;
    codec->framerate = rate;
    codec->time_base = av_inv_q(rate);
    codec->bit_rate = options.bitrate;
    codec->rc_max_rate = options.bitrate;
    codec->rc_buffer_size = static_cast<int>(options.bitrate * 2);
    codec->gop_size = 48;
    codec->max_b_frames = 0;
    codec->thread_count = 1;
    if (output_format.format->oformat->flags & AVFMT_GLOBALHEADER) {
        codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    AVDictionary *settings = nullptr;
    av_dict_set(&settings, "preset", "medium", 0);
    int const open_result = avcodec_open2(codec.get(), encoder, &settings);
    av_dict_free(&settings);
    av_check(open_result, "open libx264 encoder");
    AVStream *stream = avformat_new_stream(output_format.format, nullptr);
    if (!stream) {
        throw std::runtime_error("create video stream");
    }
    stream->time_base = codec->time_base;
    stream->avg_frame_rate = rate;
    av_check(avcodec_parameters_from_context(stream->codecpar, codec.get()),
             "copy video stream parameters");
    av_check(avio_open(&output_format.format->pb, output.c_str(), AVIO_FLAG_WRITE),
             "open MP4 output");
    av_check(avformat_write_header(output_format.format, nullptr), "write MP4 header");
    std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
    std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    if (!frame || !packet) {
        throw std::runtime_error("allocate video frame or packet");
    }
    frame->format = codec->pix_fmt;
    frame->width = codec->width;
    frame->height = codec->height;
    av_check(av_frame_get_buffer(frame.get(), 32), "allocate video frame pixels");
    std::unique_ptr<SwsContext, SwsDeleter> converter(
        sws_getContext(codec->width, codec->height, AV_PIX_FMT_BGR24, codec->width, codec->height,
                       codec->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!converter) {
        throw std::runtime_error("create color converter");
    }
    auto drain = [&] {
        for (;;) {
            int const status = avcodec_receive_packet(codec.get(), packet.get());
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                return;
            }
            av_check(status, "receive video packet");
            av_packet_rescale_ts(packet.get(), codec->time_base, stream->time_base);
            packet->stream_index = stream->index;
            int const result = av_interleaved_write_frame(output_format.format, packet.get());
            av_packet_unref(packet.get());
            av_check(result, "write video packet");
        }
    };
    for (size_t i = 0; i < png_files.size(); ++i) {
        if (options.cancelled && options.cancelled()) {
            throw std::runtime_error("video encoding cancelled");
        }
        cv::Mat input = i == 0 ? first : cv::imread(png_files[i], cv::IMREAD_COLOR);
        if (input.empty() || input.cols != codec->width || input.rows != codec->height ||
            input.type() != CV_8UC3) {
            throw std::runtime_error("invalid or inconsistent video frame: " + png_files[i]);
        }
        av_check(av_frame_make_writable(frame.get()), "make video frame writable");
        uint8_t const *data[] = {input.data, nullptr, nullptr, nullptr};
        if (input.step > static_cast<size_t>(std::numeric_limits<int>::max())) {
            throw std::runtime_error("video frame row stride too large");
        }
        int const stride[] = {static_cast<int>(input.step), 0, 0, 0};
        if (sws_scale(converter.get(), data, stride, 0, codec->height, frame->data,
                      frame->linesize) != codec->height) {
            throw std::runtime_error("failed to convert video frame");
        }
        if (i > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
            throw std::runtime_error("too many video frames");
        }
        frame->pts = static_cast<int64_t>(i);
        av_check(avcodec_send_frame(codec.get(), frame.get()), "send video frame");
        drain();
        if (options.progress) {
            options.progress(i + 1, png_files.size());
        }
    }
    if (options.cancelled && options.cancelled()) {
        throw std::runtime_error("video encoding cancelled");
    }
    av_check(avcodec_send_frame(codec.get(), nullptr), "flush video encoder");
    drain();
    av_check(av_write_trailer(output_format.format), "write MP4 trailer");
    av_check(avio_closep(&output_format.format->pb), "close MP4 output");
}

} // namespace marching_cubes

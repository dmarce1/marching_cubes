#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace {
namespace fs = std::filesystem;
using nlohmann::json;

void check(bool condition, std::string const &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct TemporaryDirectory {
    fs::path path;
    explicit TemporaryDirectory(fs::path const &parent, std::string const &prefix) {
        std::string pattern = (parent / (prefix + "_XXXXXX")).string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        char *created = mkdtemp(writable.data());
        if (!created) {
            throw std::runtime_error("mkdtemp failed: " + std::string(std::strerror(errno)));
        }
        path = created;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    TemporaryDirectory(TemporaryDirectory const &) = delete;
    TemporaryDirectory &operator=(TemporaryDirectory const &) = delete;
};

std::string read_text(fs::path const &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read " + path.string());
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
json read_json(fs::path const &path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open JSON: " + path.string());
    }
    return json::parse(input);
}
uint64_t hash_bytes(fs::path const &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot hash " + path.string());
    }
    uint64_t value = 14695981039346656037ULL;
    char buffer[8192];
    while (input.read(buffer, sizeof(buffer)) || input.gcount()) {
        for (std::streamsize i = 0; i < input.gcount(); ++i) {
            value ^= static_cast<unsigned char>(buffer[i]);
            value *= 1099511628211ULL;
        }
    }
    if (input.bad()) {
        throw std::runtime_error("failed reading " + path.string());
    }
    return value;
}
std::string hash_hex(fs::path const &path) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash_bytes(path);
    return output.str();
}
struct Fingerprint {
    uintmax_t size;
    fs::file_time_type modified;
    uint64_t hash;
    bool operator==(Fingerprint const &other) const {
        return size == other.size && modified == other.modified && hash == other.hash;
    }
};
Fingerprint fingerprint(fs::path const &path) {
    return {fs::file_size(path), fs::last_write_time(path), hash_bytes(path)};
}
std::map<std::string, Fingerprint> fingerprint_tree(fs::path const &root) {
    std::map<std::string, Fingerprint> result;
    for (auto const &entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            result.emplace(fs::relative(entry.path(), root).string(), fingerprint(entry.path()));
        }
    }
    return result;
}

struct ProcessResult {
    int status;
    std::string output;
};
bool has_png(fs::path const &root) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return false;
    }
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec),
        end;
    while (!ec && it != end) {
        if (it->is_regular_file(ec) && it->path().extension() == ".png") {
            return true;
        }
        it.increment(ec);
    }
    return false;
}
ProcessResult run(std::vector<std::string> const &args, std::string const &input,
                  fs::path const &log, std::chrono::seconds timeout = std::chrono::seconds(120),
                  fs::path const *kill_when_png = nullptr) {
    check(!args.empty(), "empty subprocess argv");
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        throw std::runtime_error("pipe failed");
    }
    int const logfile = open(log.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (logfile < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        throw std::runtime_error("failed to open subprocess log");
    }
    pid_t const child = fork();
    if (child < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        close(logfile);
        throw std::runtime_error("fork failed");
    }
    if (child == 0) {
        if (dup2(pipefd[0], STDIN_FILENO) < 0 || dup2(logfile, STDOUT_FILENO) < 0 ||
            dup2(logfile, STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(pipefd[0]);
        close(pipefd[1]);
        close(logfile);
        std::vector<char *> argv;
        for (std::string const &arg : args) {
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execv(argv[0], argv.data());
        _exit(127);
    }
    close(pipefd[0]);
    close(logfile);
    size_t written = 0;
    while (written < input.size()) {
        ssize_t const n = write(pipefd[1], input.data() + written, input.size() - written);
        if (n > 0) {
            written += static_cast<size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    close(pipefd[1]);
    int status = 0;
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        pid_t const done = waitpid(child, &status, WNOHANG);
        if (done == child) {
            break;
        }
        if (done < 0 && errno != EINTR) {
            throw std::runtime_error("waitpid failed");
        }
        if (kill_when_png && has_png(*kill_when_png)) {
            if (kill(child, SIGKILL) == 0) {
                while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
                }
                break;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            throw std::runtime_error("subprocess timed out: " + args[0]);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kill_when_png ? 2 : 20));
    }
    return {status, read_text(log)};
}

// Hold stdin until the CLI has printed its confirmation prompt, then mutate
// the test checkpoint to model a renderer committing another frame.
ProcessResult run_at_prompt(std::vector<std::string> const &args, fs::path const &log,
                            std::function<void()> const &before_answer) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        throw std::runtime_error("prompt test pipe failed");
    }
    int const logfile = open(log.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (logfile < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        throw std::runtime_error("cannot open prompt test log");
    }
    pid_t const child = fork();
    if (child < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        close(logfile);
        throw std::runtime_error("prompt test fork failed");
    }
    if (child == 0) {
        if (dup2(pipefd[0], STDIN_FILENO) < 0 || dup2(logfile, STDOUT_FILENO) < 0 ||
            dup2(logfile, STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(pipefd[0]);
        close(pipefd[1]);
        close(logfile);
        std::vector<char *> argv;
        for (std::string const &arg : args) {
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execv(argv[0], argv.data());
        _exit(127);
    }
    close(pipefd[0]);
    close(logfile);
    int status = 0;
    bool reaped = false;
    bool answered = false;
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    try {
        while (true) {
            pid_t const done = waitpid(child, &status, WNOHANG);
            if (done == child) {
                reaped = true;
                break;
            }
            if (done < 0 && errno != EINTR) {
                throw std::runtime_error("prompt test waitpid failed");
            }
            if (!answered && read_text(log).find("[y/N]") != std::string::npos) {
                before_answer();
                ssize_t const count = write(pipefd[1], "y\n", 2);
                if (count != 2) {
                    throw std::runtime_error("could not answer preview confirmation");
                }
                close(pipefd[1]);
                pipefd[1] = -1;
                answered = true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("preview prompt test timed out");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!answered) {
            throw std::runtime_error("preview exited before confirmation prompt: " +
                                     read_text(log));
        }
    } catch (...) {
        if (pipefd[1] >= 0) {
            close(pipefd[1]);
        }
        if (!reaped) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
        }
        throw;
    }
    return {status, read_text(log)};
}
void success(ProcessResult const &result, std::string const &what) {
    check(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0,
          what + " failed: " + result.output);
}
void failure(ProcessResult const &result, std::string const &what) {
    check(WIFEXITED(result.status) && WEXITSTATUS(result.status) != 0,
          what + " did not reject cleanly: " + result.output);
}
bool different_devices(fs::path const &a, fs::path const &b) {
    struct stat x{}, y{};
    if (stat(a.c_str(), &x) != 0 || stat(b.c_str(), &y) != 0) {
        throw std::runtime_error("cannot inspect filesystem devices");
    }
    return x.st_dev != y.st_dev;
}
std::vector<std::string> movie_args(fs::path const &binary, fs::path const &input,
                                    fs::path const &scratch, fs::path const &output) {
    return {binary.string(),
            "movie",
            "--input-dir",
            input.string(),
            "--scratch",
            scratch.string(),
            "--output",
            output.string(),
            "--bounds=-2,2,-2,2,-2,2",
            "--samples=21,21,21",
            "--reference=1",
            "--fields=rho_1,rho_2",
            "--levels=2,0.3",
            "--opacities=1,0.4",
            "--size=256",
            "--focus=0,0,0",
            "--scale=2.2",
            "--initial-period=2",
            "--time-unit-seconds=60",
            "--length-unit-cm=1e10",
            "--density-unit-g-cm3=1",
            "--first=0",
            "--last=1",
            "--fps=4",
            "--zero-time-index0",
            "--reserve-gib=0.01"};
}
void append(std::vector<std::string> &args, std::string const &value) { args.push_back(value); }

std::vector<std::string> encode_args(fs::path const &binary, fs::path const &scratch,
                                     fs::path const &output) {
    return {binary.string(), "encode",        "--scratch",         scratch.string(),
            "--output",      output.string(), "--reserve-gib=0.01"};
}

struct HeldLock {
    int fd;
    explicit HeldLock(fs::path const &path) : fd(open(path.c_str(), O_RDWR | O_CLOEXEC)) {
        if (fd < 0) {
            throw std::runtime_error("cannot open production lock for preview test");
        }
        if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
            close(fd);
            fd = -1;
            throw std::runtime_error("cannot hold production lock for preview test");
        }
    }
    ~HeldLock() {
        if (fd >= 0) {
            close(fd);
        }
    }
    HeldLock(HeldLock const &) = delete;
    HeldLock &operator=(HeldLock const &) = delete;
};

struct RenamedFile {
    fs::path original, renamed;
    RenamedFile(fs::path const &from, fs::path const &to) : original(from), renamed(to) {
        fs::rename(original, renamed);
    }
    ~RenamedFile() {
        std::error_code ignored;
        fs::rename(renamed, original, ignored);
    }
    RenamedFile(RenamedFile const &) = delete;
    RenamedFile &operator=(RenamedFile const &) = delete;
};

void replace_text(fs::path const &path, std::string const &text) {
    fs::path const temporary = path.string() + ".integration_test_tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.close();
        check(bool(stream), "failed writing test checkpoint update");
    }
    fs::rename(temporary, path);
}

struct RestoreFile {
    fs::path path;
    std::string contents;
    fs::file_time_type modified;
    bool active = true;
    explicit RestoreFile(fs::path const &file)
        : path(file), contents(read_text(file)), modified(fs::last_write_time(file)) {}
    void restore() {
        if (active) {
            replace_text(path, contents);
            fs::last_write_time(path, modified);
            active = false;
        }
    }
    ~RestoreFile() {
        if (active) {
            try {
                restore();
            } catch (...) {
                // The test's private temporary root is discarded on failure.
            }
        }
    }
    RestoreFile(RestoreFile const &) = delete;
    RestoreFile &operator=(RestoreFile const &) = delete;
};

void test_legacy_frame(fs::path const &binary, fs::path const &input, fs::path const &root) {
    fs::path const output = root / "legacy_frame";
    std::vector<std::string> const args{binary.string(),
                                        "frame",
                                        "--input",
                                        (input / "X.0.silo").string(),
                                        "--output",
                                        output.string(),
                                        "--bounds=-2,2,-2,2,-2,2",
                                        "--samples=21,21,21",
                                        "--reference=1",
                                        "--fields=rho_1,rho_2",
                                        "--size=64",
                                        "--levels=2,0.3",
                                        "--opacities=1,0.4",
                                        "--time=0"};
    success(run(args, "", root / "legacy.log"), "legacy frame");
    std::string const png = read_text(output / "frame.png");
    check(png.size() > 8 && png.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0,
          "legacy PNG is invalid");
    json const meta = read_json(output / "frame.json");
    check(meta.at("fields") == json::array({"rho_1", "rho_2"}) && meta.at("time") == 0 &&
              meta.at("time_overridden") == true,
          "legacy metadata mismatch");
    check(meta.at("surfaces").at(0).at("triangles") == 0 &&
              meta.at("surfaces").at(1).at("triangles").get<size_t>() > 0,
          "legacy empty-level handling mismatch");
}

void test_guards(fs::path const &binary, fs::path const &input, fs::path const &source_root,
                 fs::path const &root) {
    for (std::string const &answer : {std::string("n\n"), std::string()}) {
        bool const eof = answer.empty();
        fs::path const scratch = root / (eof ? "eof_scratch" : "no_scratch");
        fs::path const output = root / (eof ? "eof_output" : "no_output");
        ProcessResult const result = run(movie_args(binary, input, scratch, output), answer,
                                         root / (eof ? "eof.log" : "no.log"));
        check(WIFEXITED(result.status), "confirmation process crashed: " + result.output);
        check(result.output.find("[y/N]") != std::string::npos,
              "confirmation prompt absent: " + result.output);
        check(!fs::exists(scratch) && !fs::exists(output), "declining or EOF created movie output");
    }
    fs::path const dry_scratch = root / "dry_scratch";
    fs::path const dry_output = root / "dry_output";
    auto dry = movie_args(binary, input, dry_scratch, dry_output);
    append(dry, "--dry-run");
    ProcessResult const dry_result = run(dry, "", root / "dry.log");
    success(dry_result, "dry-run");
    check(dry_result.output.find("[y/N]") == std::string::npos, "dry-run prompted for input");
    check(!fs::exists(dry_scratch) && !fs::exists(dry_output), "dry-run created movie output");

    fs::path const bad_scratch = source_root / "same_device_scratch";
    fs::path const bad_output = root / "same_device_output";
    ProcessResult const same_device =
        run(movie_args(binary, input, bad_scratch, bad_output), "y\n", root / "device.log");
    failure(same_device, "same-device source and scratch");
    check(same_device.output.find("source filesystem") != std::string::npos ||
              same_device.output.find("source: ") != std::string::npos,
          "same-device rejection did not report source storage: " + same_device.output);
    check(!fs::exists(bad_scratch) && !fs::exists(bad_output),
          "same-device rejection created output");

    fs::path const bypass_scratch = root / "bypass_scratch";
    fs::path const bypass_output = root / "bypass_output";
    auto bypass = movie_args(binary, input, bypass_scratch, bypass_output);
    append(bypass, "--yes");
    failure(run(bypass, "y\n", root / "bypass.log"), "unsupported confirmation bypass");
    check(!fs::exists(bypass_scratch) && !fs::exists(bypass_output),
          "unsupported bypass created output");

    fs::path const fps_scratch = root / "unrepresentable_fps_scratch";
    fs::path const fps_output = root / "unrepresentable_fps_output";
    auto fps_args = movie_args(binary, input, fps_scratch, fps_output);
    auto const fps = std::find(fps_args.begin(), fps_args.end(), "--fps=4");
    check(fps != fps_args.end(), "test configuration lacks FPS");
    *fps = "--fps=0.000001";
    append(fps_args, "--bitrate=1");
    ProcessResult const bad_fps = run(fps_args, "y\n", root / "unrepresentable_fps.log");
    failure(bad_fps, "unrepresentable FPS");
    check(bad_fps.output.find("frame rate cannot be represented") != std::string::npos &&
              bad_fps.output.find("[y/N]") == std::string::npos,
          "unrepresentable FPS was not rejected before the prompt: " + bad_fps.output);
    check(!fs::exists(fps_scratch) && !fs::exists(fps_output),
          "unrepresentable FPS created output directories");

    fs::path const ruler_scratch = root / "oversized_ruler_scratch";
    fs::path const ruler_output = root / "oversized_ruler_output";
    auto ruler_args = movie_args(binary, input, ruler_scratch, ruler_output);
    append(ruler_args, "--ruler-cm=1e99");
    ProcessResult const bad_ruler = run(ruler_args, "y\n", root / "oversized_ruler.log");
    failure(bad_ruler, "oversized ruler");
    check(bad_ruler.output.find("ruler length does not fit") != std::string::npos &&
              bad_ruler.output.find("[y/N]") == std::string::npos,
          "oversized ruler was not rejected before the prompt: " + bad_ruler.output);
    check(!fs::exists(ruler_scratch) && !fs::exists(ruler_output),
          "oversized ruler created output directories");
}

fs::path only_png(fs::path const &root) {
    check(fs::is_directory(root), "scratch directory missing: " + root.string());
    std::vector<fs::path> files;
    for (auto const &entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".png") {
            files.push_back(entry.path());
        }
    }
    check(files.size() == 1, "expected exactly one checkpoint PNG in " + root.string());
    return files.front();
}
void check_png(fs::path const &path) {
    std::string const bytes = read_text(path);
    check(bytes.size() > 8 && bytes.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0,
          "PNG header is invalid: " + path.string());
}
void check_mp4(fs::path const &path) {
    std::ifstream file(path, std::ios::binary);
    char header[8]{};
    file.read(header, sizeof(header));
    check(file.gcount() == 8 && std::string(header + 4, 4) == "ftyp" && fs::file_size(path) > 100,
          "final MP4 is missing or invalid: " + path.string());
}

void test_encode_preview(fs::path const &binary, fs::path const &input, fs::path const &scratch,
                         fs::path const &production, fs::path const &root) {
    fs::path const checkpoint_path = scratch / "run.json";
    fs::path const first_png = scratch / "frames/frame_000000.png";
    Fingerprint const checkpoint_before = fingerprint(checkpoint_path);
    Fingerprint const frame_before = fingerprint(first_png);
    json const original = read_json(checkpoint_path);
    check(original.at("frames").at(0).is_object() && original.at("frames").at(1).is_null(),
          "preview test requires exactly one committed frame");
    fs::path const preview = root / "preview.mp4";
    fs::path const sidecar = preview.string() + ".json";
    auto args = encode_args(binary, scratch, preview);
    auto dry = args;
    append(dry, "--dry-run");
    ProcessResult const dry_result = run(dry, "", root / "preview_dry.log");
    success(dry_result, "preview dry-run");
    check(dry_result.output.find("[y/N]") == std::string::npos && !fs::exists(preview) &&
              !fs::exists(sidecar) && !fs::exists(preview.string() + ".lock"),
          "preview dry-run prompted or wrote files");
    ProcessResult const declined = run(args, "", root / "preview_declined.log");
    check(WIFEXITED(declined.status) && declined.output.find("[y/N]") != std::string::npos &&
              !fs::exists(preview) && !fs::exists(sidecar) &&
              !fs::exists(preview.string() + ".lock"),
          "preview EOF was not safely declined");

    fs::path const production_movie = production / "movie.mp4";
    ProcessResult const forbidden_production =
        run(encode_args(binary, scratch, production_movie), "y\n", root / "preview_production.log");
    failure(forbidden_production, "preview into production movie path");
    check(!fs::exists(production_movie), "preview created production movie");
    fs::path const forbidden_scratch = scratch / "forbidden.mp4";
    ProcessResult const forbidden =
        run(encode_args(binary, scratch, forbidden_scratch), "y\n", root / "preview_scratch.log");
    failure(forbidden, "preview inside scratch");
    check(!fs::exists(forbidden_scratch), "preview created output in scratch");

    // Extra complete and incomplete PNGs are not in run.json, so neither is
    // part of the frozen contiguous prefix.
    fs::path const second_png = scratch / "frames/frame_000001.png";
    fs::path const partial_png = scratch / "frames/frame_000001.tmp.png";
    fs::copy_file(first_png, second_png);
    fs::copy_file(first_png, partial_png);
    {
        HeldLock scratch_lock(scratch / ".movie.lock");
        HeldLock production_lock(production / ".movie.lock");
        success(run(args, "y\n", root / "preview_locked.log", std::chrono::seconds(30)),
                "preview while production locks are held");
    }
    check_mp4(preview);
    json const preview_metadata = read_json(sidecar);
    check(preview_metadata.at("schema") == 1 && preview_metadata.at("kind") == "partial_movie" &&
              preview_metadata.at("source_checkpoint") == fs::absolute(checkpoint_path).string() &&
              preview_metadata.at("frames_total_planned") == 2 &&
              preview_metadata.at("frame_count") == 1 &&
              preview_metadata.at("snapshot_indices") == json::array({0}) &&
              preview_metadata.at("fps") == 4 &&
              preview_metadata.at("video_hash") == hash_hex(preview) &&
              preview_metadata.at("video_bytes").get<uintmax_t>() == fs::file_size(preview),
          "preview sidecar has incorrect frozen prefix, inherited FPS, or video integrity");
    check(fingerprint(checkpoint_path) == checkpoint_before &&
              fingerprint(first_png) == frame_before,
          "preview modified its committed checkpoint or frame");

    Fingerprint const preview_before = fingerprint(preview);
    Fingerprint const sidecar_before = fingerprint(sidecar);
    failure(run(args, "y\n", root / "preview_no_replace.log"),
            "preview overwrite without --replace");
    check(fingerprint(preview) == preview_before && fingerprint(sidecar) == sidecar_before,
          "refused preview overwrite modified existing output");
    {
        RestoreFile restore(first_png);
        {
            std::ofstream truncated(first_png, std::ios::binary | std::ios::trunc);
            truncated << "bad";
            check(bool(truncated), "cannot damage private fixture PNG");
        }
        auto replace = args;
        append(replace, "--replace");
        failure(run(replace, "y\n", root / "preview_damaged.log"), "damaged committed frame");
        check(fingerprint(preview) == preview_before && fingerprint(sidecar) == sidecar_before,
              "failed preview replacement changed existing preview or metadata");
        restore.restore();
    }
    check(fingerprint(first_png) == frame_before, "private PNG fixture was not restored");

    // The frame can change after initial validation while the user is deciding.
    // A failed replacement must preserve both previously published files.
    {
        RestoreFile restore(first_png);
        auto replace = args;
        append(replace, "--replace");
        auto damage_after_prompt = [&] {
            std::ofstream truncated(first_png, std::ios::binary | std::ios::trunc);
            truncated << "damaged after initial validation";
            truncated.close();
            check(bool(truncated), "cannot damage frame during confirmation");
        };
        ProcessResult const failed =
            run_at_prompt(replace, root / "preview_post_prompt_failure.log", damage_after_prompt);
        failure(failed, "preview replacement after frame changed during confirmation");
        check(fingerprint(preview) == preview_before && fingerprint(sidecar) == sidecar_before,
              "post-confirmation failure changed the prior preview or sidecar");
        restore.restore();
    }
    check(fingerprint(first_png) == frame_before, "post-confirmation PNG was not restored");

    // A valid later record must not bridge the first null entry.
    json later = original.at("frames").at(0);
    later["snapshot_index"] = 1;
    later["png"] = "frames/frame_000001.png";
    later["png_hash"] = hash_hex(second_png);
    {
        RestoreFile restore(checkpoint_path);
        json no_prefix = original;
        no_prefix["frames"][0] = nullptr;
        no_prefix["frames"][1] = later;
        replace_text(checkpoint_path, no_prefix.dump(2) + "\n");
        fs::path const absent = root / "preview_no_prefix.mp4";
        failure(run(encode_args(binary, scratch, absent), "y\n", root / "preview_no_prefix.log"),
                "preview with no committed prefix");
        check(!fs::exists(absent) && !fs::exists(absent.string() + ".json"),
              "preview with no committed prefix wrote output");
        restore.restore();
    }

    // The checkpoint advances after the exporter reads its prefix but before
    // the confirmation reply. The sidecar must describe the original prefix.
    {
        RestoreFile restore(checkpoint_path);
        fs::path const frozen = root / "preview_frozen.mp4";
        auto advance = [&] {
            json updated = original;
            updated["frames"][1] = later;
            replace_text(checkpoint_path, updated.dump(2) + "\n");
        };
        success(run_at_prompt(encode_args(binary, scratch, frozen), root / "preview_frozen.log",
                              advance),
                "preview with concurrent checkpoint advance");
        check_mp4(frozen);
        json const meta = read_json(frozen.string() + ".json");
        check(meta.at("frame_count") == 1 && meta.at("snapshot_indices") == json::array({0}),
              "preview reread a newer checkpoint after its confirmation prompt");
        restore.restore();
    }

    fs::path const overridden = root / "preview_override.mp4";
    auto override_args = encode_args(binary, scratch, overridden);
    append(override_args, "--fps=7");
    {
        RenamedFile missing_first(input / "X.0.silo", input / "X.0.missing");
        RenamedFile missing_second(input / "X.1.silo", input / "X.1.missing");
        success(run(override_args, "y\n", root / "preview_missing_sources.log"),
                "preview without original Silo files");
    }
    check_mp4(overridden);
    check(read_json(overridden.string() + ".json").at("fps") == 7,
          "preview did not honor explicit FPS");
    auto replace_success = args;
    append(replace_success, "--replace");
    append(replace_success, "--fps=6");
    success(run(replace_success, "y\n", root / "preview_replace_success.log"),
            "successful preview replacement");
    check_mp4(preview);
    json const replaced_metadata = read_json(sidecar);
    check(replaced_metadata.at("fps") == 6 && replaced_metadata.at("frame_count") == 1 &&
              replaced_metadata.at("snapshot_indices") == json::array({0}) &&
              replaced_metadata.at("video_hash") == hash_hex(preview) &&
              replaced_metadata.at("video_bytes").get<uintmax_t>() == fs::file_size(preview),
          "successful replacement has stale or inconsistent preview metadata");
    check(fingerprint(checkpoint_path) == checkpoint_before &&
              fingerprint(first_png) == frame_before,
          "preview tests did not preserve the source checkpoint and committed frame");
    fs::remove(second_png);
    fs::remove(partial_png);
}

void test_startup_resume(fs::path const &binary, fs::path const &input, fs::path const &root) {
    fs::path const scratch = root / "startup_scratch";
    fs::path const output = root / "startup_output";
    check(fs::create_directory(scratch) && fs::create_directory(output),
          "could not create startup interruption directories");
    for (fs::path const &directory : {scratch, output}) {
        std::ofstream lock(directory / ".movie.lock", std::ios::binary);
        check(bool(lock), "could not create startup interruption lock");
    }
    auto resume = movie_args(binary, input, scratch, output);
    append(resume, "--resume");
    success(run(resume, "y\n", root / "startup_resume.log"), "resume after startup interruption");
    check_mp4(output / "movie.mp4");
    json const state = read_json(scratch / "run.json");
    check(state.at("schema") == 2 && state.at("stage") == "complete" &&
              state.at("frames").size() == 2 && state.at("frames").at(0).is_object() &&
              state.at("frames").at(1).is_object(),
          "startup resume did not complete both frames");
}

void test_resume(fs::path const &binary, fs::path const &input, fs::path const &root) {
    fs::path const scratch = root / "resume_scratch";
    fs::path const output = root / "resume_output";
    auto initial = movie_args(binary, input, scratch, output);
    append(initial, "--stop-after=1");
    success(run(initial, "y\n", root / "checkpoint.log"), "checkpoint run");
    fs::path const first_png = only_png(scratch);
    json const checkpoint = read_json(scratch / "run.json");
    check(checkpoint.at("schema") == 2 && checkpoint.at("stage") == "rendering" &&
              checkpoint.at("movie").is_null(),
          "checkpoint manifest state mismatch");
    check(checkpoint.at("frames").size() == 2 && checkpoint.at("frames").at(0).is_object() &&
              checkpoint.at("frames").at(1).is_null(),
          "checkpoint should contain exactly one completed frame");
    check(checkpoint.at("frames").at(0).at("snapshot_index") == 0 &&
              checkpoint.at("frames").at(0).at("png") == "frames/frame_000000.png" &&
              first_png == scratch / "frames/frame_000000.png",
          "checkpoint frame identity mismatch");
    check_png(first_png);
    Fingerprint const first = fingerprint(first_png);
    check(!fs::exists(output / "movie.mp4"), "checkpoint prematurely encoded a movie");
    test_encode_preview(binary, input, scratch, output, root);

    // A stale legacy temporary is a hardlink to an unrelated file. Checkpoint
    // updates must never truncate or replace either name.
    fs::path const sentinel = root / "unrelated_sentinel";
    {
        std::ofstream stream(sentinel, std::ios::binary);
        stream << "unrelated data that must remain unchanged\n";
        check(bool(stream), "cannot create sentinel fixture");
    }
    fs::path const stale_temp = scratch / "run.json.tmp";
    fs::create_hard_link(sentinel, stale_temp);
    Fingerprint const sentinel_before = fingerprint(sentinel);

    auto mismatch = movie_args(binary, input, scratch, output);
    auto const fps = std::find(mismatch.begin(), mismatch.end(), "--fps=4");
    check(fps != mismatch.end(), "test configuration lacks FPS");
    *fps = "--fps=5";
    append(mismatch, "--resume");
    ProcessResult const incompatible = run(mismatch, "y\n", root / "mismatch.log");
    failure(incompatible, "incompatible resume");
    check(incompatible.output.find("configuration mismatch") != std::string::npos,
          "incompatible resume reported the wrong failure: " + incompatible.output);
    check(fingerprint(first_png) == first, "rejected resume modified completed frame");
    check(!fs::exists(output / "movie.mp4"), "rejected resume published a movie");

    auto resume = movie_args(binary, input, scratch, output);
    append(resume, "--resume");
    success(run(resume, "y\n", root / "resume.log"), "resume");
    check(fingerprint(first_png) == first, "resume rewrote a completed valid PNG");
    check(fs::equivalent(sentinel, stale_temp) && fingerprint(sentinel) == sentinel_before,
          "checkpoint update changed an unrelated hardlinked temporary");
    check_mp4(output / "movie.mp4");
    json const final = read_json(output / "manifest.json");
    check(final == read_json(scratch / "run.json"),
          "published manifest differs from checkpoint state");
    check(final.at("schema") == 2 && final.at("stage") == "complete" &&
              final.at("movie").at("filename") == "movie.mp4" &&
              final.at("movie").at("bytes").get<uintmax_t>() ==
                  fs::file_size(output / "movie.mp4") &&
              final.at("movie").at("hash") == hash_hex(output / "movie.mp4"),
          "final movie manifest mismatch");
    check(final.at("frames").size() == 2 && final.at("frames").at(0).at("snapshot_index") == 0 &&
              final.at("frames").at(1).at("snapshot_index") == 1 &&
              final.at("frames").at(1).at("png") == "frames/frame_000001.png",
          "final frame order mismatch");
    for (auto const &frame : final.at("frames")) {
        std::string const hash = frame.at("png_hash").get<std::string>();
        check(!hash.empty() && std::all_of(hash.begin(), hash.end(),
                                           [](unsigned char c) {
                                               return (c >= '0' && c <= '9') ||
                                                      (c >= 'a' && c <= 'f') ||
                                                      (c >= 'A' && c <= 'F');
                                           }),
              "frame hash is not hexadecimal");
        fs::path const png = scratch / frame.at("png").get<std::string>();
        check_png(png);
        check(hash == hash_hex(png), "recorded PNG hash does not match content");
    }
    auto const &meta0 = final.at("frames").at(0).at("metadata");
    auto const &meta1 = final.at("frames").at(1).at("metadata");
    check(meta0.at("time") == 0 && meta0.at("time_overridden") == true &&
              std::abs(meta0.at("raw_master_time").get<double>() / 4.49081e-97 - 1) < 1e-12 &&
              meta1.at("time") == 0.5 && meta1.at("time_overridden") == false,
          "source time and override metadata mismatch");
    Fingerprint const movie = fingerprint(output / "movie.mp4");
    success(run(resume, "y\n", root / "completed_resume.log"), "resume completed movie");
    check(fingerprint(first_png) == first && fingerprint(output / "movie.mp4") == movie,
          "completed resume rewrote a verified artifact");
    check(fs::equivalent(sentinel, stale_temp) && fingerprint(sentinel) == sentinel_before,
          "completed resume changed an unrelated hardlinked temporary");
}

void test_corrupt_frame(fs::path const &binary, fs::path const &input, fs::path const &root) {
    fs::path const scratch = root / "corrupt_scratch";
    fs::path const output = root / "corrupt_output";
    auto first = movie_args(binary, input, scratch, output);
    append(first, "--stop-after=1");
    success(run(first, "y\n", root / "corrupt_checkpoint.log"), "corrupt-test checkpoint");
    fs::path const png = only_png(scratch);
    uint64_t const original_hash = hash_bytes(png);
    {
        std::ofstream truncated(png, std::ios::binary | std::ios::trunc);
        check(bool(truncated), "failed to truncate checkpoint PNG");
        truncated << "bad";
    }
    auto resume = movie_args(binary, input, scratch, output);
    append(resume, "--resume");
    success(run(resume, "y\n", root / "corrupt_resume.log"), "corrupt-frame resume");
    check_png(png);
    check(hash_bytes(png) == original_hash, "corrupt PNG was not regenerated identically");
    check_mp4(output / "movie.mp4");
}

void test_sigkill_resume(fs::path const &binary, fs::path const &input, fs::path const &root) {
    fs::path const scratch = root / "kill_scratch";
    fs::path const output = root / "kill_output";
    ProcessResult const stopped = run(movie_args(binary, input, scratch, output), "y\n",
                                      root / "kill.log", std::chrono::seconds(25), &scratch);
    if (!WIFSIGNALED(stopped.status) || WTERMSIG(stopped.status) != SIGKILL) {
        success(stopped, "fast completion in SIGKILL test");
        std::cout << "SKIP SIGKILL restart: fixture movie finished before checkpoint could be "
                     "interrupted\n";
        return;
    }
    auto resume = movie_args(binary, input, scratch, output);
    append(resume, "--resume");
    success(run(resume, "y\n", root / "kill_resume.log"), "resume after SIGKILL");
    check_mp4(output / "movie.mp4");
}

void test_changed_source(fs::path const &binary, fs::path const &input, fs::path const &root) {
    fs::path const scratch = root / "changed_scratch";
    fs::path const output = root / "changed_output";
    auto first = movie_args(binary, input, scratch, output);
    append(first, "--stop-after=1");
    success(run(first, "y\n", root / "changed_checkpoint.log"), "source-change checkpoint");
    fs::path const frame = only_png(scratch);
    Fingerprint const before = fingerprint(frame);
    {
        std::ofstream changed(input / "X.1.silo", std::ios::binary | std::ios::app);
        check(bool(changed), "cannot modify test-only source fixture");
        changed.put('\0');
        check(bool(changed), "cannot modify test-only source fixture");
    }
    auto resume = movie_args(binary, input, scratch, output);
    append(resume, "--resume");
    ProcessResult const changed = run(resume, "y\n", root / "changed_resume.log");
    failure(changed, "changed-source resume");
    check(changed.output.find("Source files changed") != std::string::npos ||
              changed.output.find("Source changed") != std::string::npos,
          "changed-source resume reported the wrong failure: " + changed.output);
    check(fingerprint(frame) == before, "changed-source rejection modified saved frame");
    check(!fs::exists(output / "movie.mp4"), "changed-source resume published a movie");
}

bool nearly(double a, double b) {
    return std::abs(a - b) <= 1e-9 * std::max({1.0, std::abs(a), std::abs(b)});
}
void collect_annotation_metadata(json const &value, std::vector<json> &found) {
    if (value.is_object()) {
        if (value.contains("time_seconds") && value.contains("time_minutes") &&
            value.contains("time_over_initial_period") && value.contains("ruler_cm") &&
            value.contains("ruler_solar_radii") && value.contains("ruler_exact_pixels") &&
            value.contains("ruler_rendered_cm")) {
            found.push_back(value);
        }
        for (auto const &item : value.items()) {
            collect_annotation_metadata(item.value(), found);
        }
    } else if (value.is_array()) {
        for (auto const &item : value) {
            collect_annotation_metadata(item, found);
        }
    }
}
void check_annotation_metadata(fs::path const &scratch, fs::path const &output) {
    std::vector<json> annotations;
    for (fs::path const &root : {scratch, output}) {
        if (!fs::is_directory(root)) {
            continue;
        }
        for (auto const &entry : fs::recursive_directory_iterator(root)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                collect_annotation_metadata(read_json(entry.path()), annotations);
            }
        }
    }
    check(!annotations.empty(), "no annotated frame metadata was persisted");
    bool saw_zero = false, saw_second = false;
    for (json const &a : annotations) {
        double const time = a.at("time_seconds").get<double>();
        if (nearly(time, 0)) {
            saw_zero = true;
        } else if (nearly(time, 30)) {
            saw_second = true;
        } else {
            throw std::runtime_error("incorrect annotated frame time in seconds");
        }
        check(nearly(a.at("time_minutes").get<double>(), time / 60),
              "time minutes conversion mismatch");
        check(nearly(a.at("time_over_initial_period").get<double>(), time / 2),
              "orbital time conversion mismatch");
        check(a.at("scene_width") == 256 && a.at("scene_height") == 256,
              "annotation scene dimensions mismatch");
        check(a.at("time_unit_seconds") == 60 && a.at("length_unit_cm") == 1e10 &&
                  a.at("density_unit_g_cm3") == 1,
              "physical unit metadata mismatch");
        check(a.at("surfaces").size() == 2 && a.at("surfaces").at(0).at("density_g_cm3") == 2 &&
                  nearly(a.at("surfaces").at(1).at("density_g_cm3").get<double>(), 0.3),
              "annotated density conversion mismatch");
        double const ruler = a.at("ruler_cm").get<double>();
        double const pixels = a.at("ruler_exact_pixels").get<double>();
        check(ruler > 0 && nearly(pixels, ruler * 256 / (4.4e10)),
              "physical ruler does not match scene scale");
        check(nearly(a.at("ruler_solar_radii").get<double>(), ruler / 6.957e10),
              "solar radius conversion mismatch");
        check(nearly(a.at("ruler_rendered_cm").get<double>(),
                     a.at("ruler_pixels").get<double>() * 4.4e10 / 256),
              "rendered ruler pixel scale mismatch");
    }
    check(saw_zero && saw_second, "missing zero or second-frame physical annotation time");
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 3) {
            throw std::invalid_argument("usage: test_pipeline MARCHING_CUBES MAKE_FIXTURE");
        }
        std::signal(SIGPIPE, SIG_IGN);
        fs::path const binary = fs::canonical(argv[1]);
        fs::path const fixture = fs::canonical(argv[2]);
        TemporaryDirectory source_root("/tmp", "mc_pipeline_source");
        TemporaryDirectory work_root(fs::current_path(), "mc_pipeline_work");
        if (!different_devices(source_root.path, work_root.path)) {
            std::cout
                << "SKIP: /tmp and test working directory are on the same filesystem device\n";
            return 77;
        }
        fs::path const input = source_root.path / "inputs";
        success(run({fixture.string(), input.string()}, "", work_root.path / "fixture.log"),
                "create fixture");
        auto const original = fingerprint_tree(input);
        check(!original.empty(), "fixture contains no source files");
        test_legacy_frame(binary, input, work_root.path);
        test_guards(binary, input, source_root.path, work_root.path);
        test_resume(binary, input, work_root.path);
        check_annotation_metadata(work_root.path / "resume_scratch",
                                  work_root.path / "resume_output");
        test_startup_resume(binary, input, work_root.path);
        test_corrupt_frame(binary, input, work_root.path);
        test_sigkill_resume(binary, input, work_root.path);
        check(fingerprint_tree(input) == original,
              "movie operations modified source file contents or metadata");

        // Deliberate source mutation is restricted to a separate disposable fixture.
        fs::path const mutable_input = source_root.path / "mutable_inputs";
        success(run({fixture.string(), mutable_input.string()}, "",
                    work_root.path / "fixture_mutable.log"),
                "create mutable fixture");
        test_changed_source(binary, mutable_input, work_root.path);
        check(fingerprint_tree(input) == original,
              "mutable-fixture test modified original sources");
        std::cout << "native pipeline integration tests passed\n";
        return 0;
    } catch (std::exception const &error) {
        std::cerr << "native pipeline test: " << error.what() << '\n';
        return 1;
    }
}

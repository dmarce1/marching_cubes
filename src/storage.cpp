#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <silo.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>

#include "marching_cubes/storage.hpp"

namespace marching_cubes {
namespace {
namespace fs = std::filesystem;

using U = uint64_t;
U add(U a, U b) {
    if (a > std::numeric_limits<U>::max() - b) {
        throw std::overflow_error("storage estimate overflow");
    }
    return a + b;
}
U mul(U a, U b) {
    if (b && a > std::numeric_limits<U>::max() / b) {
        throw std::overflow_error("storage estimate overflow");
    }
    return a * b;
}
U ceil_div(U a, U b) { return a / b + (a % b != 0); }
struct FileCloser {
    void operator()(DBfile *p) const {
        if (p) {
            DBClose(p);
        }
    }
};
struct MeshCloser {
    void operator()(DBmultimesh *p) const {
        if (p) {
            DBFreeMultimesh(p);
        }
    }
};
struct VarCloser {
    void operator()(DBmultivar *p) const {
        if (p) {
            DBFreeMultivar(p);
        }
    }
};
using File = std::unique_ptr<DBfile, FileCloser>;
using MultiMesh = std::unique_ptr<DBmultimesh, MeshCloser>;
using MultiVar = std::unique_ptr<DBmultivar, VarCloser>;

FileStamp stamp(fs::path const &p) {
    auto const real = fs::canonical(p);
    if (!fs::is_regular_file(real)) {
        throw std::runtime_error("source is not a regular file: " + real.string());
    }
    uintmax_t const size = fs::file_size(real);
    if (!size) {
        throw std::runtime_error("empty source file: " + real.string());
    }
    auto const ticks = fs::last_write_time(real).time_since_epoch().count();
    return {real, size, static_cast<int64_t>(ticks)};
}
std::pair<std::string, std::string> reference(std::string const &s, fs::path const &master) {
    size_t const colon = s.find(':');
    if (colon == std::string::npos) {
        return {master.string(), s};
    }
    fs::path file = s.substr(0, colon);
    if (file.is_relative()) {
        file = master.parent_path() / file;
    }
    return {file.lexically_normal().string(), s.substr(colon + 1)};
}
bool within(fs::path const &child, fs::path const &parent) {
    auto a = child.begin();
    for (auto b = parent.begin(); b != parent.end(); ++b, ++a) {
        if (a == child.end() || *a != *b) {
            return false;
        }
    }
    return true;
}
fs::path ancestor(fs::path p) {
    while (!fs::exists(p)) {
        auto const next = p.parent_path();
        if (next == p || next.empty()) {
            throw std::runtime_error("no existing path ancestor: " + p.string());
        }
        p = next;
    }
    if (!fs::is_directory(p)) {
        throw std::runtime_error("destination ancestor is not a directory: " + p.string());
    }
    return p;
}
fs::path destination(fs::path const &p) {
    if (p.empty()) {
        throw std::invalid_argument("empty destination path");
    }
    fs::path const real = fs::weakly_canonical(fs::absolute(p));
    ancestor(real);
    return real;
}
uintmax_t device(fs::path const &p) {
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0) {
        throw std::runtime_error("cannot stat: " + p.string());
    }
    return static_cast<uintmax_t>(st.st_dev);
}
// Resolve partition parents and device-mapper/MD slave devices when sysfs
// exposes them. Pseudo filesystems have no corresponding block device.
void collect_disks(fs::path const &entry, std::set<fs::path> &disks, std::set<fs::path> &visited) {
    std::error_code ec;
    fs::path node = fs::canonical(entry, ec);
    if (ec || !visited.insert(node).second) {
        return;
    }
    if (fs::exists(node / "partition")) {
        node = node.parent_path();
    }
    fs::path const slaves = node / "slaves";
    bool has_slaves = false;
    if (fs::is_directory(slaves)) {
        for (auto const &child : fs::directory_iterator(slaves)) {
            has_slaves = true;
            collect_disks(child.path(), disks, visited);
        }
    }
    if (!has_slaves) {
        disks.insert(node);
    }
}
std::set<fs::path> physical_disks(uintmax_t id) {
    dev_t const d = static_cast<dev_t>(id);
    fs::path const link =
        fs::path("/sys/dev/block") / (std::to_string(major(d)) + ":" + std::to_string(minor(d)));
    std::set<fs::path> disks, visited;
    collect_disks(link, disks, visited);
    return disks;
}
bool shares_disk(std::set<fs::path> const &a, std::set<fs::path> const &b) {
    for (auto const &p : a) {
        if (b.count(p)) {
            return true;
        }
    }
    return false;
}
void require_local(fs::path const &p) {
    struct statfs s{};
    if (::statfs(p.c_str(), &s) != 0) {
        throw std::runtime_error("cannot inspect filesystem: " + p.string());
    }
    // Known network/remote and FUSE filesystems (which may be sshfs or rclone).
    uint32_t const type = static_cast<uint32_t>(s.f_type);
    if (type == 0x6969UL || type == 0xff534d42UL || type == 0xfe534d42UL || type == 0x65735546UL ||
        type == 0x5346414fUL || type == 0x73757245UL || type == 0x564cUL || type == 0x19830326UL) {
        throw std::runtime_error("destination filesystem is network or FUSE: " + p.string());
    }
}
SpaceVolume volume(fs::path const &p, U payload, U requested_reserve) {
    U constexpr gib = U(1) << 30;
    U const reserve = requested_reserve ? requested_reserve : std::max(gib, ceil_div(payload, 10));
    U const required = add(payload, reserve);
    auto const available = fs::space(p).available;
    return {p, payload, reserve, required, static_cast<U>(available), available >= required};
}
} // namespace

std::vector<SourceSnapshot> discover(fs::path const &directory, std::string const &prefix,
                                     std::string const &suffix) {
    if (prefix.empty() || suffix.empty()) {
        throw std::invalid_argument("master prefix and suffix must be nonempty");
    }
    fs::path const dir = fs::canonical(directory);
    if (!fs::is_directory(dir)) {
        throw std::runtime_error("source is not a directory: " + dir.string());
    }
    std::map<size_t, fs::path> found;
    for (auto const &entry : fs::directory_iterator(dir)) {
        std::string const name = entry.path().filename().string();
        if (name.size() <= prefix.size() + suffix.size() ||
            name.compare(0, prefix.size(), prefix) != 0 ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
            continue;
        }
        std::string const number =
            name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
        if (!std::all_of(number.begin(), number.end(),
                         [](unsigned char c) { return c >= '0' && c <= '9'; })) {
            continue;
        }
        size_t index = 0;
        auto const parsed = std::from_chars(number.data(), number.data() + number.size(), index);
        if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size()) {
            throw std::runtime_error("Silo index out of range: " + name);
        }
        if (!entry.is_regular_file()) {
            throw std::runtime_error("master is not a regular file: " + entry.path().string());
        }
        if (!found.emplace(index, entry.path()).second) {
            throw std::runtime_error("duplicate numeric Silo index: " + number);
        }
    }
    std::vector<SourceSnapshot> result;
    result.reserve(found.size());
    for (auto const &[index, path] : found) {
        result.push_back({index, path});
    }
    return result;
}

std::vector<Snapshot> inventory(std::vector<SourceSnapshot> const &selected,
                                std::vector<std::string> const &fields) {
    if (selected.empty() || fields.empty()) {
        throw std::invalid_argument("select snapshots and at least one density field");
    }
    std::vector<Snapshot> output;
    std::set<size_t> seen_indices;
    for (auto const &source : selected) {
        if (!seen_indices.insert(source.index).second) {
            throw std::runtime_error("duplicate selected snapshot index");
        }
        Snapshot snapshot{
            source.index, stamp(source.master), {}, std::numeric_limits<double>::quiet_NaN()};
        File file(DBOpen(snapshot.master.path.string().c_str(), DB_UNKNOWN, DB_READ));
        if (!file) {
            throw std::runtime_error("cannot open Silo master: " + snapshot.master.path.string());
        }
        MultiMesh mesh(DBGetMultimesh(file.get(), "quadmesh"));
        if (!mesh || mesh->nblocks <= 0 || !mesh->meshnames) {
            throw std::runtime_error("missing explicit quadmesh multimesh: " +
                                     snapshot.master.path.string());
        }
        std::vector<MultiVar> vars;
        for (auto const &field : fields) {
            MultiVar var(DBGetMultivar(file.get(), field.c_str()));
            if (!var || var->nvars != mesh->nblocks || !var->varnames) {
                throw std::runtime_error("missing or inconsistent Silo multivar: " + field);
            }
            vars.push_back(std::move(var));
        }
        std::map<fs::path, FileStamp> references;
        std::unordered_map<std::string, fs::path> resolved;
        auto resolve = [&](std::string const &name) -> fs::path const & {
            auto it = resolved.find(name);
            if (it == resolved.end()) {
                FileStamp const entry = stamp(name);
                if (entry.path != snapshot.master.path) {
                    references.emplace(entry.path, entry);
                }
                it = resolved.emplace(name, entry.path).first;
            }
            return it->second;
        };
        for (int i = 0; i < mesh->nblocks; ++i) {
            if (!mesh->meshnames[i]) {
                throw std::runtime_error("missing multimesh reference");
            }
            auto const mr = reference(mesh->meshnames[i], snapshot.master.path);
            fs::path const mf = resolve(mr.first);
            for (size_t j = 0; j < vars.size(); ++j) {
                if (!vars[j]->varnames[i]) {
                    throw std::runtime_error("missing multivar block reference: " + fields[j]);
                }
                auto const vr = reference(vars[j]->varnames[i], snapshot.master.path);
                if (resolve(vr.first) != mf ||
                    fs::path(mr.second).parent_path() != fs::path(vr.second).parent_path()) {
                    throw std::runtime_error("variable block does not match its mesh: " +
                                             fields[j]);
                }
            }
        }
        for (auto const &[path, entry] : references) {
            (void)path;
            snapshot.files.push_back(entry);
        }
        if (DBGetComponentType(file.get(), "quadmesh", "dtime") == DB_DOUBLE) {
            void *p = DBGetComponent(file.get(), "quadmesh", "dtime");
            if (p) {
                snapshot.raw_time = *static_cast<double *>(p);
                std::free(p);
            }
        }
        output.push_back(std::move(snapshot));
    }
    return output;
}

fs::path validate_destination(std::vector<Snapshot> const &sources, fs::path const &path) {
    if (sources.empty()) {
        throw std::invalid_argument("destination validation needs at least one source");
    }
    fs::path const resolved = destination(path);
    fs::path const dest_ancestor = ancestor(resolved);
    require_local(dest_ancestor);
    auto const target_device = device(dest_ancestor);
    auto const target_disks = physical_disks(target_device);
    std::map<uintmax_t, std::set<fs::path>> source_disks;
    for (auto const &snap : sources) {
        auto check_source = [&](FileStamp const &f) {
            FileStamp const current = stamp(f.path);
            if (current.path != f.path || current.size != f.size ||
                current.mtime_ticks != f.mtime_ticks) {
                throw std::runtime_error("source changed after inventory: " + f.path.string());
            }
            fs::path const tree = current.path.parent_path();
            if (within(resolved, tree)) {
                throw std::runtime_error("destination inside a source tree: " + tree.string());
            }
            auto const source_device = device(current.path);
            if (source_device == target_device) {
                throw std::runtime_error("destination on a source filesystem: " +
                                         current.path.string());
            }
            auto it = source_disks.find(source_device);
            if (it == source_disks.end()) {
                it = source_disks.emplace(source_device, physical_disks(source_device)).first;
            }
            if (shares_disk(it->second, target_disks)) {
                throw std::runtime_error(
                    "destination shares a physical block device with a source: " +
                    current.path.string());
            }
        };
        check_source(snap.master);
        for (auto const &file : snap.files) {
            check_source(file);
        }
    }
    return resolved;
}

fs::path validate_export_destination(std::vector<fs::path> const &source_files,
                                     fs::path const &path) {
    if (source_files.empty()) {
        throw std::invalid_argument("export destination validation needs recorded source paths");
    }
    fs::path const resolved = destination(path);
    fs::path const dest_ancestor = ancestor(resolved);
    require_local(dest_ancestor);
    auto const target_device = device(dest_ancestor);
    auto const target_disks = physical_disks(target_device);
    std::map<uintmax_t, std::set<fs::path>> source_disks;
    for (auto const &source : source_files) {
        if (source.empty()) {
            throw std::invalid_argument("empty recorded source path");
        }
        fs::path const resolved_source = fs::weakly_canonical(fs::absolute(source));
        fs::path const source_tree = resolved_source.parent_path();
        if (within(resolved, source_tree)) {
            throw std::runtime_error("destination inside a recorded source tree: " +
                                     source_tree.string());
        }
        // If the original volume is offline this may be a parent mount, not the
        // original source device. It is still checked conservatively; the old
        // path cannot identify an unmounted filesystem or its old symlinks.
        fs::path const source_ancestor = ancestor(source_tree);
        auto const source_device = device(source_ancestor);
        if (source_device == target_device) {
            throw std::runtime_error("destination on recorded source ancestor filesystem: " +
                                     source_ancestor.string());
        }
        auto it = source_disks.find(source_device);
        if (it == source_disks.end()) {
            it = source_disks.emplace(source_device, physical_disks(source_device)).first;
        }
        if (shares_disk(it->second, target_disks)) {
            throw std::runtime_error(
                "destination shares a physical block device with a recorded source ancestor: " +
                source_ancestor.string());
        }
    }
    return resolved;
}

SpacePlan preflight(std::vector<Snapshot> const &sources, fs::path const &scratch,
                    fs::path const &result, size_t frames, size_t width, size_t height, double fps,
                    uint64_t bitrate, uint64_t reserve_bytes, size_t completed_frames,
                    bool completed_movie) {
    if (sources.empty() || !frames || !width || !height || !std::isfinite(fps) || fps <= 0 ||
        !bitrate) {
        throw std::invalid_argument(
            "storage preflight needs sources, frames, dimensions, fps and bitrate");
    }
    if (completed_frames > frames) {
        throw std::invalid_argument("completed frames exceed total frames");
    }
    SpacePlan plan;
    plan.scratch = validate_destination(sources, scratch);
    plan.result = validate_destination(sources, result);
    if (within(plan.scratch, plan.result) || within(plan.result, plan.scratch)) {
        throw std::runtime_error("scratch and result paths overlap");
    }
    fs::path const sa = ancestor(plan.scratch), ra = ancestor(plan.result);
    auto const sd = device(sa), rd = device(ra);
    // PNG worst case: RGB scanlines, filter bytes, zlib bound allowance, chunks,
    // plus one extra in-progress frame and 1 MiB of metadata per frame.
    U const raw = add(mul(mul(width, height), 3), height);
    U const png = add(add(raw, ceil_div(raw, 100)), 65536);
    U constexpr mib = U(1) << 20;
    plan.estimated_png_bytes = png;
    size_t const remaining = frames - completed_frames;
    U const scratch_payload = add(mul(add(remaining, remaining ? 1 : 0), png), mul(remaining, mib));
    plan.estimated_frame_bytes = scratch_payload;
    // Bitrate targets do not cap an encoder. Also reserve the uncompressed
    // RGB-sized bound for every frame, whichever is larger.
    long double const duration_seconds =
        static_cast<long double>(frames) / static_cast<long double>(fps);
    long double const rate_bytes =
        std::ceil((1.5L * duration_seconds + 2.0L) * static_cast<long double>(bitrate) / 8.0L);
    if (!std::isfinite(rate_bytes) ||
        rate_bytes > static_cast<long double>(std::numeric_limits<U>::max())) {
        throw std::overflow_error("movie bitrate estimate overflow");
    }
    U const rate_estimate = add(static_cast<U>(rate_bytes), 16 * mib);
    plan.estimated_movie_bytes = std::max(rate_estimate, mul(png, frames));
    U const result_payload = add(completed_movie ? 0 : plan.estimated_movie_bytes, 16 * mib);
    plan.same_device = sd == rd;
    if (plan.same_device) {
        plan.volumes.push_back(volume(sa, add(scratch_payload, result_payload), reserve_bytes));
    } else {
        plan.volumes.push_back(volume(sa, scratch_payload, reserve_bytes));
        plan.volumes.push_back(volume(ra, result_payload, reserve_bytes));
    }
    plan.enough = std::all_of(plan.volumes.begin(), plan.volumes.end(),
                              [](SpaceVolume const &v) { return v.enough; });
    return plan;
}

} // namespace marching_cubes

#include "install.h"
#include "releases.h"
#include "../platform/sha1.h"

#include <nod.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace fs = std::filesystem;

const wchar_t* const kInstallImageFilter = L"*.iso;*.rvz;*.wia;*.wbfs;*.ciso;*.gcz;*.nfs";

namespace
{
constexpr uint64_t kUnencryptedSize = 0x50000;

struct NodDeleter { void operator()(NodHandle* h) const { nod_free(h); } };
using NodPtr = std::unique_ptr<NodHandle, NodDeleter>;

std::string NodError()
{
    const char* m = nod_error_message();
    return m ? m : "unknown error";
}

NodPtr OpenDisc(const fs::path& image, std::string& error)
{
    NodDiscOptions opt{};
    opt.preloader_threads = 1;   // read ahead while files are written
    NodHandle* h = nullptr;
    if (nod_disc_open((const char*)image.u8string().c_str(), &opt, &h) != NOD_RESULT_OK)
    {
        error = NodError();
        return nullptr;
    }
    return NodPtr(h);
}

bool ReadExact(NodHandle* h, uint8_t* out, size_t len)
{
    while (len)
    {
        int64_t n = nod_read(h, out, len);
        if (n <= 0) return false;
        out += n;
        len -= (size_t)n;
    }
    return true;
}

bool WriteFile(const fs::path& p, const void* data, size_t len)
{
    FILE* f = _wfopen(p.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

bool Sha1Of(const void* data, size_t len, uint8_t out[20])
{
    return Sha1_Hash(data, len, out);
}

struct FstEntry
{
    uint32_t index;
    bool dir;
    uint32_t size;   // file size, or the end index for directories
    std::string name;
};

uint32_t CollectFst(uint32_t index, NodNodeKind kind, const char* name, uint32_t size, void* user)
{
    ((std::vector<FstEntry>*)user)->push_back({ index, kind == NOD_NODE_KIND_DIRECTORY, size, name });
    return index + 1;
}
}   // namespace

std::string Install_GameId(const fs::path& image)
{
    std::string error;
    NodPtr disc = OpenDisc(image, error);
    NodDiscHeader h{};
    if (!disc || nod_disc_header(disc.get(), &h) != NOD_RESULT_OK) return {};
    return std::string(h.game_id, 6);
}

InstallResult Install_Run(const fs::path& image, const fs::path& dest, InstallProgress& progress, std::string& error)
{
    NodPtr disc = OpenDisc(image, error);
    if (!disc) return InstallResult::OpenFailed;
    NodDiscHeader header{};
    if (nod_disc_header(disc.get(), &header) != NOD_RESULT_OK)
    {
        error = NodError();
        return InstallResult::OpenFailed;
    }
    const GameRelease* rel = Release_Find(std::string(header.game_id, 6));
    if (!rel)
    {
        error = "the disc is " + std::string(header.game_id, 6) + ", not Sonic and the Black Knight (" + kReleaseNames + ")";
        return InstallResult::WrongGame;
    }

    // unencrypted area (disc header, partition table, region info) for raw disc reads
    std::vector<uint8_t> raw(kUnencryptedSize);
    if (nod_seek(disc.get(), 0, 0) != 0 || !ReadExact(disc.get(), raw.data(), raw.size()))
    {
        error = "cannot read the disc header: " + NodError();
        return InstallResult::OpenFailed;
    }

    NodHandle* partRaw = nullptr;
    if (nod_disc_open_partition_kind(disc.get(), NOD_PARTITION_KIND_DATA, nullptr, &partRaw) != NOD_RESULT_OK)
    {
        error = "cannot open the data partition: " + NodError();
        return InstallResult::OpenFailed;
    }
    NodPtr part(partRaw);
    NodPartitionMeta meta{};
    if (nod_partition_meta(part.get(), &meta) != NOD_RESULT_OK || !meta.raw_dol.data)
    {
        error = "cannot read the partition headers: " + NodError();
        return InstallResult::OpenFailed;
    }
    // the recompiled code is generated from exactly this main.dol
    uint8_t sha[20];
    char hex[41] = {};
    const KnownFile* dol = nullptr;
    for (auto& k : std::span(rel->files, rel->fileCount))
        if (strcmp(k.path, "sys/main.dol") == 0) dol = &k;
    if (Sha1Of(meta.raw_dol.data, meta.raw_dol.size, sha))
        for (int i = 0; i < 20; i++) snprintf(hex + 2 * i, 3, "%02x", sha[i]);
    if (!dol || strcmp(hex, dol->sha1) != 0)
    {
        error = std::string("this version of the game is not supported (main.dol differs from the ") + rel->name +
                " release this build is made for)";
        return InstallResult::WrongVersion;
    }

    std::vector<FstEntry> fst;
    nod_partition_iterate_fst(part.get(), CollectFst, &fst);
    uint64_t total = 0;
    for (auto& e : fst)
        if (!e.dir) total += e.size;
    progress.total = total;
    progress.done = 0;

    fs::path tmp = dest;
    tmp += ".partial";
    std::error_code ec;
    fs::remove_all(tmp, ec);   // only ever our own leftover from an interrupted install
    // removes the partial output on failure or cancel (after the final rename it is gone)
    struct PartialGuard
    {
        fs::path p;
        ~PartialGuard() { std::error_code e; fs::remove_all(p, e); }
    } guard{ tmp };
    fs::create_directories(tmp / "sys", ec);
    fs::create_directories(tmp / "disc", ec);
    fs::create_directories(tmp / "files", ec);
    if (ec)
    {
        error = "cannot create " + tmp.string() + ": " + ec.message();
        return InstallResult::WriteFailed;
    }

    struct Blob { const char* path; NodBlob blob; };
    const Blob blobs[] = {
        { "sys/boot.bin", meta.raw_boot },       { "sys/bi2.bin", meta.raw_bi2 },
        { "sys/apploader.img", meta.raw_apploader }, { "sys/main.dol", meta.raw_dol },
        { "sys/fst.bin", meta.raw_fst },         { "ticket.bin", meta.raw_ticket },
        { "tmd.bin", meta.raw_tmd },             { "cert.bin", meta.raw_cert_chain },
        { "h3.bin", meta.raw_h3_table },
    };
    bool ok = true;
    for (auto& b : blobs)
        if (b.blob.data) ok &= WriteFile(tmp / b.path, b.blob.data, b.blob.size);
    ok &= WriteFile(tmp / "disc/header.bin", raw.data(), 0x100);
    ok &= WriteFile(tmp / "disc/region.bin", raw.data() + 0x4E000, 0x20);
    ok &= WriteFile(tmp / "disc/unencrypted.bin", raw.data(), raw.size());
    if (!ok)
    {
        error = "cannot write to " + tmp.string();
        return InstallResult::WriteFailed;
    }

    // files/: directories carry the index one past their last child
    std::vector<std::pair<uint32_t, fs::path>> dirs = { { UINT32_MAX, tmp / "files" } };
    std::vector<uint8_t> buf(1 << 20);
    for (auto& e : fst)
    {
        while (dirs.size() > 1 && e.index >= dirs.back().first) dirs.pop_back();
        fs::path p = dirs.back().second / fs::path(std::u8string(e.name.begin(), e.name.end()));
        if (e.dir)
        {
            fs::create_directories(p, ec);
            dirs.push_back({ e.size, p });
            continue;
        }
        if (progress.cancel) return InstallResult::Canceled;
        NodHandle* fileRaw = nullptr;
        if (nod_partition_open_file(part.get(), e.index, &fileRaw) != NOD_RESULT_OK)
        {
            error = "cannot read " + e.name + ": " + NodError();
            return InstallResult::OpenFailed;
        }
        NodPtr file(fileRaw);
        FILE* out = _wfopen(p.c_str(), L"wb");
        if (!out)
        {
            error = "cannot write " + p.string();
            return InstallResult::WriteFailed;
        }
        uint64_t left = e.size;
        while (left)
        {
            size_t n = (size_t)std::min<uint64_t>(left, buf.size());
            if (!ReadExact(file.get(), buf.data(), n))
            {
                fclose(out);
                error = "cannot read " + e.name + ": " + NodError();
                return InstallResult::OpenFailed;
            }
            if (fwrite(buf.data(), 1, n, out) != n)
            {
                fclose(out);
                error = "cannot write " + p.string() + " (disk full?)";
                return InstallResult::WriteFailed;
            }
            left -= n;
            progress.done += n;
            if (progress.cancel)
            {
                fclose(out);
                return InstallResult::Canceled;
            }
        }
        if (fclose(out) != 0)
        {
            error = "cannot write " + p.string() + " (disk full?)";
            return InstallResult::WriteFailed;
        }
    }

    fs::create_directories(dest.parent_path(), ec);
    // reinstall over an earlier (e.g. damaged) install: swap it out only once the new
    // copy is complete
    fs::path old = dest;
    old += ".old";
    bool replaced = false;
    if (fs::exists(dest, ec))
    {
        fs::remove_all(old, ec);
        fs::rename(dest, old, ec);
        if (ec)
        {
            error = "cannot replace " + dest.string() + ": " + ec.message();
            return InstallResult::WriteFailed;
        }
        replaced = true;
    }
    fs::rename(tmp, dest, ec);
    if (ec)
    {
        error = "cannot rename " + tmp.string() + " to " + dest.string() + ": " + ec.message();
        if (replaced) { std::error_code e2; fs::rename(old, dest, e2); }
        return InstallResult::WriteFailed;
    }
    if (replaced) fs::remove_all(old, ec);
    return InstallResult::Ok;
}

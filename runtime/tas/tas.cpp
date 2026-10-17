#include "tas.h"
#include "../cpu/cpu.h"
#include "../input/gamepad.h"
#include "../kernel/memory.h"
#include "../log.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <tuple>
#include <vector>
#include <windows.h>

extern "C" uint32_t g_tasProbe[16] = {};
#ifdef BKR_CALL_COUNTS
extern "C" uint32_t g_callCounts[0x200000] = {};
#endif

namespace
{
// ------------------------------------------------------------------ guest values
// Address expressions: hex constants, probes (p0..p15), "+"/"-" offsets and [expr]
// for reading a pointer, e.g. "[p2+0x10]+0x44".
bool ValidGuest(uint32_t a, uint32_t size)
{
    uint32_t seg = a >> 28, phys = a & 0x0FFFFFFF;
    if (seg == 0x8 || seg == 0xC) return phys + size <= MEM1_SIZE;
    if (seg == 0x9 || seg == 0xD) return phys + size <= MEM2_SIZE;
    return false;
}

bool EvalAddr(const std::string& s, size_t& i, uint32_t& out);

bool EvalTerm(const std::string& s, size_t& i, uint32_t& out)
{
    if (i < s.size() && s[i] == '[')
    {
        i++;
        uint32_t p;
        if (!EvalAddr(s, i, p) || i >= s.size() || s[i] != ']') return false;
        i++;
        if (!ValidGuest(p, 4)) return false;
        out = Read32(p);
        return true;
    }
    if (i < s.size() && (s[i] == 'p' || s[i] == 'P'))
    {
        size_t j = i + 1;
        int n = 0;
        while (j < s.size() && isdigit((unsigned char)s[j])) n = n * 10 + (s[j++] - '0');
        if (j == i + 1 || n >= 16) return false;
        i = j;
        out = g_tasProbe[n];
        return out != 0;
    }
    size_t j = i;
    if (s.compare(i, 2, "0x") == 0 || s.compare(i, 2, "0X") == 0) j += 2;
    size_t k = j;
    while (k < s.size() && isxdigit((unsigned char)s[k])) k++;
    if (k == j) return false;
    out = (uint32_t)strtoul(s.substr(j, k - j).c_str(), nullptr, 16);
    i = k;
    return true;
}

bool EvalAddr(const std::string& s, size_t& i, uint32_t& out)
{
    if (!EvalTerm(s, i, out)) return false;
    while (i < s.size() && (s[i] == '+' || s[i] == '-'))
    {
        char op = s[i++];
        uint32_t v;
        if (!EvalTerm(s, i, v)) return false;
        out = op == '+' ? out + v : out - v;
    }
    return true;
}

enum class VType { U8, S8, U16, S16, U32, S32, F32 };

bool ParseType(const std::string& t, VType& v)
{
    static const struct { const char* n; VType v; } kTypes[] = {
        { "u8", VType::U8 }, { "s8", VType::S8 }, { "u16", VType::U16 }, { "s16", VType::S16 },
        { "u32", VType::U32 }, { "s32", VType::S32 }, { "f32", VType::F32 },
    };
    for (auto& k : kTypes)
        if (t == k.n) { v = k.v; return true; }
    return false;
}

// Reads a typed value; false if the address does not resolve to guest RAM.
bool ReadValue(VType t, const std::string& expr, double& out)
{
    size_t i = 0;
    uint32_t a;
    if (!EvalAddr(expr, i, a) || i != expr.size()) return false;
    uint32_t size = (t == VType::U8 || t == VType::S8) ? 1 : (t == VType::U16 || t == VType::S16) ? 2 : 4;
    if (!ValidGuest(a, size)) return false;
    switch (t)
    {
    case VType::U8: out = Read8(a); break;
    case VType::S8: out = (int8_t)Read8(a); break;
    case VType::U16: out = Read16(a); break;
    case VType::S16: out = (int16_t)Read16(a); break;
    case VType::U32: out = Read32(a); break;
    case VType::S32: out = (int32_t)Read32(a); break;
    case VType::F32: { uint32_t u = Read32(a); float f; memcpy(&f, &u, 4); out = f; break; }
    }
    return true;
}

// ------------------------------------------------------------------ script
enum class Op { Wait, Hold, Tap, Mash, Set, Until, Check, Log, Shot, Dump, Calls, Repeat, End, Quit };

struct Input
{
    uint16_t buttons = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0, px = -1, py = -1;
    bool stick = false;
};

struct Cond
{
    bool file = false;     // "file <substring>"
    std::string text;      // file substring or address expression
    VType type = VType::U32;
    std::string op;
    double value = 0;
};

struct Cmd
{
    Op op;
    double secs = 0, period = 0;
    Input input;
    bool mash = false;
    std::vector<Cond> conds;   // all must hold
    std::string text;
    int count = 0, match = -1;
    int line = 0;
};

struct Watch
{
    std::string name, expr;
    VType type;
};

std::vector<Cmd> s_cmds;
std::vector<Watch> s_watches;
bool s_active = false;
size_t s_pc = 0;
double s_cmdStart = -1;
Input s_background;
std::vector<int> s_repeatLeft;
std::mutex s_fileMutex;
std::vector<std::string> s_fileReads;
size_t s_fileMark = 0;
std::mutex s_shotMutex;
std::string s_shotName;
FILE* s_csv = nullptr;
std::string s_csvPath;
std::atomic<uint32_t> s_marker{ 0 };
std::string s_markerText;
std::mutex s_markerMutex;
std::filesystem::path s_outDir;

double Now() { return (double)Timebase_Now() / (double)TB_FREQ; }

bool ParseInputToken(const std::string& word, Input& in)
{
    std::istringstream parts(word);
    std::string tok;
    bool ok = true;
    while (std::getline(parts, tok, '+'))
    {
        auto eq = tok.find('=');
        if (eq != std::string::npos)
        {
            float v = std::stof(tok.substr(eq + 1));
            std::string k = tok.substr(0, eq);
            if (k == "PX") in.px = v;
            else if (k == "PY") in.py = v;
            else
            {
                in.stick = true;
                if (k == "LX") in.lx = v; else if (k == "LY") in.ly = v; else if (k == "RX") in.rx = v;
                else if (k == "RY") in.ry = v; else if (k == "RT") in.rt = v; else if (k == "LT") in.lt = v;
                else ok = false;
            }
            continue;
        }
        static const struct { const char* n; uint16_t b; } kNames[] = {
            { "A", PAD_A }, { "B", PAD_B }, { "X", PAD_X }, { "Y", PAD_Y }, { "LB", PAD_LB }, { "RB", PAD_RB },
            { "START", PAD_START }, { "BACK", PAD_BACK }, { "UP", PAD_DPAD_UP }, { "DOWN", PAD_DPAD_DOWN },
            { "LEFT", PAD_DPAD_LEFT }, { "RIGHT", PAD_DPAD_RIGHT }, { "HOME", PAD_HOME }, { "LS", PAD_LTHUMB },
            { "RS", PAD_RTHUMB },
        };
        bool found = false;
        for (auto& n : kNames)
            if (tok == n.n) { in.buttons |= n.b; found = true; }
        if (!found && tok != "NONE") ok = false;
    }
    return ok;
}

bool ParseCond(std::istringstream& ss, std::string first, Cond& c)
{
    if (first == "file") { c.file = true; return (bool)(ss >> c.text); }
    if (!ParseType(first, c.type)) return false;
    return (bool)(ss >> c.text >> c.op >> c.value);
}

bool TestCond(const Cond& c)
{
    if (c.file)
    {
        std::lock_guard lk(s_fileMutex);
        for (size_t i = s_fileMark; i < s_fileReads.size(); i++)
            if (s_fileReads[i].find(c.text) != std::string::npos) return true;
        return false;
    }
    double v;
    if (!ReadValue(c.type, c.text, v)) return false;
    const std::string& o = c.op;
    if (o == "==") return v == c.value;
    if (o == "!=") return v != c.value;
    if (o == "<") return v < c.value;
    if (o == "<=") return v <= c.value;
    if (o == ">") return v > c.value;
    if (o == ">=") return v >= c.value;
    return false;
}

void LoadFile(const std::filesystem::path& path, int depth)
{
    std::ifstream f(path);
    if (!f) FATAL("TAS: cannot open {}", path.string());
    std::string line;
    int n = 0;
    while (std::getline(f, line))
    {
        n++;
        if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
        std::istringstream ss(line);
        std::string w;
        if (!(ss >> w)) continue;
        auto bad = [&]() { FATAL("TAS: {}:{}: cannot parse '{}'", path.filename().string(), n, line); };
        Cmd c{};
        c.line = n;
        if (w == "include")
        {
            std::string inc;
            ss >> inc;
            if (depth > 8) bad();
            LoadFile(path.parent_path() / inc, depth + 1);
            continue;
        }
        if (w == "watch")
        {
            Watch wt;
            std::string t;
            if (!(ss >> wt.name >> t >> wt.expr) || !ParseType(t, wt.type)) bad();
            s_watches.push_back(wt);
            continue;
        }
        if (w == "wait") { c.op = Op::Wait; if (!(ss >> c.secs)) bad(); }
        else if (w == "hold" || w == "tap" || w == "set" || w == "mash")
        {
            c.op = w == "hold" ? Op::Hold : w == "tap" ? Op::Tap : w == "set" ? Op::Set : Op::Mash;
            if (c.op == Op::Hold && !(ss >> c.secs)) bad();
            std::string tok;
            while (ss >> tok)
            {
                if (isdigit((unsigned char)tok[0]) || tok[0] == '.')
                {
                    // tap INPUT [secs], mash INPUT secs [period]
                    if (c.secs == 0 || c.op == Op::Hold) c.secs = std::stod(tok);
                    else c.period = std::stod(tok);
                    continue;
                }
                if (!ParseInputToken(tok, c.input)) bad();
            }
            if (c.op == Op::Tap && c.secs == 0) c.secs = 0.1;
            if (c.op == Op::Mash && c.secs == 0) bad();
            if (c.period == 0) c.period = 0.2;
        }
        else if (w == "until" || w == "check")
        {
            c.op = w == "until" ? Op::Until : Op::Check;
            c.secs = 60;
            std::string tok;
            while (ss >> tok)
            {
                if (tok == "timeout") { if (!(ss >> c.secs)) bad(); continue; }
                if (tok == "mash") { std::string in; if (!(ss >> in) || !ParseInputToken(in, c.input)) bad(); c.mash = true; c.period = 0.2; continue; }
                if (tok == "and") continue;
                if (c.op == Op::Check && tok == "msg") { std::getline(ss, c.text); break; }
                Cond cd;
                if (!ParseCond(ss, tok, cd)) bad();
                c.conds.push_back(cd);
            }
            if (c.conds.empty()) bad();
        }
        else if (w == "log" || w == "shot" || w == "dump" || w == "calls")
        {
            c.op = w == "log" ? Op::Log : w == "shot" ? Op::Shot : w == "dump" ? Op::Dump : Op::Calls;
            std::getline(ss, c.text);
            while (!c.text.empty() && c.text[0] == ' ') c.text.erase(0, 1);
            if (c.op != Op::Log && c.text.empty()) bad();
        }
        else if (w == "repeat") { c.op = Op::Repeat; if (!(ss >> c.count)) bad(); }
        else if (w == "end") c.op = Op::End;
        else if (w == "quit") { c.op = Op::Quit; ss >> c.count; }
        else bad();
        s_cmds.push_back(c);
    }
}

void Marker(const std::string& text)
{
    LOG_INFO("TAS: {}", text);
    std::lock_guard lk(s_markerMutex);
    s_markerText = text;
    s_marker++;
}

[[noreturn]] void Quit(int code)
{
    LOG_INFO("TAS: quit ({})", code);
    if (s_csv) fflush(s_csv);
    Gamepad_StopRumble();
    TerminateProcess(GetCurrentProcess(), (UINT)code);
    for (;;) {}
}

void DumpMemory(const std::string& name)
{
    std::filesystem::create_directories(s_outDir / "dumps");
    for (auto [suffix, addr, size] : { std::tuple{ ".mem1", 0x80000000u, MEM1_SIZE }, std::tuple{ ".mem2", 0x90000000u, MEM2_SIZE } })
    {
        std::filesystem::path p = s_outDir / "dumps" / (name + suffix);
        if (FILE* f = _wfopen(p.c_str(), L"wb"))
        {
            fwrite(GuestPtr(addr), 1, size, f);
            fclose(f);
        }
    }
    LOG_INFO("TAS: dumped memory as {}", name);
}

// "calls NAME": writes the call counters gathered since the previous "calls" (or since
// boot) with the elapsed guest time to dumps/NAME.calls, then resets them. "calls -"
// only resets. Needs a BKR_CALL_COUNTS build.
void DumpCalls(const std::string& name)
{
#ifdef BKR_CALL_COUNTS
    static double since = 0;
    double now = Now();
    if (name != "-")
    {
        std::filesystem::create_directories(s_outDir / "dumps");
        std::filesystem::path p = s_outDir / "dumps" / (name + ".calls");
        if (FILE* f = _wfopen(p.c_str(), L"wb"))
        {
            double secs = now - since;
            fwrite(&secs, sizeof(secs), 1, f);
            fwrite(g_callCounts, sizeof(g_callCounts), 1, f);
            fclose(f);
        }
        LOG_INFO("TAS: call counts {} over {:.2f} s", name, now - since);
    }
    memset(g_callCounts, 0, sizeof(g_callCounts));
    since = now;
#else
    LOG_WARN("TAS: 'calls {}' needs a BKR_CALL_COUNTS build", name);
#endif
}

void Merge(GamepadState& out, const Input& in)
{
    out.buttons |= in.buttons;
    if (in.stick) { out.lx = in.lx; out.ly = in.ly; out.rx = in.rx; out.ry = in.ry; out.lt = in.lt; out.rt = in.rt; }
    if (in.px >= 0) out.px = in.px;
    if (in.py >= 0) out.py = in.py;
}

// Runs commands until one needs more time; returns that command's input contribution.
void Step(GamepadState& out)
{
    double now = Now();
    for (int guard = 0; guard < 10000 && s_pc < s_cmds.size(); guard++)
    {
        Cmd& c = s_cmds[s_pc];
        bool fresh = s_cmdStart < 0;
        if (fresh)
        {
            s_cmdStart = now;
            std::lock_guard lk(s_fileMutex);
            s_fileMark = s_fileReads.size();
        }
        double el = now - s_cmdStart;
        auto next = [&]() { s_pc++; s_cmdStart = -1; };
        switch (c.op)
        {
        case Op::Wait:
            if (el < c.secs) return;
            next();
            continue;
        case Op::Hold:
            if (el < c.secs) { Merge(out, c.input); return; }
            next();
            continue;
        case Op::Tap:
            if (el < c.secs) { Merge(out, c.input); return; }
            if (el < c.secs + 0.1) return;
            next();
            continue;
        case Op::Mash:
            if (el < c.secs) { if (fmod(el, c.period) < c.period / 2) Merge(out, c.input); return; }
            next();
            continue;
        case Op::Set:
            s_background = c.input;
            next();
            continue;
        case Op::Until:
        {
            bool all = true;
            for (auto& cd : c.conds) all = all && TestCond(cd);
            if (all) { LOG_INFO("TAS: line {} satisfied after {:.2f} s", c.line, el); next(); continue; }
            if (el >= c.secs)
            {
                LOG_WARN("TAS: FAIL line {}: condition not met within {:.0f} s", c.line, c.secs);
                Quit(3);
            }
            if (c.mash && fmod(el, c.period) < c.period / 2) Merge(out, c.input);
            return;
        }
        case Op::Check:
        {
            bool all = true;
            for (auto& cd : c.conds) all = all && TestCond(cd);
            LOG_INFO("TAS: {} line {}{}", all ? "PASS" : "FAIL", c.line, c.text.empty() ? "" : ":" + c.text);
            next();
            continue;
        }
        case Op::Log: Marker(c.text); next(); continue;
        case Op::Shot: { std::lock_guard lk(s_shotMutex); s_shotName = c.text; } next(); continue;
        case Op::Dump: DumpMemory(c.text); next(); continue;
        case Op::Calls: DumpCalls(c.text); next(); continue;
        case Op::Repeat:
            s_repeatLeft.push_back(c.count);
            next();
            continue;
        case Op::End:
        {
            if (s_repeatLeft.empty()) { next(); continue; }
            if (--s_repeatLeft.back() > 0)
            {
                // jump back to the matching repeat
                int depth = 0;
                size_t j = s_pc;
                while (j-- > 0)
                {
                    if (s_cmds[j].op == Op::End) depth++;
                    else if (s_cmds[j].op == Op::Repeat && depth-- == 0) break;
                }
                s_pc = j + 1;
                s_cmdStart = -1;
            }
            else
            {
                s_repeatLeft.pop_back();
                next();
            }
            continue;
        }
        case Op::Quit: Quit(c.count);
        }
    }
    if (s_pc >= s_cmds.size())
    {
        Marker("script finished");
        Quit(0);
    }
}
}   // namespace

void Tas_Load(const char* path)
{
    std::error_code ec;
    std::filesystem::path p = std::filesystem::absolute(path, ec);
    if (!*path || ec || !std::filesystem::exists(p, ec)) FATAL("TAS: script '{}' not found", path);
    try
    {
        LoadFile(p, 0);
    }
    catch (const std::exception& e)   // e.g. a malformed number in the script
    {
        FATAL("TAS: cannot load {}: {}", p.string(), e.what());
    }
    s_active = true;
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    s_outDir = std::filesystem::path(buf).parent_path();
    LOG_INFO("TAS: loaded {} ({} commands, {} watches)", p.string(), s_cmds.size(), s_watches.size());
}

void Tas_SetTelemetry(const char* csvPath) { s_csvPath = csvPath; }

bool Tas_Active() { return s_active; }

bool Tas_Poll(GamepadState& out)
{
    if (!s_active) return false;
    out = GamepadState();
    out.connected = true;
    Step(out);
    Merge(out, s_background);
    return true;
}

void Tas_OnPresent(void (*saveShot)(const char* path))
{
    if (!s_active) return;
    std::string shot;
    {
        std::lock_guard lk(s_shotMutex);
        shot.swap(s_shotName);
    }
    if (!shot.empty())
    {
        std::filesystem::create_directories(s_outDir / "shots");
        saveShot((s_outDir / "shots" / (shot + ".bmp")).string().c_str());
    }
    if (s_csvPath.empty()) return;
    if (!s_csv)
    {
        s_csv = _fsopen(s_csvPath.c_str(), "w", _SH_DENYWR);
        if (!s_csv) { s_csvPath.clear(); return; }
        fprintf(s_csv, "t,marker");
        for (auto& w : s_watches) fprintf(s_csv, ",%s", w.name.c_str());
        fprintf(s_csv, "\n");
    }
    static uint32_t lastMarker = 0;
    std::string marker;
    if (uint32_t m = s_marker.load(); m != lastMarker)
    {
        lastMarker = m;
        std::lock_guard lk(s_markerMutex);
        marker = s_markerText;
        for (char& ch : marker) if (ch == ',' || ch == '"') ch = ' ';
    }
    fprintf(s_csv, "%.4f,%s", Now(), marker.c_str());
    for (auto& w : s_watches)
    {
        double v;
        if (ReadValue(w.type, w.expr, v)) fprintf(s_csv, ",%.6g", v);
        else fprintf(s_csv, ",");
    }
    fprintf(s_csv, "\n");
    static int rows = 0;
    if (++rows % 60 == 0) fflush(s_csv);
}

void Tas_OnFileRead(const std::string& path)
{
    if (!s_active) return;
    LOG_INFO("TAS: file {}", path);
    std::lock_guard lk(s_fileMutex);
    s_fileReads.push_back(path);
}

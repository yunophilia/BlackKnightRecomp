#include "dsp_coef.h"

#include <cmath>

namespace
{
constexpr int N = 512;
constexpr double PI = 3.14159265358979323846;

// Modified Bessel function of the first kind, order 0 (numpy.i0 for the Kaiser window)
double I0(double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; ; k++)
    {
        double h = x / (2.0 * k);
        term *= h * h;
        sum += term;
        if (term < sum * 1e-17) return sum;
    }
}

double Sinc(double v) { return v == 0.0 ? 1.0 : std::sin(PI * v) / (PI * v); }

// 512 taps -> 128 phases x 4 taps, normalized so the largest phase sums to 32767
void Convert(const double* c, int16_t* out)
{
    double m = -1e300;
    for (int p = 0; p < 128; p++)
    {
        double s = 0;
        for (int t = 0; t < 4; t++) s += c[t * 128 + 127 - p];
        if (s > m) m = s;
    }
    for (int p = 0; p < 128; p++)
        for (int t = 0; t < 4; t++)
            out[p * 4 + t] = (int16_t)(uint16_t)(int)std::nearbyint(c[t * 128 + 127 - p] / m * 32767.0);
}
}   // namespace

void DSP_GenerateCoefs(int16_t out[0x800])
{
    double c1[N], c2[N], c3[N];
    const double beta = PI * 9.0 / 4.0, i0b = I0(beta);
    for (int n = 0; n < N; n++)
    {
        double x = -2.0 + 4.0 * n / (N - 1);
        double hamming = 0.54 - 0.46 * std::cos(2.0 * PI * n / (N - 1));
        double r = 2.0 * n / (N - 1) - 1.0;
        double kaiser = I0(beta * std::sqrt(std::fmax(0.0, 1.0 - r * r))) / i0b;
        c1[n] = Sinc(x * 0.5) * hamming;
        c2[n] = Sinc(x * 0.75) * kaiser;
        c3[n] = Sinc(x) * hamming;
    }
    Convert(c1, out);
    Convert(c2, out + 0x200);
    Convert(c3, out + 0x400);
    for (int i = 0x600; i < 0x800; i++) out[i] = 0;

    // values the GBA ucode needs (same patch list as Dolphin's script)
    static const uint16_t gba[][2] = {
        { 0x03b, 0x0065 }, { 0x043, 0x0076 }, { 0x0ca, 0x3461 }, { 0x0e2, 0x376f },
        { 0x1b8, 0x007f }, { 0x1f8, 0x0009 }, { 0x1fc, 0x0003 }, { 0x229, 0x657c },
        { 0x231, 0x64fc }, { 0x259, 0x6143 }, { 0x285, 0x5aff }, { 0x456, 0x102f },
        { 0x468, 0xf808 }, { 0x491, 0x6a0f }, { 0x5f1, 0x0200 }, { 0x5f6, 0x7f65 },
        { 0x65b, 0x0000 }, { 0x66b, 0x0000 }, { 0x66c, 0x06f2 }, { 0x6fe, 0x0008 },
        { 0x723, 0xffe0 }, { 0x766, 0x0273 },
    };
    for (auto& p : gba) out[p[0]] = (int16_t)p[1];
}

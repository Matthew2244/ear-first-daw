#include "capture/ReturnFinder.h"
#include <cmath>
#include <complex>
#include <algorithm>

namespace earfirst
{

namespace
{
using cpx = std::complex<double>;
constexpr double pi = 3.14159265358979323846;

void fft (std::vector<cpx>& a, bool inverse)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = 2 * pi / (double) len * (inverse ? 1 : -1);
        const cpx wl (std::cos (ang), std::sin (ang));
        for (size_t i = 0; i < n; i += len)
        {
            cpx w (1);
            for (size_t k = 0; k < len / 2; ++k)
            {
                const cpx u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse)
        for (auto& x : a) x /= (double) n;
}

size_t pow2AtLeast (size_t n)   { size_t p = 1; while (p < n) p <<= 1; return p; }
} // namespace

ReturnCheck checkReturn (const float* outRaw, const float* inRaw, int len, int maxLag, double sampleRate)
{
    ReturnCheck r;
    if (len < 1024 || maxLag < 0) return r;

    // Take out any steady offset first: some interfaces put DC on an input
    // (channel 64 on a StudioLive 64S sits at -27 dB), and it would swamp the
    // comparison. A two-pole high-pass at 20 Hz, run forward over each.
    std::vector<float> inBuf ((size_t) len), outBuf ((size_t) (len + maxLag));
    auto highPass = [sampleRate] (const float* src, float* dst, int n)
    {
        const double rc = 1.0 / (2 * pi * 20.0), dt = 1.0 / sampleRate, a = rc / (rc + dt);
        double mean = 0;
        for (int i = 0; i < std::min (n, 4800); ++i) mean += src[i];
        mean /= std::min (n, 4800);
        double px = mean, py = 0, px2 = 0, py2 = 0;
        for (int i = 0; i < n; ++i)
        {
            const double x = src[i];
            const double y = a * (py + x - px); px = x; py = y;
            const double y2 = a * (py2 + y - px2); px2 = y; py2 = y2;
            dst[i] = (float) y2;
        }
    };
    highPass (inRaw, inBuf.data(), len);
    highPass (outRaw, outBuf.data(), len + maxLag);
    const float* in = inBuf.data();
    const float* out = outBuf.data();

    double eIn = 0, eOut = 0;
    for (int i = 0; i < len; ++i) eIn += (double) in[i] * in[i];
    for (int i = 0; i < len; ++i) eOut += (double) out[maxLag + i] * out[maxLag + i];
    // Below about -70 dB there's nothing to compare.
    if (eIn / len < 1.0e-7 || eOut / len < 1.0e-7)
    {
        // The output might still have had sound a little earlier; check the whole span.
        double eAll = 0;
        for (int i = 0; i < len + maxLag; ++i) eAll += (double) out[i] * out[i];
        if (eIn / len < 1.0e-7 || eAll / (len + maxLag) < 1.0e-7) return r;
    }
    r.enoughSignal = true;

    // Cross-correlation for every delay at once: c[k] = sum in[j] * out[j + k],
    // and a delay of L samples is k = maxLag - L.
    const size_t n = pow2AtLeast ((size_t) (2 * len + maxLag));
    std::vector<cpx> A (n), B (n);
    for (int i = 0; i < len; ++i) A[(size_t) i] = in[i];
    for (int i = 0; i < len + maxLag; ++i) B[(size_t) i] = out[i];
    fft (A, false); fft (B, false);
    for (size_t i = 0; i < n; ++i) A[i] = std::conj (A[i]) * B[i];
    fft (A, true);

    // Output energy over each window, for a correlation that's 0 to 1.
    std::vector<double> prefix ((size_t) (len + maxLag + 1), 0.0);
    for (int i = 0; i < len + maxLag; ++i) prefix[(size_t) i + 1] = prefix[(size_t) i] + (double) out[i] * out[i];

    double best = -1; int bestK = maxLag;
    for (int k = 0; k <= maxLag; ++k)
    {
        const double eo = prefix[(size_t) (k + len)] - prefix[(size_t) k];
        if (eo <= 0) continue;
        const double rho = A[(size_t) k].real() / std::sqrt (eIn * eo);
        if (rho > best) { best = rho; bestK = k; }
    }
    r.correlation = std::max (0.0, best);
    r.lag = maxLag - bestK;

    // Level difference per octave band, output lined up at that delay.
    const size_t m = pow2AtLeast ((size_t) len);
    std::vector<cpx> X (m), O (m);
    for (int i = 0; i < len; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2 * pi * i / (len - 1));
        X[(size_t) i] = in[i] * w;
        O[(size_t) i] = out[bestK + i] * w;
    }
    fft (X, false); fft (O, false);

    std::vector<double> ex, eo;
    // Third-octave bands: fine enough that a room's comb filtering shows.
    const double edge = std::pow (2.0, 1.0 / 6.0);
    for (double centre = 100; centre * edge < sampleRate / 2 && centre < 16000; centre *= std::pow (2.0, 1.0 / 3.0))
    {
        const auto lo = (size_t) std::max (1.0, centre / edge * m / sampleRate);
        const auto hi = (size_t) std::min ((double) m / 2, centre * edge * m / sampleRate);
        double sx = 0, so = 0;
        for (size_t b = lo; b < hi; ++b) { sx += std::norm (X[b]); so += std::norm (O[b]); }
        ex.push_back (sx); eo.push_back (so);
    }
    const double loudest = eo.empty() ? 0 : *std::max_element (eo.begin(), eo.end());
    double lowDb = 1e9, highDb = -1e9;
    for (size_t b = 0; b < eo.size(); ++b)
        if (eo[b] > loudest * 1.0e-4 && eo[b] > 0)   // bands within 40 dB of the loudest
        {
            const double db = 10 * std::log10 (std::max (ex[b], 1.0e-30) / eo[b]);
            lowDb = std::min (lowDb, db); highDb = std::max (highDb, db);
            ++r.bands;
        }
    r.spreadDb = r.bands > 0 ? highDb - lowDb : 0;
    r.isReturn = r.correlation > 0.9 && r.bands >= 10 && r.spreadDb < 3.0;
    return r;
}

std::vector<float> makeSweep (double sampleRate, double seconds, float level)
{
    const int n = (int) (sampleRate * seconds);
    const double f0 = 80, f1 = std::min (14000.0, sampleRate * 0.4);
    const double k = std::log (f1 / f0);
    std::vector<float> s ((size_t) n);
    const int fade = (int) (sampleRate * 0.01);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / sampleRate;
        const double phase = 2 * pi * f0 * seconds / k * (std::exp (t / seconds * k) - 1);
        double g = level;
        if (i < fade) g *= (double) i / fade;
        if (i > n - fade) g *= (double) (n - i) / fade;
        s[(size_t) i] = (float) (g * std::sin (phase));
    }
    return s;
}

} // namespace earfirst

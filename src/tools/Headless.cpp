#include "Headless.h"

#include "Biquad.h"
#include "Decimate.h"
#include "DeviceWorker.h"
#include "EmotiBitDiscovery.h"
#include "HeartRate.h"
#include "RateMeter.h"
#include "Readouts.h"
#include "Retimer.h"
#include "RingBuffer.h"
#include "SignalSpec.h"

#include "board_shim.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

volatile std::sig_atomic_t g_interruptRequested = 0;

namespace
{

using SteadyClock = std::chrono::steady_clock;

void pumpFor (double seconds)
{
    const auto end = SteadyClock::now () + std::chrono::duration<double> (seconds);
    while (SteadyClock::now () < end && !g_interruptRequested)
    {
        QCoreApplication::processEvents (QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for (std::chrono::milliseconds (3));
    }
}

template <class Pred> bool pumpUntil (Pred pred, double timeoutSec)
{
    const auto end = SteadyClock::now () + std::chrono::duration<double> (timeoutSec);
    while (!pred ())
    {
        if (SteadyClock::now () >= end)
            return false;
        QCoreApplication::processEvents (QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for (std::chrono::milliseconds (3));
    }
    return true;
}

double since (SteadyClock::time_point t0)
{
    return std::chrono::duration<double> (SteadyClock::now () - t0).count ();
}

struct DeviceRun
{
    std::string name;
    DeviceKind kind = DeviceKind::Cyton;
    std::unique_ptr<DeviceWorker> worker = std::make_unique<DeviceWorker> ();
    bool connected = false, failed = false, disconnected = false, finished = false;
    QString info, error, discInfo;
    int progressCount = 0;
    SteadyClock::time_point startedAt;
    double connectSec = 0.0;
    std::vector<RateMeter> meters;
    std::vector<std::uint64_t> countAtStart;
    std::vector<std::string> problems;
    QStringList recFiles;
    bool recStopped = false;

    void resetFlags ()
    {
        connected = failed = disconnected = finished = false;
        recFiles.clear ();
        recStopped = false;
        info.clear ();
        error.clear ();
        discInfo.clear ();
        progressCount = 0;
    }
    bool resolved () const
    {
        return connected || failed || finished;
    }
};

void wire (DeviceRun &r, bool chatty)
{
    DeviceWorker *w = r.worker.get ();
    DeviceRun *rp = &r;
    QObject::connect (w, &DeviceWorker::connected, w, [rp, chatty] (const QString &info) {
        rp->connected = true;
        rp->info = info;
        rp->connectSec = since (rp->startedAt);
        if (chatty)
            std::printf ("  [%s] connected after %.1f s (%s)\n", rp->name.c_str (), rp->connectSec,
                qPrintable (info));
        std::fflush (stdout);
    });
    QObject::connect (w, &DeviceWorker::failed, w, [rp, chatty] (const QString &err) {
        rp->failed = true;
        rp->error = err;
        if (chatty)
            std::printf ("  [%s] FAILED after %.1f s: %s\n", rp->name.c_str (), since (rp->startedAt),
                qPrintable (QString (err).replace ('\n', QStringLiteral ("\n      "))));
        std::fflush (stdout);
    });
    QObject::connect (w, &DeviceWorker::disconnected, w, [rp, chatty] (const QString &info) {
        rp->disconnected = true;
        rp->discInfo = info;
        if (chatty)
            std::printf ("  [%s] disconnected (%s)\n", rp->name.c_str (), qPrintable (info));
        std::fflush (stdout);
    });
    QObject::connect (w, &DeviceWorker::finished, w, [rp] () { rp->finished = true; });
    QObject::connect (w, &DeviceWorker::progress, w, [rp, chatty] (const QString &m) {
        ++rp->progressCount;
        if (chatty)
            std::printf ("  [%s] %s\n", rp->name.c_str (), qPrintable (m));
        std::fflush (stdout);
    });
    QObject::connect (w, &DeviceWorker::message, w, [rp] (const QString &m) {
        std::printf ("  [%s] %s\n", rp->name.c_str (), qPrintable (m));
        std::fflush (stdout);
    });
    QObject::connect (w, &DeviceWorker::dataStall, w, [rp] (bool stalled) {
        std::printf ("  [%s] %s\n", rp->name.c_str (), stalled ? "no data for > 3 s" : "data resumed");
        std::fflush (stdout);
    });
    QObject::connect (w, &DeviceWorker::recordingStarted, w, [rp] (const QStringList &files) {
        rp->recFiles = files;
        for (const QString &f : files)
            std::printf ("  [%s] recording -> %s\n", rp->name.c_str (), qPrintable (f));
        std::fflush (stdout);
    });
    QObject::connect (w, &DeviceWorker::recordingStopped, w, [rp] (const QStringList &files) {
        rp->recStopped = true;
        std::printf ("  [%s] recording stopped (%d file%s closed)\n", rp->name.c_str (),
            static_cast<int> (files.size ()), files.size () == 1 ? "" : "s");
        std::fflush (stdout);
    });
}

DeviceConfig makeConfig (DeviceKind kind, bool synthetic, const ProbeOptions *po,
    std::vector<std::string> *problems)
{
    DeviceConfig cfg;
    cfg.slot = deviceSlotName (kind);
    cfg.displayName = deviceDisplayName (kind);
    cfg.boardId = synthetic ? static_cast<int> (BoardIds::SYNTHETIC_BOARD) : realBoardIdFor (kind);
    if (synthetic)
        cfg.params.other_info = "bioacq:" + cfg.slot;
    else if (kind == DeviceKind::Cyton)
        cfg.params.serial_port = po->cytonPort.toStdString ();
    else
    {
        cfg.params.ip_address = po->emotibitIp.trimmed ().toStdString ();
        cfg.params.timeout = po->emotibitTimeoutSec;
        cfg.ownDiscovery = !po->bfDiscovery;
    }
    cfg.signalSpecs = resolveSignals (cfg.boardId, signalDefsFor (kind), problems);
    cfg.pollIntervalMs = kind == DeviceKind::Cyton ? 10 : 15;
    cfg.countPackageGaps = kind == DeviceKind::Cyton;
    cfg.filePrefix = cfg.slot + (synthetic ? "_synthetic" : "");
    cfg.stamp = QDateTime::currentDateTime ().toString (QStringLiteral ("yyyyMMdd_HHmmss")).toStdString ();
    if (po)
    {
        cfg.record = po->record;
        cfg.recordDir = po->recordDir.toStdString ();
    }
    return cfg;
}

std::string rowsStr (const std::vector<int> &rows)
{
    std::string s = "[";
    for (std::size_t i = 0; i < rows.size (); ++i)
        s += (i ? "," : "") + std::to_string (rows[i]);
    return s + "]";
}

void beginMeasure (DeviceRun &r)
{
    const auto &chans = r.worker->channels ();
    r.meters.assign (chans.size (), RateMeter (2.0));
    r.countAtStart.clear ();
    const double now = wallClockSeconds ();
    for (std::size_t i = 0; i < chans.size (); ++i)
    {
        r.countAtStart.push_back (chans[i].ring->totalWritten ());
        r.meters[i].sample (now, chans[i].ring->totalWritten ());
    }
}

void sampleMeters (DeviceRun &r)
{
    const auto &chans = r.worker->channels ();
    const double now = wallClockSeconds ();
    for (std::size_t i = 0; i < chans.size () && i < r.meters.size (); ++i)
        r.meters[i].sample (now, chans[i].ring->totalWritten ());
}

struct SignalReport
{
    std::string key;
    std::uint64_t samples = 0;
    double rate2s = 0.0, rateAvg = 0.0, nominal = 0.0;
    bool finite = true;
    std::vector<double> latest;
    double ageSec = 0.0;
};

std::vector<SignalReport> report (DeviceRun &r, double measureSec, bool printTable)
{
    std::vector<SignalReport> out;
    const auto &chans = r.worker->channels ();
    const double now = wallClockSeconds ();
    if (printTable)
        std::printf ("  %-20s %-10s %-9s %4s %8s %8s %9s %9s  %s\n", "signal", "preset", "rows",
            "ts", "nominal", "samples", "rate(2s)", "rate(avg)", "latest value(s)");
    for (std::size_t i = 0; i < chans.size (); ++i)
    {
        const SignalChannel &ch = chans[i];
        SignalReport rep;
        rep.key = ch.spec.key;
        rep.samples = ch.ring->totalWritten ();
        rep.nominal = ch.spec.nominalRate;
        rep.rate2s = i < r.meters.size () ? r.meters[i].rate () : 0.0;
        const std::uint64_t base = i < r.countAtStart.size () ? r.countAtStart[i] : 0;
        rep.rateAvg = measureSec > 0.0 ? static_cast<double> (rep.samples - base) / measureSec : 0.0;
        double ts = 0.0;
        if (ch.ring->latest (ts, rep.latest))
            rep.ageSec = now - ts;
        std::vector<double> t;
        std::vector<std::vector<double>> v;
        const std::size_t n = ch.ring->copySince (now - 2.0, t, v);
        for (std::size_t c = 0; c < v.size (); ++c)
            for (std::size_t k = 0; k < n; ++k)
                if (!std::isfinite (v[c][k]))
                    rep.finite = false;
        if (printTable)
        {
            std::string vals;
            const std::size_t shown = ch.spec.rows.size ();
            for (std::size_t c = 0; c < rep.latest.size () && c < shown; ++c)
            {
                char buf[48];
                std::snprintf (buf, sizeof (buf), "%s%.*f", c ? " " : "", ch.spec.valueDecimals, rep.latest[c]);
                vals += buf;
            }
            if (ch.rawChannel >= 0 && ch.rawChannel < static_cast<int> (rep.latest.size ()))
            {
                char buf[48];
                std::snprintf (buf, sizeof (buf), " (raw %.1f)", rep.latest[static_cast<std::size_t> (ch.rawChannel)]);
                vals += buf;
            }
            std::printf ("  %-20s %-10s %-9s %4d %8.1f %8llu %9.1f %9.1f  %s %s (age %.2f s)\n",
                ch.spec.key.c_str (), presetName (ch.spec.preset).c_str (), rowsStr (ch.spec.rows).c_str (),
                ch.spec.timestampRow, rep.nominal, static_cast<unsigned long long> (rep.samples),
                rep.rate2s, rep.rateAvg, vals.c_str (), ch.spec.units.c_str (), rep.ageSec);
            if (ch.spec.substituted)
                std::printf ("  %-20s   (source: %s)\n", "", ch.spec.source.c_str ());
        }
        out.push_back (rep);
    }
    std::fflush (stdout);
    return out;
}

// ---------------------------------------------------------------- unit checks
struct Checker
{
    int failures = 0;
    int total = 0;
    void operator() (bool ok, const std::string &what)
    {
        ++total;
        if (!ok)
            ++failures;
        std::printf ("  [%s] %s\n", ok ? " OK " : "FAIL", what.c_str ());
        std::fflush (stdout);
    }
};

std::string fmt (const char *f, double a, double b = 0.0, double c = 0.0)
{
    char buf[256];
    std::snprintf (buf, sizeof (buf), f, a, b, c);
    return buf;
}

bool allFinite (const std::vector<QPolygonF> &segs, int count)
{
    for (int s = 0; s < count; ++s)
        for (const QPointF &pt : segs[static_cast<std::size_t> (s)])
            if (!std::isfinite (pt.x ()) || !std::isfinite (pt.y ()))
                return false;
    return true;
}

// Synthetic EmotiBit PPG through HeartRate::Tracker: 25 Hz with +-2 ms timing
// jitter, delivered in 3-sample packets. Per channel: pulse rate (0 = no
// pulse), white noise and baseline wander (0.25 Hz respiration 1.5 x and
// 0.05 Hz drift 3 x the given fraction), both relative to the pulse amplitude.
struct PpgChannelSim
{
    double bpm = 0.0;
    double noise = 0.0;
    double wander = 0.0;
};

struct HrSimResult
{
    std::vector<HeartRate::Sample> samples;
    int sourceSwitches = 0;
    const HeartRate::Sample *lastValid () const
    {
        for (std::size_t i = samples.size (); i-- > 0;)
            if (std::isfinite (samples[i].bpm))
                return &samples[i];
        return nullptr;
    }
    int validCount () const
    {
        int n = 0;
        for (const HeartRate::Sample &s : samples)
            n += std::isfinite (s.bpm) ? 1 : 0;
        return n;
    }
};

HrSimResult simulateHeartRate (const PpgChannelSim ch[HeartRate::kSources], double seconds, unsigned seed)
{
    const double fs = 25.0, t0 = 1.7e9, amp = 800.0;
    const double dc[HeartRate::kSources] = {120000.0, 90000.0, 150000.0};
    std::mt19937 rng (seed);
    std::normal_distribution<double> noise (0.0, 1.0);
    std::uniform_real_distribution<double> jitter (-0.002, 0.002);
    HeartRate::Tracker tracker (fs);
    HrSimResult r;
    double t[3];
    double x[HeartRate::kSources][3];
    int k = 0, last = HeartRate::SourceNone;
    const int n = static_cast<int> (seconds * fs);
    for (int i = 0; i < n; ++i)
    {
        const double ti = i / fs;
        t[k] = t0 + ti + jitter (rng);
        for (int c = 0; c < HeartRate::kSources; ++c)
        {
            double v = ch[c].bpm > 0.0 ? HeartRate::syntheticPpg (ti, ch[c].bpm, dc[c], amp) : dc[c];
            v += ch[c].wander * amp *
                (1.5 * std::sin (2.0 * M_PI * 0.25 * ti + c) + 3.0 * std::sin (2.0 * M_PI * 0.05 * ti + 1.0 + c));
            v += ch[c].noise * amp * noise (rng);
            x[c][k] = v;
        }
        if (++k < 3)
            continue;
        k = 0;
        for (int c = 0; c < HeartRate::kSources; ++c)
            tracker.process (c, t, x[c], 3);
        tracker.update (t[2], r.samples);
        if (tracker.source () != HeartRate::SourceNone && tracker.source () != last)
        {
            if (last != HeartRate::SourceNone)
                ++r.sourceSwitches;
            last = tracker.source ();
        }
    }
    return r;
}

void heartRateChecks (Checker &check)
{
    // A clean-ish green channel at each rate; red noisier, IR noise only.
    for (double bpm : {60.0, 72.0, 120.0, 180.0})
    {
        const PpgChannelSim ch[3] = {{bpm, 0.08, 0.3}, {bpm, 0.6, 0.3}, {0.0, 1.0, 0.3}};
        const HrSimResult r = simulateHeartRate (ch, 20.0, static_cast<unsigned> (bpm));
        const HeartRate::Sample *s = r.lastValid ();
        const bool ok = s && std::isfinite (r.samples.back ().bpm) && std::fabs (s->bpm - bpm) <= 2.0 &&
            s->source == HeartRate::SourceGreen;
        check (ok, fmt ("heart rate: %.0f bpm synthetic PPG (noise + baseline wander, 25 Hz) -> %.2f bpm from GREEN, "
                        "quality %.0f %%",
                       bpm, s ? s->bpm : std::numeric_limits<double>::quiet_NaN (), s ? 100.0 * s->quality : 0.0));
    }
    // Reflectance counts drop at systole: beats must sit at the count minima
    // (plus the band-pass delay), not at the diastolic maxima half a beat away.
    {
        HeartRate::BeatDetector d (25.0);
        std::vector<double> t, x;
        for (int i = 0; i < 500; ++i)
        {
            t.push_back (i / 25.0);
            x.push_back (HeartRate::syntheticPpg (i / 25.0, 72.0, 120000.0, 800.0));
        }
        d.process (t.data (), x.data (), t.size ());
        double sum = 0.0, sq = 0.0;
        for (double b : d.beats ())
        {
            const double lag = b - HeartRate::systolicTime (b + 0.5 * 60.0 / 72.0, 72.0);
            sum += lag;
            sq += lag * lag;
        }
        const double nb = static_cast<double> (d.beats ().size ());
        const double mean = nb > 0.0 ? sum / nb : std::numeric_limits<double>::quiet_NaN ();
        const double sd = nb > 0.0 ? std::sqrt (std::max (0.0, sq / nb - mean * mean)) : 0.0;
        check (nb >= 20 && mean >= 0.0 && mean <= 0.12 && sd < 0.01,
            fmt ("heart rate: beats found on the INVERTED counts, %.0f beats %.3f s after the systolic count minima "
                 "(sd %.4f s)",
                nb, mean, sd));
    }
    {
        const PpgChannelSim noise[3] = {{0.0, 1.0, 0.3}, {0.0, 1.0, 0.0}, {0.0, 1.0, 1.0}};
        const PpgChannelSim flat[3] = {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}};
        int noiseValid = 0;
        for (unsigned seed = 1; seed <= 4; ++seed)
            noiseValid += simulateHeartRate (noise, 30.0, seed).validCount ();
        const HrSimResult f = simulateHeartRate (flat, 20.0, 1);
        check (noiseValid == 0 && f.validCount () == 0 && !f.samples.empty (),
            fmt ("heart rate: noise-only (4 x 30 s) -> %.0f HR values, flat -> %.0f HR values (status only)",
                double (noiseValid), double (f.validCount ())));
    }
    {
        const PpgChannelSim ch[3] = {{0.0, 1.0, 0.3}, {72.0, 0.08, 0.3}, {72.0, 0.5, 0.3}};
        const HrSimResult r = simulateHeartRate (ch, 20.0, 7);
        const HeartRate::Sample *s = r.lastValid ();
        check (s && s->source == HeartRate::SourceRed && std::fabs (s->bpm - 72.0) <= 2.0 && r.sourceSwitches == 0,
            fmt ("heart rate: source selection picks the clean channel (RED %.2f bpm; green noise, IR noisy), "
                 "%.0f source switches",
                s ? s->bpm : std::numeric_limits<double>::quiet_NaN (), double (r.sourceSwitches)));
    }
}

void unitChecks (Checker &check)
{
    // Ring buffer wrap-around
    {
        SignalRing ring (2, 100);
        std::vector<double> ts, a, b;
        for (int i = 0; i < 250; ++i)
        {
            ts.push_back (i);
            a.push_back (i);
            b.push_back (-i);
        }
        for (int off = 0; off < 250; off += 7)
        {
            const int n = std::min (7, 250 - off);
            const double *vals[2] = {a.data () + off, b.data () + off};
            ring.append (ts.data () + off, vals, static_cast<std::size_t> (n));
        }
        std::vector<double> t;
        std::vector<std::vector<double>> v;
        const std::size_t all = ring.copySince (-1e9, t, v);
        const std::size_t tail = ring.copySince (240.0, t, v);
        const bool ok = all == 100 && tail == 10 && t.front () == 240.0 && t.back () == 249.0 &&
            v[1].back () == -249.0 && ring.totalWritten () == 250;
        check (ok, "ring buffer: wrap-around keeps newest 100 of 250, window copy in order");
    }
    // Decimation
    {
        const int n = 100000;
        std::vector<double> t (n), v (n);
        std::mt19937 rng (1);
        std::normal_distribution<double> noise (0.0, 0.2);
        for (int i = 0; i < n; ++i)
        {
            t[i] = i * 1e-4; // 10 kHz for 10 s
            v[i] = std::sin (2.0 * M_PI * 3.0 * t[i]) + noise (rng);
        }
        PixelMap m;
        m.tStart = 0.0;
        m.tSpan = 10.0;
        m.left = 50.0;
        m.width = 800.0;
        m.vLo = -2.0;
        m.vSpan = 4.0;
        m.top = 0.0;
        m.height = 200.0;
        std::vector<QPolygonF> segs;
        std::size_t pts = 0;
        const int nseg = buildPolylines (t.data (), v.data (), n, 0.0, m, 1.0, segs, &pts);
        check (nseg == 1 && pts <= 2 * 801 + 2,
            fmt ("decimation: 100000 samples on 800 px -> %.0f points (limit 1604)", double (pts)));

        std::vector<double> ts (200), vs (200);
        for (int i = 0; i < 200; ++i)
        {
            ts[i] = i * 0.05 + (i >= 100 ? 3.0 : 0.0); // 3 s dropout in the middle
            vs[i] = std::cos (i * 0.1);
        }
        m.tSpan = 13.0;
        const int nseg2 = buildPolylines (ts.data (), vs.data (), 200, 0.0, m, 1.0, segs, &pts);
        check (nseg2 == 2 && pts == 200,
            fmt ("decimation: sparse data kept exactly (%.0f points), 3 s dropout -> %.0f segments",
                double (pts), double (nseg2)));

        // Non-finite values are gaps, never NaN vertices (exact and decimated paths)
        std::vector<double> tn (100), vn (100);
        for (int i = 0; i < 100; ++i)
        {
            tn[i] = i * 0.05;
            vn[i] = std::sin (i * 0.2);
        }
        vn[50] = std::numeric_limits<double>::quiet_NaN ();
        vn[51] = std::numeric_limits<double>::infinity ();
        m.tSpan = 5.0;
        const int nsegN = buildPolylines (tn.data (), vn.data (), 100, 0.0, m, 1.0, segs, &pts);
        const bool exactOk = nsegN == 2 && pts == 98 && allFinite (segs, nsegN);
        std::vector<double> vd (v);
        for (int i = 500; i < n; i += 9973)
            vd[static_cast<std::size_t> (i)] = std::numeric_limits<double>::quiet_NaN ();
        vd[0] = std::numeric_limits<double>::quiet_NaN (); // first sample of the first bin
        m.tSpan = 10.0;
        std::size_t ptsD = 0;
        const int nsegD = buildPolylines (t.data (), vd.data (), n, 0.0, m, 1.0, segs, &ptsD);
        check (exactOk && nsegD > 1 && allFinite (segs, nsegD) &&
                ptsD <= static_cast<std::size_t> (2 * 801 + 2 * nsegD + 2),
            fmt ("decimation: NaN/Inf -> gaps (sparse: %.0f segments, dense: %.0f segments), all vertices finite",
                double (nsegN), double (nsegD)));

        // exact <-> decimated hysteresis at ~2.2 samples per pixel
        std::vector<double> th (1100), vh (1100);
        for (int i = 0; i < 1100; ++i)
        {
            th[i] = i * (10.0 / 1100.0);
            vh[i] = std::sin (i * 0.37);
        }
        m.width = 500.0;
        bool dm = false;
        std::size_t p1 = 0, p2 = 0;
        buildPolylines (th.data (), vh.data (), 1100, 0.0, m, 1.0, segs, &p1, nullptr, &dm);
        const bool stayExact = !dm && p1 == 1100;
        dm = true;
        buildPolylines (th.data (), vh.data (), 1100, 0.0, m, 1.0, segs, &p2, nullptr, &dm);
        const bool stayDense = dm && p2 < 1100;
        check (stayExact && stayDense,
            fmt ("decimation hysteresis at 2.2 samples/px: exact stays exact (%.0f pts), decimated stays decimated (%.0f pts)",
                double (p1), double (p2)));
    }
    // EmotiBit packet re-timing (display timestamps)
    {
        // 3 samples per packet stamped within 2 us, one packet every 0.12 s (25 Hz)
        std::vector<double> in;
        for (int k = 0; k < 40; ++k)
            for (int j = 0; j < 3; ++j)
                in.push_back (1.7e9 + 0.12 * k + j * 1e-6);
        PacketRetimer rt (25.0);
        std::vector<double> out (in.size ());
        rt.process (in.data (), out.data (), in.size ());
        double worst = 0.0;
        for (std::size_t i = 1; i < out.size (); ++i)
            worst = std::max (worst, std::fabs ((out[i] - out[i - 1]) - 0.04));
        // same stream delivered in polls of 5 samples (packets split across polls)
        PacketRetimer rt2 (25.0);
        std::vector<double> out2 (in.size ());
        for (std::size_t off = 0; off < in.size (); off += 5)
            rt2.process (in.data () + off, out2.data () + off, std::min<std::size_t> (5, in.size () - off));
        bool mono = true;
        double dmax = 0.0;
        for (std::size_t i = 1; i < out2.size (); ++i)
        {
            const double d = out2[i] - out2[i - 1];
            mono = mono && d > 0.0;
            dmax = std::max (dmax, d);
        }
        check (worst < 1e-4 && mono && dmax <= 0.12 + 1e-6 && std::fabs (out2.back () - in.back ()) < 0.05,
            fmt ("retimer: 3-sample packets spread to 40 ms steps (max error %.1g s); split polls stay "
                 "monotonic (largest step %.3f s)",
                worst, dmax));
    }
    // Biquads at the Cyton rate
    {
        const double fs = 250.0;
        auto rmsOfTail = [] (const std::vector<double> &y, std::size_t from) {
            double s = 0.0;
            for (std::size_t i = from; i < y.size (); ++i)
                s += y[i] * y[i];
            return std::sqrt (s / static_cast<double> (y.size () - from));
        };
        auto meanOfTail = [] (const std::vector<double> &y, std::size_t from) {
            double s = 0.0;
            for (std::size_t i = from; i < y.size (); ++i)
                s += y[i];
            return s / static_cast<double> (y.size () - from);
        };
        const std::size_t N = static_cast<std::size_t> (fs * 6), tail = static_cast<std::size_t> (fs * 3);

        Biquad hp = Biquad::highPass (fs, 0.5);
        std::vector<double> y (N);
        for (std::size_t i = 0; i < N; ++i)
            y[i] = hp.process (-35000.0 + 50.0 * std::sin (2.0 * M_PI * 5.0 * i / fs));
        const double hpMean = meanOfTail (y, tail), hpRms = rmsOfTail (y, tail);
        FilterChain hpChain;
        hpChain.stages.push_back (Biquad::highPass (fs, 0.5));
        const double g05 = hpChain.gainAt (fs, 0.5), g01 = hpChain.gainAt (fs, 0.1);
        check (std::fabs (hpMean) < 0.5 && std::fabs (hpRms - 50.0 / std::sqrt (2.0)) < 2.0 &&
                std::fabs (g05 - std::sqrt (0.5)) < 0.01 && g01 < 0.05,
            fmt ("high-pass 0.5 Hz: -35000 uV DC removed (mean %.3f), 5 Hz kept (rms %.2f of 35.36), 0.1 Hz gain %.3f",
                hpMean, hpRms, g01));

        Biquad lp = Biquad::lowPass (fs, 40.0);
        for (std::size_t i = 0; i < N; ++i)
            y[i] = lp.process (100.0 * std::sin (2.0 * M_PI * 10.0 * i / fs));
        const double lpPass = rmsOfTail (y, tail) / (100.0 / std::sqrt (2.0));
        Biquad lp2 = Biquad::lowPass (fs, 40.0);
        for (std::size_t i = 0; i < N; ++i)
            y[i] = lp2.process (100.0 * std::sin (2.0 * M_PI * 100.0 * i / fs));
        const double lpStop = 20.0 * std::log10 (rmsOfTail (y, tail) / (100.0 / std::sqrt (2.0)));
        FilterChain lpChain;
        lpChain.stages.push_back (Biquad::lowPass (fs, 40.0));
        check (std::fabs (lpPass - 1.0) < 0.03 && lpStop < -12.0 && std::fabs (lpChain.gainAt (fs, 40.0) - std::sqrt (0.5)) < 0.01,
            fmt ("low-pass 40 Hz: 10 Hz gain %.3f, 100 Hz attenuated %.1f dB, -3 dB at 40 Hz", lpPass, lpStop));

        Biquad nt = Biquad::notch (fs, 60.0, 30.0);
        for (std::size_t i = 0; i < N; ++i)
            y[i] = nt.process (100.0 * std::sin (2.0 * M_PI * 60.0 * i / fs));
        const double att = 20.0 * std::log10 (rmsOfTail (y, tail) / (100.0 / std::sqrt (2.0)));
        Biquad nt2 = Biquad::notch (fs, 60.0, 30.0);
        for (std::size_t i = 0; i < N; ++i)
            y[i] = nt2.process (100.0 * std::sin (2.0 * M_PI * 10.0 * i / fs));
        const double pass = rmsOfTail (y, tail) / (100.0 / std::sqrt (2.0));
        check (att < -30.0 && std::fabs (pass - 1.0) < 0.02,
            fmt ("notch 60 Hz: 60 Hz attenuated %.1f dB, 10 Hz gain %.3f", att, pass));
    }
    heartRateChecks (check);
    // Signal resolution on the real board descriptors (no hardware needed)
    {
        std::vector<std::string> problems;
        const auto cy = resolveSignals (realBoardIdFor (DeviceKind::Cyton), signalDefsFor (DeviceKind::Cyton), &problems);
        const auto em = resolveSignals (realBoardIdFor (DeviceKind::EmotiBit), signalDefsFor (DeviceKind::EmotiBit), &problems);
        bool ok = problems.empty () && cy.size () == 1 && em.size () == 5;
        std::string detail;
        for (const auto &s : cy)
            detail += s.key + "=" + presetName (s.preset) + rowsStr (s.rows) + " ";
        for (const auto &s : em)
        {
            detail += s.key + "=" + presetName (s.preset) + rowsStr (s.rows) + " ";
            ok = ok && !s.substituted;
        }
        check (ok, "real board mapping: " + detail);
    }
    // Instrument-panel readout rules (Readouts.h)
    {
        const double h1 = Readouts::railHeadroom (11200.0), h2 = Readouts::railHeadroom (-181904.0),
                     h3 = Readouts::railHeadroom (250000.0);
        check (std::fabs (h1 - 0.940267) < 1e-5 && std::fabs (h2 - 0.029845) < 1e-5 && h3 == 0.0 &&
                std::isnan (Readouts::railHeadroom (std::numeric_limits<double>::quiet_NaN ())),
            fmt ("rail headroom = 1 - peak|raw| / 187500 uV: 11.2 mV -> %.1f %%, -181.9 mV -> %.1f %%, beyond FS -> %.0f %%",
                h1 * 100.0, h2 * 100.0, h3 * 100.0));
        const bool hyst = Readouts::nearRail (0.09, false) && !Readouts::nearRail (0.11, false) &&
            Readouts::nearRail (0.11, true) && !Readouts::nearRail (0.13, true);
        check (hyst, "near-rail warning: enter below 10 % headroom, leave above 12 % (hysteresis)");
        {
            // Raw ECG near the rail for 3 s, then a normal ECG: judged on the
            // last 2 s at the GUI's 4 Hz cadence, the warning must clear within
            // ~2 s of the recovery (it used to wait for the whole plot window).
            SignalRing ring (1, 4000);
            const double fs = 250.0, recovery = 3.0;
            for (int i = 0; i < 2000; ++i)
            {
                const double t = i / fs;
                const double v = t < recovery ? 181000.0 + 500.0 * std::sin (2.0 * M_PI * 5.0 * t)
                                              : 1200.0 * std::sin (2.0 * M_PI * 1.2 * t);
                const double *vals[1] = {&v};
                ring.append (&t, vals, 1);
            }
            std::vector<double> tb;
            std::vector<std::vector<double>> vb;
            bool near = false, wasOn = false;
            double clearedAt = std::numeric_limits<double>::quiet_NaN ();
            for (double now = 0.25; now <= 8.0; now += 0.25)
            {
                // evaluate the ring as it was at `now` (samples up to now only)
                SignalRing part (1, 4000);
                const std::size_t n = ring.copySince (-1.0, tb, vb);
                for (std::size_t k = 0; k < n && tb[k] <= now; ++k)
                {
                    const double *vals[1] = {&vb[0][k]};
                    part.append (&tb[k], vals, 1);
                }
                const double peak = Readouts::ringPeakAbs (part, 0, now, Readouts::kRailWindowSec, tb, vb);
                near = Readouts::nearRail (Readouts::railHeadroom (peak), near);
                wasOn = wasOn || (near && now < recovery);
                if (!near && wasOn && !std::isfinite (clearedAt))
                    clearedAt = now;
            }
            check (wasOn && std::isfinite (clearedAt) && clearedAt - recovery <= Readouts::kRailWindowSec + 0.25 && !near,
                fmt ("near-rail on the last 2 s of raw ECG: warning cleared %.2f s after the input recovered", clearedAt - recovery));
        }
        {
            const QString sat = Readouts::nearRailSentence (Readouts::railHeadroom (187500.0), 187500.0, false);
            const QString nearTxt = Readouts::nearRailSentence (Readouts::railHeadroom (181000.0), 181000.0, false);
            check (sat == QStringLiteral ("ECG saturated at ±187,500 µV — reseat the electrode or check contact.") &&
                    !sat.contains (QStringLiteral ("within")) && Readouts::saturated (-187499.5) &&
                    !Readouts::saturated (187400.0) && nearTxt.contains (QStringLiteral ("within 3.4 %")),
                "near-rail wording: '" + sat.toStdString () + "' at full scale, 'within 3.4 %' at 181 mV");
        }
        using RT = Readouts::RateTone;
        check (Readouts::rateTone (true, 249.8, 250.0) == RT::Ok && Readouts::rateTone (true, 237.6, 250.0) == RT::Ok &&
                Readouts::rateTone (true, 237.4, 250.0) == RT::Low && Readouts::rateTone (true, 18.1, 25.0) == RT::Low &&
                Readouts::rateTone (false, 0.0, 15.0) == RT::None &&
                Readouts::rateText (false, 0.0, 15.0) == QStringLiteral ("\u2014 / 15 Hz") &&
                Readouts::rateText (true, 249.84, 250.0) == QStringLiteral ("249.8 / 250 Hz"),
            "rate colour rule: amber only when > 5 % below nominal ('249.8 / 250 Hz'); '\xe2\x80\x94 / 15 Hz' before the first sample");
        check (Readouts::isStalled (true, 1.2) && !Readouts::isStalled (true, 0.8) && !Readouts::isStalled (false, 5.0) &&
                !Readouts::isStalled (true, std::numeric_limits<double>::quiet_NaN ()),
            "stall detection: streaming and no new sample for > 1 s");
        {
            const double nan = std::numeric_limits<double>::quiet_NaN ();
            using Readouts::isStalled, Readouts::silentSeconds, Readouts::stallThreshold;
            const double g = Readouts::kFirstSampleGraceEmotibit;
            const bool never = isStalled (true, silentSeconds (false, nan, 3.5), stallThreshold (false, g)) &&
                !isStalled (true, silentSeconds (false, nan, 2.0), stallThreshold (false, g)) &&
                !isStalled (true, silentSeconds (false, nan, nan), stallThreshold (false, g)) &&
                isStalled (true, silentSeconds (true, 1.2, 0.1), stallThreshold (true, g)) &&
                !isStalled (false, silentSeconds (false, nan, 9.0), stallThreshold (false, g));
            check (never, "stall detection: a stream with no sample 3 s after it started streaming counts as stalled");
        }
        check (Readouts::headroomPercent (0.0996) == QStringLiteral ("9.9 %") &&
                Readouts::headroomPercent (0.940267) == QStringLiteral ("94.0 %") &&
                Readouts::headroomPercent (0.10) == QStringLiteral ("10.0 %") &&
                Readouts::headroomPercent (std::numeric_limits<double>::quiet_NaN ()) == QStringLiteral ("—"),
            "headroom readout floored to 0.1 %: 0.0996 reads '9.9 %', never '10 %' below the threshold");
        int last = -1;
        const double seq[] = {253, 254, 255, 0, 1, 3, 4, 10};
        const std::uint64_t g1 = Readouts::packageGaps (last, seq, 8);
        const double seq2[] = {12, 13};
        const std::uint64_t g2 = Readouts::packageGaps (last, seq2, 2);
        check (g1 == 6 && g2 == 1,
            fmt ("dropped packets from package_num gaps across the 255 -> 0 wrap: %.0f + %.0f (expected 6 + 1)",
                double (g1), double (g2)));
        const QString a = Readouts::number (-18.44, 1, true), b = Readouts::number (181904.0, 0, true),
                      c = Readouts::number (41872.0, 0), d = Readouts::number (-0.0004, 3, true);
        check (a == QStringLiteral ("\u221218.4") && b == QStringLiteral ("+181,904") && c == QStringLiteral ("41,872") &&
                d == QStringLiteral ("0.000") && Readouts::elapsed (257.9) == QStringLiteral ("00:04:17") &&
                Readouts::bytes (84.2e6) == QStringLiteral ("84.2 MB"),
            "readout formatting: U+2212 minus, explicit +, thousands separators, hh:mm:ss, SI bytes");
        Readouts::CpuMeter cm;
        cm.sample ();
        const auto tb = SteadyClock::now ();
        volatile double sink = 0.0;
        while (since (tb) < 0.25)
            sink = sink + std::sqrt (sink + 1.0);
        const double pct = cm.sample ();
        check (pct > 50.0 && pct < 400.0, fmt ("CPU meter (getrusage, all threads): a busy loop reads %.0f %% of one core", pct));
        {
            const double nan = std::numeric_limits<double>::quiet_NaN ();
            using Readouts::heartRateView;
            const auto ok = heartRateView (true, true, 0.6, 71.6, 0.95, HeartRate::SourceGreen, 9, 0.9, 20.0);
            const auto stale = heartRateView (true, true, 5.0, 71.6, 0.95, HeartRate::SourceGreen, 9, 0.9, 20.0);
            const auto noPulse = heartRateView (true, true, 0.4, nan, 1.0, HeartRate::SourceNone, 2, 0.1, 20.0);
            const auto irregular = heartRateView (true, true, 0.4, nan, 0.45, HeartRate::SourceNone, 9, 0.8, 20.0);
            const auto acquiring = heartRateView (true, true, 0.4, nan, 0.0, HeartRate::SourceNone, 1, 0.0, 3.0);
            const auto stalled = heartRateView (true, true, 4.5, nan, 0.0, HeartRate::SourceNone, 1, 0.0, 3.0);
            const auto off = heartRateView (false, true, 0.6, 71.6, 0.95, HeartRate::SourceGreen, 9, 0.9, 20.0);
            check (ok.value == QStringLiteral ("72") && ok.source == QStringLiteral ("GREEN") &&
                    ok.chip == QStringLiteral ("QUALITY 95 %") && ok.valid && stale.value == QStringLiteral ("—") &&
                    stale.chip == QStringLiteral ("NO DATA") && stale.warn && noPulse.value == QStringLiteral ("—") &&
                    noPulse.chip == QStringLiteral ("NO PULSE") && irregular.chip == QStringLiteral ("IRREGULAR · Q 45 %") &&
                    acquiring.chip == QStringLiteral ("ACQUIRING") && !acquiring.warn &&
                    stalled.chip == QStringLiteral ("NO DATA") && stalled.warn && off.value == QStringLiteral ("—") &&
                    !off.valid,
                "heart-rate readout: '72' + GREEN + 'QUALITY 95 %'; stale -> '—' NO DATA (also while acquiring); "
                "no pulse / irregular / acquiring -> '—' with the reason; not streaming -> '—'");
        }
    }
}

// ------------------------------------------------------- EmotiBit discovery
// Minimal stand-in for an EmotiBit's advertising socket on 127.0.0.1: answers
// every HELLO_EMOTIBIT with a HELLO_HOST sent back to the sender (or stays
// silent), like the firmware does on UDP port 3131.
class FakeEmotibit
{
public:
    explicit FakeEmotibit (bool answer) : answer_ (answer)
    {
        fd_ = ::socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd_ < 0)
            return;
        struct sockaddr_in a;
        std::memset (&a, 0, sizeof (a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
        a.sin_port = 0;
        socklen_t al = sizeof (a);
        if (::bind (fd_, reinterpret_cast<struct sockaddr *> (&a), sizeof (a)) != 0 ||
            ::getsockname (fd_, reinterpret_cast<struct sockaddr *> (&a), &al) != 0)
        {
            ::close (fd_);
            fd_ = -1;
            return;
        }
        port_ = ntohs (a.sin_port);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50000;
        ::setsockopt (fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv));
        thread_ = std::thread ([this] { serve (); });
    }
    ~FakeEmotibit ()
    {
        quit_.store (true);
        if (thread_.joinable ())
            thread_.join ();
        if (fd_ >= 0)
            ::close (fd_);
    }
    FakeEmotibit (const FakeEmotibit &) = delete;
    FakeEmotibit &operator= (const FakeEmotibit &) = delete;

    bool ok () const
    {
        return fd_ >= 0 && port_ > 0;
    }
    int port () const
    {
        return port_;
    }
    int hellos () const
    {
        return hellos_.load ();
    }

private:
    void serve ()
    {
        char buf[1024];
        while (!quit_.load ())
        {
            struct sockaddr_in from;
            socklen_t fl = sizeof (from);
            const ssize_t n = ::recvfrom (
                fd_, buf, sizeof (buf), 0, reinterpret_cast<struct sockaddr *> (&from), &fl);
            if (n <= 0)
                continue;
            if (std::string (buf, static_cast<std::size_t> (n)).find (",HE,") == std::string::npos)
                continue;
            hellos_.fetch_add (1);
            if (!answer_)
                continue;
            // header (6 fields) + payload; BrainFlow reads the serial from field 9
            static const std::string hh = "123456,7,4,HH,1,100,DI,127.0.0.1,DP,MD-V5-0000123\n";
            ::sendto (fd_, hh.data (), hh.size (), 0, reinterpret_cast<struct sockaddr *> (&from), fl);
        }
    }

    bool answer_;
    int fd_ = -1;
    int port_ = 0;
    std::atomic<bool> quit_ {false};
    std::atomic<int> hellos_ {0};
    std::thread thread_;
};

DeviceConfig emotibitTestConfig (int port, double timeoutSec)
{
    DeviceConfig cfg;
    cfg.slot = "emotibit";
    cfg.displayName = "EmotiBit";
    cfg.boardId = realBoardIdFor (DeviceKind::EmotiBit);
    cfg.params.ip_address = "127.0.0.1";
    cfg.params.timeout = 5;
    cfg.discoveryPort = port;
    cfg.discoveryTimeoutSec = timeoutSec;
    cfg.signalSpecs = resolveSignals (cfg.boardId, signalDefsFor (DeviceKind::EmotiBit), nullptr);
    cfg.pollIntervalMs = 15;
    return cfg;
}

void discoveryChecks (Checker &check)
{
    FakeEmotibit dev (true), mute (false);
    if (!dev.ok () || !mute.ok ())
    {
        check (false, "discovery: cannot open local UDP test sockets");
        return;
    }
    const auto never = [] { return false; };
    {
        const auto r = emotibit::discover ({"127.0.0.1"}, dev.port (), 3.0, "", never);
        check (r.found && r.ip == "127.0.0.1" && r.serial == "MD-V5-0000123" && r.seconds < 1.5,
            "discovery: HELLO_EMOTIBIT -> HELLO_HOST with a local fake EmotiBit: found '" + r.ip +
                "', serial '" + r.serial + "'" + fmt (" in %.2f s", r.seconds));
    }
    {
        const auto r = emotibit::discover ({"127.0.0.1"}, dev.port (), 0.6, "MD-OTHER", never);
        check (!r.found && !r.cancelled && r.anySendOk && dev.hellos () > 1,
            "discovery: HELLO_HOST from another serial number is ignored (" + r.error + ")");
    }
    {
        const auto t0 = SteadyClock::now ();
        const auto r = emotibit::discover ({"127.0.0.1"}, mute.port (), 30.0, "",
            [t0] { return since (t0) > 0.2; });
        check (r.cancelled && !r.found && r.seconds < 0.6,
            fmt ("discovery: cancel honoured after %.2f s (timeout was 30 s)", r.seconds));
    }
    {
        std::atomic<int> probes {0};
        const auto r = emotibit::discover ({"127.0.0.1"}, mute.port (), 1.2, "", never, &probes);
        check (!r.found && probes.load () == 2,
            fmt ("discovery: probe counter reports %.0f HELLO rounds in 1.2 s (one per second)", double (probes.load ())));
    }
    {
        emotibit::DiscoveryResult r;
        r.targets = {"172.31.255.255"};
        r.anySendOk = true;
        const QString t = DeviceWorker::discoveryFailureText (r, "", 5.0);
        check (t.contains (QStringLiteral ("same subnet")) && t.contains (QStringLiteral ("IP address")),
            "discovery: blank-IP failure explains the same-subnet limit and asks for the IP");
    }

    // The worker runs the discovery BEFORE any BrainFlow call: a stop during
    // it is honoured at once and an unanswered IP fails without BrainFlow.
    DeviceRun ed;
    ed.name = "emotibit(fake)";
    ed.kind = DeviceKind::EmotiBit;
    wire (ed, false);
    {
        ed.resetFlags ();
        ed.startedAt = SteadyClock::now ();
        const bool started = ed.worker->start (emotibitTestConfig (mute.port (), 30.0));
        pumpFor (0.3);
        const bool searching = ed.worker->state () == DeviceWorker::Connecting &&
            !ed.worker->inBrainFlowSetup () && ed.progressCount > 0;
        const auto tStop = SteadyClock::now ();
        ed.worker->requestStop ();
        const bool fin = pumpUntil ([&] { return ed.finished; }, 5.0) && ed.worker->waitForFinished (2000);
        const double ms = since (tStop) * 1000.0;
        check (started && searching && fin && ed.disconnected && ed.discInfo == QStringLiteral ("cancelled") &&
                !ed.failed && !ed.connected && ms < 500.0,
            fmt ("worker: EmotiBit search runs outside BrainFlow's lock; Cancel honoured in %.0f ms", ms));
    }
    {
        ed.resetFlags ();
        ed.startedAt = SteadyClock::now ();
        const bool started = ed.worker->start (emotibitTestConfig (mute.port (), 0.5));
        const bool fin = pumpUntil ([&] { return ed.finished; }, 5.0) && ed.worker->waitForFinished (2000);
        const double sec = since (ed.startedAt);
        check (started && fin && ed.failed && ed.error.contains (QStringLiteral ("no EmotiBit answered at 127.0.0.1")) &&
                sec < 2.0,
            fmt ("worker: unanswered EmotiBit IP fails after %.2f s without touching BrainFlow", sec));
    }
}

} // namespace

// =============================================================================
int runSelftest (double seconds)
{
    seconds = std::max (2.0, seconds);
    Checker check;
    std::printf ("bioacq selftest -- both device slots on BrainFlow SYNTHETIC_BOARD, %.1f s\n", seconds);
    std::printf ("BrainFlow %s\n\n", BoardShim::get_version ().c_str ());

    std::printf ("[1] unit checks\n");
    unitChecks (check);

    DeviceRun cy, em;
    cy.name = "cyton";
    cy.kind = DeviceKind::Cyton;
    em.name = "emotibit";
    em.kind = DeviceKind::EmotiBit;
    wire (cy, true);
    wire (em, true);

    // ---------------------------------------------------------- cancel path
    // Deterministic: the (test-only) 500 ms delay before prepare_session ignores
    // stop requests, exactly like a real prepare_session that is still blocking.
    std::printf ("\n[2] disconnect while prepare_session is still blocking\n");
    {
        std::vector<std::string> problems;
        DeviceConfig cfg = makeConfig (DeviceKind::Cyton, true, nullptr, &problems);
        cfg.testPrepareDelayMs = 500;
        cy.resetFlags ();
        cy.startedAt = SteadyClock::now ();
        const bool started = cy.worker->start (cfg);
        pumpFor (0.1);
        const bool inPrepare = cy.worker->state () == DeviceWorker::Connecting && cy.worker->inBrainFlowSetup ();
        cy.worker->requestStop ();
        const bool fin = pumpUntil ([&] { return cy.finished; }, 10.0);
        const double sec = since (cy.startedAt);
        const bool joined = cy.worker->waitForFinished (2000) && !cy.worker->isActive ();
        check (started && inPrepare && fin && joined && !cy.failed && !cy.connected && cy.disconnected &&
                cy.discInfo == QStringLiteral ("cancelled") && sec < 1.5,
            fmt ("stop requested 0.1 s into a 0.5 s blocking prepare_session: cancelled, finished after %.2f s", sec));

        // Identical settings must connect again at once; a leaked session
        // would fail with ANOTHER_BOARD_IS_CREATED_ERROR.
        cfg.testPrepareDelayMs = 0;
        cy.resetFlags ();
        cy.startedAt = SteadyClock::now ();
        const bool started2 = cy.worker->start (cfg);
        const bool con = pumpUntil ([&] { return cy.resolved (); }, 10.0) && cy.connected;
        cy.worker->requestStop ();
        const bool fin2 = pumpUntil ([&] { return cy.finished; }, 10.0) && cy.worker->waitForFinished (2000);
        check (started2 && con && fin2 && !cy.failed,
            "immediate reconnect with identical settings succeeds (release_session ran, no leaked session)");
    }

    // ---------------------------------------------------------- discovery
    std::printf ("\n[3] EmotiBit discovery (lock-free, cancellable; local fake device, no network)\n");
    discoveryChecks (check);

    // ---------------------------------------------------------- streaming
    std::printf ("\n[4] streaming through DeviceWorker + SignalRing (%.1f s)\n", seconds);
    for (DeviceRun *r : {&cy, &em})
    {
        std::vector<std::string> problems;
        DeviceConfig cfg = makeConfig (r->kind, true, nullptr, &problems);
        for (const auto &p : problems)
            std::printf ("  [%s] note: %s\n", r->name.c_str (), p.c_str ());
        r->resetFlags ();
        r->startedAt = SteadyClock::now ();
        if (!r->worker->start (cfg))
            std::printf ("  [%s] start() refused\n", r->name.c_str ());
    }
    const bool bothResolved = pumpUntil ([&] { return cy.resolved () && em.resolved (); }, 20.0);
    check (bothResolved && cy.connected && em.connected, "both synthetic sessions connected");

    if (cy.connected && em.connected)
    {
        beginMeasure (cy);
        beginMeasure (em);
        const auto t0 = SteadyClock::now ();
        bool toggled = false;
        while (since (t0) < seconds && !g_interruptRequested)
        {
            pumpFor (0.25);
            sampleMeters (cy);
            sampleMeters (em);
            if (!toggled && since (t0) >= seconds / 2.0)
            {
                cy.worker->setHighPass (true, 1.0);
                cy.worker->setNotch (true, 60.0);
                toggled = true;
                std::printf ("  [cyton] high-pass 1 Hz + notch 60 Hz enabled at t=%.1f s\n", since (t0));
            }
        }
        const double measured = since (t0);

        std::printf ("\n");
        std::vector<SignalReport> reps;
        for (DeviceRun *r : {&cy, &em})
        {
            std::printf ("  %s (%s):\n", r->name.c_str (), r->worker->config ().displayName.c_str ());
            auto rr = report (*r, measured, true);
            reps.insert (reps.end (), rr.begin (), rr.end ());
        }
        std::printf ("\n");
        for (const SignalReport &rep : reps)
        {
            const bool rateOk = rep.nominal > 0.0 && std::fabs (rep.rate2s - rep.nominal) / rep.nominal < 0.15 &&
                std::fabs (rep.rateAvg - rep.nominal) / rep.nominal < 0.20;
            char line[256];
            std::snprintf (line, sizeof (line),
                ": %llu samples, %.1f Hz (2 s) / %.1f Hz (avg), nominal %.0f Hz%s",
                static_cast<unsigned long long> (rep.samples), rep.rate2s, rep.rateAvg, rep.nominal,
                rep.finite ? "" : ", NON-FINITE values");
            check (rep.samples > 0 && rateOk && rep.finite, rep.key + line);
        }

        // Filter path: synthetic exg[0] = 10 uV offset + 5 Hz sine. After the HP
        // was switched on, the filtered channel must be ~zero-mean while the raw
        // channel keeps the offset.
        {
            const SignalChannel &ch = cy.worker->channels ().front ();
            std::vector<double> t;
            std::vector<std::vector<double>> v;
            const double win = std::min (1.0, seconds / 4.0);
            const std::size_t n = ch.ring->copySince (wallClockSeconds () - win, t, v);
            double mf = 0.0, mr = 0.0;
            for (std::size_t k = 0; k < n; ++k)
            {
                mf += v[0][k];
                mr += v[static_cast<std::size_t> (ch.rawChannel)][k];
            }
            if (n)
            {
                mf /= static_cast<double> (n);
                mr /= static_cast<double> (n);
            }
            check (n > 0 && std::fabs (mf) < 2.0 && std::fabs (mr) > 5.0,
                fmt ("worker filters: raw mean %.2f uV -> filtered mean %.2f uV over last %.2f s", mr, mf, win));
        }

        // Recording started and stopped on the RUNNING session (BrainFlow
        // add_streamer / delete_streamer from the worker thread).
        {
            QTemporaryDir tmp;
            cy.recFiles.clear ();
            cy.recStopped = false;
            cy.worker->requestRecording (true, tmp.path ().toStdString (), "selftest");
            const bool started = pumpUntil ([&] { return !cy.recFiles.isEmpty (); }, 3.0) && cy.worker->isRecording ();
            pumpFor (1.0);
            const QString f = cy.recFiles.value (0);
            const qint64 s1 = QFileInfo (f).size ();
            cy.worker->requestRecording (false, std::string (), std::string ());
            const bool stopped = pumpUntil ([&] { return cy.recStopped; }, 3.0) && !cy.worker->isRecording ();
            const qint64 s2 = QFileInfo (f).size ();
            pumpFor (0.5);
            const qint64 s3 = QFileInfo (f).size ();
            int rows = 0;
            QFile file (f);
            if (file.open (QIODevice::ReadOnly))
                rows = static_cast<int> (file.readAll ().count ('\n'));
            check (tmp.isValid () && started && stopped && s2 > 0 && s2 >= s1 && s3 == s2 && rows > 150,
                fmt ("recording started/stopped on a running session: %.0f rows, file closed on stop (%.0f bytes, "
                     "no growth after stop)",
                    double (rows), double (s3)));
        }
        check (cy.worker->countsDrops () && cy.worker->droppedPackets () == 0,
            fmt ("package-number gap counter on the synthetic Cyton: %.0f packets dropped",
                double (cy.worker->droppedPackets ())));

    }

    // ---------------------------------------------------------- shutdown
    std::printf ("\n[5] shutdown\n");
    {
        const auto t0 = SteadyClock::now ();
        cy.worker->requestStop ();
        em.worker->requestStop ();
        const bool fin = pumpUntil ([&] { return cy.finished && em.finished; }, 15.0);
        const double ms = since (t0) * 1000.0;
        const bool joined = cy.worker->waitForFinished (2000) && em.worker->waitForFinished (2000) &&
            !cy.worker->isActive () && !em.worker->isActive ();
        check (fin && joined && !cy.failed && !em.failed,
            fmt ("stop_stream + release_session on both workers, threads joined in %.0f ms", ms));
    }

    // ---------------------------------------------------------- reconnect
    std::printf ("\n[6] reconnect the same slot (session key reused)\n");
    {
        std::vector<std::string> problems;
        DeviceConfig cfg = makeConfig (DeviceKind::Cyton, true, nullptr, &problems);
        cy.resetFlags ();
        cy.startedAt = SteadyClock::now ();
        const bool started = cy.worker->start (cfg);
        const bool con = pumpUntil ([&] { return cy.resolved (); }, 10.0) && cy.connected;
        pumpFor (1.0);
        const std::uint64_t n = cy.worker->channels ().empty () ? 0 : cy.worker->channels ().front ().ring->totalWritten ();
        cy.worker->requestStop ();
        const bool fin = pumpUntil ([&] { return cy.finished; }, 10.0) && cy.worker->waitForFinished (2000);
        check (started && con && n > 0 && fin && !cy.failed,
            fmt ("second session on the same slot: %.0f samples in ~1 s, clean stop", double (n)));
    }

    // ---------------------------------------------------------- test hooks
    std::printf ("\n[7] test hooks (display only): EmotiBit stall + Cyton raw offset\n");
    {
        std::vector<std::string> problems;
        DeviceConfig c1 = makeConfig (DeviceKind::Cyton, true, nullptr, &problems);
        c1.testRawOffset = 181000.0;
        DeviceConfig c2 = makeConfig (DeviceKind::EmotiBit, true, nullptr, &problems);
        c2.testFreezeAfterSec = 0.6;
        cy.resetFlags ();
        em.resetFlags ();
        cy.startedAt = em.startedAt = SteadyClock::now ();
        const bool st = cy.worker->start (c1) && em.worker->start (c2);
        const bool con = pumpUntil ([&] { return cy.resolved () && em.resolved (); }, 10.0) && cy.connected && em.connected;
        pumpFor (1.3);
        auto total = [] (DeviceRun &r) {
            std::uint64_t n = 0;
            for (const auto &ch : r.worker->channels ())
                n += ch.ring->totalWritten ();
            return n;
        };
        const std::uint64_t e1 = total (em);
        pumpFor (0.8);
        const std::uint64_t e2 = total (em);
        const bool frozen = e1 > 0 && e2 == e1 && em.worker->state () == DeviceWorker::Streaming;
        double raw = 0.0;
        bool have = false;
        if (!cy.worker->channels ().empty ())
        {
            const SignalChannel &ch = cy.worker->channels ().front ();
            double ts = 0.0;
            std::vector<double> latest;
            have = ch.ring->latest (ts, latest) && ch.rawChannel >= 0 &&
                ch.rawChannel < static_cast<int> (latest.size ());
            if (have)
                raw = latest[static_cast<std::size_t> (ch.rawChannel)];
        }
        const double head = Readouts::railHeadroom (std::fabs (raw));
        check (st && con && frozen,
            fmt ("stall hook: EmotiBit ring frozen at %.0f samples while the session stays open", double (e2)));
        check (have && std::fabs (raw - 181000.0) < 500.0 && head < 0.05 && Readouts::nearRail (head, false),
            fmt ("rail-offset hook: raw Ch1 %.0f uV -> headroom %.1f %% (near rail)", raw, head * 100.0));
        cy.worker->requestStop ();
        em.worker->requestStop ();
        pumpUntil ([&] { return cy.finished && em.finished; }, 15.0);
        cy.worker->waitForFinished (2000);
        em.worker->waitForFinished (2000);
    }

    std::printf ("\nRESULT: %s (%d/%d checks passed)\n", check.failures ? "FAIL" : "PASS",
        check.total - check.failures, check.total);
    std::fflush (stdout);
    return check.failures ? 1 : 0;
}

// =============================================================================
int runProbe (const ProbeOptions &o)
{
    const QString emDesc = o.emotibitIp.trimmed ().isEmpty ()
        ? QStringLiteral ("broadcast discovery")
        : o.emotibitIp.trimmed ();
    std::printf ("bioacq probe -- real devices (Cyton: %s, EmotiBit: %s%s, timeout %d s), "
                 "measuring %.1f s\n",
        o.skipCyton ? "skipped" : qPrintable (o.cytonPort),
        o.skipEmotibit ? "skipped" : qPrintable (emDesc),
        (!o.skipEmotibit && o.bfDiscovery) ? " via BrainFlow's discovery" : "",
        o.emotibitTimeoutSec, o.seconds);
    std::printf ("BrainFlow %s\n\n", BoardShim::get_version ().c_str ());
    std::fflush (stdout);

    std::vector<std::unique_ptr<DeviceRun>> runs;
    for (DeviceKind kind : {DeviceKind::Cyton, DeviceKind::EmotiBit})
    {
        if ((kind == DeviceKind::Cyton && o.skipCyton) || (kind == DeviceKind::EmotiBit && o.skipEmotibit))
            continue;
        auto r = std::make_unique<DeviceRun> ();
        r->name = deviceSlotName (kind);
        r->kind = kind;
        wire (*r, true);
        DeviceConfig cfg = makeConfig (kind, false, &o, &r->problems);
        for (const auto &p : r->problems)
            std::printf ("  [%s] note: %s\n", r->name.c_str (), p.c_str ());
        std::printf ("  [%s] connecting...\n", r->name.c_str ());
        std::fflush (stdout);
        r->startedAt = SteadyClock::now ();
        r->worker->start (cfg);
        runs.push_back (std::move (r));
    }

    // Wait for every device to connect or fail (EmotiBit discovery can be slow).
    pumpUntil (
        [&] {
            if (g_interruptRequested)
                return true;
            for (auto &r : runs)
                if (!r->resolved ())
                    return false;
            return true;
        },
        120.0);

    int connectedCount = 0;
    for (auto &r : runs)
        if (r->connected)
        {
            ++connectedCount;
            beginMeasure (*r);
        }

    if (connectedCount > 0 && !g_interruptRequested)
    {
        std::printf ("\n  measuring for %.1f s...\n", o.seconds);
        std::fflush (stdout);
        const auto t0 = SteadyClock::now ();
        double nextPrint = 2.0;
        while (since (t0) < o.seconds && !g_interruptRequested)
        {
            pumpFor (0.25);
            for (auto &r : runs)
                if (r->connected)
                    sampleMeters (*r);
            if (since (t0) >= nextPrint)
            {
                std::string line;
                for (auto &r : runs)
                    if (r->connected)
                        for (std::size_t i = 0; i < r->meters.size (); ++i)
                        {
                            char buf[96];
                            std::snprintf (buf, sizeof (buf), "%s %.1f Hz  ",
                                r->worker->channels ()[i].spec.key.c_str (), r->meters[i].rate ());
                            line += buf;
                        }
                std::printf ("  t=%4.1f s  %s\n", since (t0), line.c_str ());
                std::fflush (stdout);
                nextPrint += 2.0;
            }
        }
        const double measured = since (t0);
        std::printf ("\n");
        for (auto &r : runs)
            if (r->connected)
            {
                std::printf ("  %s (%s), connected in %.1f s:\n", r->name.c_str (),
                    r->worker->config ().displayName.c_str (), r->connectSec);
                r->problems.clear ();
                auto reps = report (*r, measured, true);
                for (const auto &rep : reps)
                    if (rep.samples == 0)
                        r->problems.push_back (rep.key + " delivered no samples");
            }
    }

    // Stop everything; wait for clean release (a pending connect may still be running).
    const auto tStop = SteadyClock::now ();
    for (auto &r : runs)
        r->worker->requestStop ();
    const bool fin = pumpUntil (
        [&] {
            for (auto &r : runs)
                if (!r->finished)
                    return false;
            return true;
        },
        60.0);
    for (auto &r : runs)
        r->worker->waitForFinished (2000);
    std::printf ("\n  shutdown: %s in %.0f ms\n", fin ? "all sessions released" : "TIMEOUT waiting for workers",
        since (tStop) * 1000.0);

    int rc = connectedCount > 0 ? 0 : 1;
    std::printf ("\nSUMMARY\n");
    for (auto &r : runs)
    {
        if (r->connected)
        {
            if (r->problems.empty ())
                std::printf ("  %-9s OK - streaming on all signals\n", r->name.c_str ());
            else
            {
                rc = rc == 0 ? 2 : rc;
                for (const auto &p : r->problems)
                    std::printf ("  %-9s WARNING - %s\n", r->name.c_str (), p.c_str ());
            }
        }
        else
            std::printf ("  %-9s NOT CONNECTED - %s\n", r->name.c_str (),
                qPrintable (QString (r->error.isEmpty () ? QStringLiteral ("interrupted") : r->error)
                                .replace ('\n', QStringLiteral ("\n            "))));
    }
    std::fflush (stdout);
    return g_interruptRequested ? 130 : rc;
}

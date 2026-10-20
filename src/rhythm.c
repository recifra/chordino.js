#include "rhythm.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef RHYTHM_TUNE
#define TUNABLE
int rhythmSplit;                    // what the last decode split its beats in, for the bench
#else
#define TUNABLE static const
#endif

// Run once a recording, so sized down: -O3 unrolled and inlined all of this
// into 10 KB of wasm
#define ONCE __attribute__((minsize))

// Tempo: BPM looked for, and the prior over them, a log-normal distribution
TUNABLE double minBpm = 40;
TUNABLE double maxBpm = 240;
TUNABLE double tempoCentre = 120;
TUNABLE double tempoSpread = 1;     // octaves
TUNABLE int combSize = 4;           // multiples of the period that count
TUNABLE double windowSeconds = 8;   // of each autocorrelation
// Beats: how much a beat costs for each octave its interval is off the period
TUNABLE double tightness = 600;
// Bars
TUNABLE double changeLead = 0.5;    // of a beat before it, where its chord changes count
TUNABLE double changeWeight = 1;
TUNABLE double bassWeight = 1;
TUNABLE double fourBias = 1.3;      // 4 beats a bar over 2 or 3, at the same contrast
TUNABLE double jumpCost = 8;        // accent a bar has to gain to start out of turn
TUNABLE double sixBias = 1;         // 6 eighths a bar over 3 beats
TUNABLE double eighthBpm = 110;     // the slowest beats that can be eighths of 6/8
TUNABLE double ternaryBias = 1.2;   // thirds of a beat over halves
TUNABLE double ternaryFloor = 0.1;  // and over the beat itself

ONCE rhythm_t *rhythm_create(float sampleRate) {
    rhythm_t *rhythm = malloc(sizeof(rhythm_t));
    rhythm->onset = onset_create(sampleRate);
    rhythm->beats = vector_create(sizeof(rhythm_beat_t));
    rhythm_reset(rhythm);
    return rhythm;
}

void rhythm_destroy(rhythm_t *rhythm) {
    onset_destroy(rhythm->onset);
    vector_destroy(&rhythm->beats);
    free(rhythm);
}

void rhythm_reset(rhythm_t *rhythm) {
    onset_reset(rhythm->onset);
    rhythm->start = 0;
    rhythm->fed = false;
    vector_clear(&rhythm->beats);
    memset(&rhythm->result, 0, sizeof(rhythm_result_t));
}

void rhythm_process(rhythm_t *rhythm, const float *samples, int count, realtime_t timestamp) {
    if (!rhythm->fed) {
        rhythm->start = realtime_to_seconds(timestamp);
        rhythm->fed = true;
    }
    onset_process(rhythm->onset, samples, count);
}

// The onset strength high-passed, with what it holds steady taken off
ONCE static void detrend(const float *strength, int count, double *out) {
    double previous = 0, filtered = 0;
    for (int i = 0; i < count; ++i) {
        filtered = strength[i] - previous + 0.99 * filtered;
        previous = strength[i];
        out[i] = filtered;
    }
}

// The autocorrelation of x up to maxLag, as the mean of that of windows of it
// each against its own energy, so a loud part counts as much as a quiet one
ONCE static void autocorrelate(const double *x, int count, int window, int maxLag, double *acf) {
    memset(acf, 0, (maxLag + 1) * sizeof(double));
    if (window > count) window = count;
    int windows = 0;
    for (int start = 0; start + window <= count; start += window / 2) {
        double energy = 0;
        for (int i = start; i < start + window; ++i) energy += x[i] * x[i];
        if (energy <= 0) continue;
        for (int lag = 0; lag <= maxLag; ++lag) {
            double sum = 0;
            int end = start + window;
            if (end > count - lag) end = count - lag;
            for (int i = start; i < end; ++i) sum += x[i] * x[i + lag];
            acf[lag] += sum / energy;
        }
        windows++;
    }
    for (int lag = 0; windows > 0 && lag <= maxLag; ++lag) acf[lag] /= windows;
}

// The nearest whole number to a positive x; round() would be more wasm
static int nearest(double x) {
    return (int) floor(x + 0.5);
}

// e^x, with the pow() Chordino brings in; exp() would be more wasm
static double exponential(double x) {
    return pow(M_E, x);
}

// The median of count values, which it leaves partly sorted: a selection that
// is fine for the few thousand there are
ONCE static double median(double *values, int count) {
    int middle = count / 2;
    for (int i = 0; i <= middle; ++i) {
        int smallest = i;
        for (int j = i + 1; j < count; ++j) if (values[j] < values[smallest]) smallest = j;
        double value = values[i];
        values[i] = values[smallest];
        values[smallest] = value;
    }
    return values[middle];
}

ONCE static double interpolate(const double *values, int count, double at) {
    int below = (int) floor(at);
    if (below < 0) return values[0];
    if (below >= count - 1) return values[count - 1];
    double fraction = at - below;
    return values[below] * (1 - fraction) + values[below + 1] * fraction;
}

// The beat period, in frames, whose multiples the onset strength repeats at
// most, weighed by the prior
ONCE static double estimate_period(const float *strength, int count, double period) {
    double fastest = 60 / (maxBpm * period), slowest = 60 / (minBpm * period);
    int maxLag = (int) ceil(slowest * combSize) + 1;
    double *x = malloc(count * sizeof(double));
    double *acf = malloc((maxLag + 1) * sizeof(double));
    detrend(strength, count, x);
    autocorrelate(x, count, nearest(windowSeconds / period), maxLag, acf);
    double best = 0, bestScore = -INFINITY;
    for (double lag = fastest; lag <= slowest; lag += 0.25) {
        double sum = 0;
        for (int k = 1; k <= combSize; ++k) sum += interpolate(acf, maxLag + 1, lag * k);
        double octaves = log(60 / (lag * period) / tempoCentre) / M_LN2 / tempoSpread;
        double score = sum / combSize * exponential(-0.5 * octaves * octaves);
        if (score > bestScore) {
            bestScore = score;
            best = lag;
        }
    }
    free(x);
    free(acf);
    return best;
}

// Ellis's dynamic programming: the beats, as frames, that land on the most
// onset strength with intervals the closest to the period. Returns how many
// went into beats.
ONCE static int track_beats(const float *strength, int count, double lag, int *beats) {
    double mean = 0, deviation = 0;
    for (int i = 0; i < count; ++i) mean += strength[i];
    mean /= count;
    for (int i = 0; i < count; ++i) deviation += (strength[i] - mean) * (strength[i] - mean);
    deviation = sqrt(deviation / (count > 1 ? count - 1 : 1));
    if (!(deviation > 0)) return 0;

    // The onset strength smoothed over a 32nd of the period
    int reach = nearest(lag);
    double *kernel = malloc((reach * 2 + 1) * sizeof(double));
    for (int j = -reach; j <= reach; ++j) {
        double z = j * 32.0 / lag;
        kernel[j + reach] = exponential(-0.5 * z * z) / deviation;
    }
    double *score = malloc(count * sizeof(double));
    double maxScore = 0;
    for (int i = 0; i < count; ++i) {
        double sum = 0;
        for (int j = -reach; j <= reach; ++j) {
            if (i + j >= 0 && i + j < count) sum += strength[i + j] * kernel[j + reach];
        }
        score[i] = sum;
        if (sum > maxScore) maxScore = sum;
    }
    free(kernel);

    int closest = nearest(lag / 2), farthest = nearest(lag * 2);
    double *cost = malloc((farthest + 1) * sizeof(double));
    for (int d = closest; d <= farthest; ++d) {
        double octaves = log((double) d / lag);
        cost[d] = -tightness * octaves * octaves;
    }
    double *cumulative = malloc(count * sizeof(double));
    int *back = malloc(count * sizeof(int));
    bool first = true;
    for (int i = 0; i < count; ++i) {
        // Reaching back before the start costs only the interval
        double best = -INFINITY;
        int from = -1;
        for (int d = closest; d <= farthest; ++d) {
            double candidate = cost[d] + (i - d >= 0 ? cumulative[i - d] : 0);
            if (candidate > best) {
                best = candidate;
                from = i - d;
            }
        }
        cumulative[i] = score[i] + best;
        if (first && score[i] < 0.01 * maxScore) {
            back[i] = -1;
        } else {
            back[i] = from >= 0 ? from : -1;
            first = false;
        }
    }

    // The last beat: the last peak of the cumulative score above half their median
    int peaks = 0;
    double *values = malloc(count * sizeof(double));
    for (int i = 1; i + 1 < count; ++i) {
        if (cumulative[i] > cumulative[i - 1] && cumulative[i] >= cumulative[i + 1]) values[peaks++] = cumulative[i];
    }
    int beatCount = 0;
    if (peaks > 0) {
        double threshold = 0.5 * median(values, peaks);
        int last = -1;
        for (int i = count - 2; i >= 1 && last < 0; --i) {
            if (cumulative[i] > cumulative[i - 1] && cumulative[i] >= cumulative[i + 1] && cumulative[i] > threshold) last = i;
        }
        for (int i = last; i >= 0; i = back[i]) beats[beatCount++] = i;
        for (int i = 0; i < beatCount / 2; ++i) {
            int beat = beats[i];
            beats[i] = beats[beatCount - 1 - i];
            beats[beatCount - 1 - i] = beat;
        }
    }

    // Leading and trailing beats on next to nothing are silence, not music:
    // with the beats around them, or a tenth as much on their own
    if (beatCount > 0) {
        double *smooth = malloc(beatCount * sizeof(double));
        double energy = 0;
        static const double hann[5] = {0, 0.5, 1, 0.5, 0};
        for (int i = 0; i < beatCount; ++i) {
            double sum = 0;
            for (int j = -2; j <= 2; ++j) {
                if (i + j >= 0 && i + j < beatCount) sum += hann[j + 2] * score[beats[i + j]];
            }
            smooth[i] = sum;
            energy += sum * sum;
        }
        double threshold = 0.5 * sqrt(energy / beatCount);
        int start = 0, end = beatCount;
        while (start < end && (smooth[start] <= threshold || score[beats[start]] <= threshold / 10)) start++;
        while (end > start && (smooth[end - 1] <= threshold || score[beats[end - 1]] <= threshold / 10)) end--;
        memmove(beats, beats + start, (end - start) * sizeof(int));
        beatCount = end - start;
        free(smooth);
    }

    free(values);
    free(score);
    free(cost);
    free(cumulative);
    free(back);
    return beatCount;
}

ONCE static float peak_near(const float *values, int count, double at, int reach) {
    int centre = nearest(at);
    float best = 0;
    for (int i = centre - reach; i <= centre + reach; ++i) {
        if (i >= 0 && i < count && values[i] > best) best = values[i];
    }
    return best;
}

// Whether the beats split in three rather than two: the onset strength a third
// and two thirds into them against halfway, each over what the beats hold
// anywhere (the median of 24 points along them), and against the beat itself,
// for beats that do not split at all
ONCE static bool ternary(const float *strength, int count, const int *beats, int beatCount) {
    double profile[24] = {0}, sorted[24];
    for (int i = 0; i + 1 < beatCount; ++i) {
        double interval = beats[i + 1] - beats[i];
        for (int k = 0; k < 24; ++k) profile[k] += peak_near(strength, count, beats[i] + interval * k / 24, 1);
    }
    for (int k = 0; k < 24; ++k) {
        int at = k;
        while (at > 0 && sorted[at - 1] > profile[k]) {
            sorted[at] = sorted[at - 1];
            at--;
        }
        sorted[at] = profile[k];
    }
    double base = (sorted[11] + sorted[12]) / 2;
    double halves = profile[12] - base, thirds = (profile[8] + profile[16]) / 2 - base;
    return thirds > ternaryBias * fmax(halves, 0) && thirds > ternaryFloor * (profile[0] - base);
}

// How strongly each beat starts a bar: the chords that change on it and the
// bass that comes in with it, each against its mean over the beats
ONCE static void accents(const rhythm_t *rhythm, const chordino_result_t *chords, const int *beats, int beatCount,
                    double *accent) {
    const onset_t *onset = rhythm->onset;
    int count = onset->strength.count;
    const float *bass = onset->bass.data;
    double *changes = calloc(beatCount, sizeof(double));
    double *bassAt = malloc(beatCount * sizeof(double));
    // Each change goes to the beat it is closest to, a little early counting as on it
    if (chords) {
        int beat = 0;
        for (int c = 1; c + 1 < chords->chordCount; ++c) {
            double frame = (realtime_to_seconds(chords->chords[c].time) - rhythm->start) / onset->period;
            while (beat + 1 < beatCount && frame >= beats[beat + 1] - changeLead * (beats[beat + 1] - beats[beat])) beat++;
            if (beat == 0 && frame < beats[0] - changeLead * (beatCount > 1 ? beats[1] - beats[0] : 0)) continue;
            changes[beat] += 1;
        }
    }
    double changeMean = 0, bassMean = 0;
    for (int i = 0; i < beatCount; ++i) {
        bassAt[i] = peak_near(bass, count, beats[i], 2);
        changeMean += changes[i];
        bassMean += bassAt[i];
    }
    for (int i = 0; i < beatCount; ++i) {
        accent[i] = (changeMean > 0 ? changeWeight * changes[i] * beatCount / changeMean : 0) +
                    (bassMean > 0 ? bassWeight * bassAt[i] * beatCount / bassMean : 0);
    }
    free(changes);
    free(bassAt);
}

// How much more accented the beats every so many are than the others,
// counting from the beat that makes the most of it
ONCE static double contrast(const double *accent, int beatCount, int every) {
    double best = -INFINITY;
    for (int start = 0; start < every; ++start) {
        double on = 0, off = 0;
        int onCount = 0, offCount = 0;
        for (int i = 0; i < beatCount; ++i) {
            if ((i - start) % every == 0) {
                on += accent[i];
                onCount++;
            } else {
                off += accent[i];
                offCount++;
            }
        }
        if (onCount == 0 || offCount == 0) continue;
        best = fmax(best, on / onCount - off / offCount);
    }
    return best;
}

// Beats a bar: those whose downbeats are the most accented over the other
// beats. Whether the beats go in twos or
// threes comes first, as the more beats a bar the more ways there are for one
// of them to stand out by chance; then twos make 2 or 4 a bar, and threes 3,
// or 6 for beats as fast as the eighths of 6/8.
ONCE static int find_bars(const double *accent, int beatCount, double bpm) {
    double two = contrast(accent, beatCount, 2);
    double three = contrast(accent, beatCount, 3);
    double four = contrast(accent, beatCount, 4) * fourBias;
    double six = bpm >= eighthBpm ? contrast(accent, beatCount, 6) * sixBias : -INFINITY;
    if (three > fmax(two, four)) return six > three ? 6 : 3;
    return four >= two ? 4 : 2;
}

// Where in its bar of perBar each beat falls, from 0: the downbeats every
// perBar beats on the most accent, but free to start a bar anew at a cost,
// where a bar runs longer or shorter, or a beat went missing or in twice
ONCE static void place_bars(const double *accent, int beatCount, int perBar, int *position) {
    double mean = 0;
    for (int i = 0; i < beatCount; ++i) mean += accent[i];
    mean /= beatCount;
    double *score = malloc(beatCount * perBar * sizeof(double));
    int *back = malloc(beatCount * perBar * sizeof(int));
    for (int p = 0; p < perBar; ++p) score[p] = p == 0 ? accent[0] - mean : 0;
    for (int i = 1; i < beatCount; ++i) {
        const double *before = score + (i - 1) * perBar;
        for (int p = 0; p < perBar; ++p) {
            int from = (p + perBar - 1) % perBar;
            double best = before[from];
            for (int q = 0; q < perBar; ++q) {
                if (q != (p + perBar - 1) % perBar && before[q] - jumpCost > best) {
                    best = before[q] - jumpCost;
                    from = q;
                }
            }
            score[i * perBar + p] = best + (p == 0 ? accent[i] - mean : 0);
            back[i * perBar + p] = from;
        }
    }
    int p = 0;
    for (int q = 1; q < perBar; ++q) {
        if (score[(beatCount - 1) * perBar + q] > score[(beatCount - 1) * perBar + p]) p = q;
    }
    for (int i = beatCount - 1; i >= 0; --i) {
        position[i] = p;
        if (i > 0) p = back[i * perBar + p];
    }
    free(score);
    free(back);
}

ONCE const rhythm_result_t *rhythm_decode(rhythm_t *rhythm, const chordino_result_t *chords) {
    onset_t *onset = rhythm->onset;
    onset_finish(onset);
    vector_clear(&rhythm->beats);
    memset(&rhythm->result, 0, sizeof(rhythm_result_t));
    int count = onset->strength.count;
    const float *strength = onset->strength.data;
    if (count < 2) return &rhythm->result;

    double lag = estimate_period(strength, count, onset->period);
    int *beats = malloc(count * sizeof(int));
    int beatCount = lag > 0 ? track_beats(strength, count, lag, beats) : 0;
    if (beatCount < 2) {
        free(beats);
        return &rhythm->result;
    }

    double *accent = malloc(beatCount * sizeof(double));
    accents(rhythm, chords, beats, beatCount, accent);
    int perBar = find_bars(accent, beatCount, 60 / (lag * onset->period));
    int *positions = malloc(beatCount * sizeof(int));
    place_bars(accent, beatCount, perBar, positions);
    free(accent);

    // A compound meter counts the eighths: the thirds of beats that split in
    // three, or the beats themselves when they are the eighths of 6/8
    int split = perBar != 6 && ternary(strength, count, beats, beatCount) ? 3 : 1;
#ifdef RHYTHM_TUNE
    rhythmSplit = split;
#endif
    rhythm_result_t *result = &rhythm->result;
    result->numerator = perBar * split;
    result->denominator = split == 3 || perBar == 6 ? 8 : 4;
    for (int i = 0; i < beatCount; ++i) {
        for (int part = 0; part < split; ++part) {
            if (part > 0 && i + 1 == beatCount) break;
            double frame = part == 0 ? beats[i] : beats[i] + (beats[i + 1] - beats[i]) * part / (double) split;
            rhythm_beat_t *beat = vector_push(&rhythm->beats, NULL, 1);
            beat->time = rhythm->start + frame * onset->period;
            beat->position = positions[i] * split + part + 1;
        }
    }
    // The tempo is the median interval between beats
    double *intervals = malloc((beatCount - 1) * sizeof(double));
    for (int i = 0; i + 1 < beatCount; ++i) intervals[i] = beats[i + 1] - beats[i];
    result->bpm = 60 / (median(intervals, beatCount - 1) * onset->period) * split;
    free(intervals);
    free(positions);
    free(beats);
    result->beatCount = rhythm->beats.count;
    result->beats = rhythm->beats.data;
    return result;
}

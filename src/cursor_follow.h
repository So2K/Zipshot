#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

// Pure normalized camera model. No cursor reads, capture, window or encoder APIs.
// Times use the recorded video clock: callers remove pause gaps before logging.
// Coordinates are relative to the ORIGINAL capture rectangle, not the monitor.
namespace cursor_follow {

struct Sample {
    double t = 0, x = .5, y = .5;
    bool enabled = false, visible = true, paused = false;
};

struct Config {
    double zoom = 1.6, deadZoneFraction = .2;
    double panSeconds = .25, zoomSeconds = .3, missingHoldSeconds = .35;
};

struct Camera {
    double t = 0, left = 0, top = 0, width = 1, height = 1, zoom = 1;
};

namespace detail {

inline double Bounded(double value, double low, double high, double fallback) {
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

inline Config Sanitize(Config c) {
    c.zoom = Bounded(c.zoom, 1, 4, 1);
    c.deadZoneFraction = Bounded(c.deadZoneFraction, 0, .8, .2);
    c.panSeconds = Bounded(c.panSeconds, .04, 3, .25);
    c.zoomSeconds = Bounded(c.zoomSeconds, .04, 3, .3);
    c.missingHoldSeconds = Bounded(c.missingHoldSeconds, 0, 3, .35);
    return c;
}

inline bool Visible(const Sample& s) {
    return s.visible && std::isfinite(s.x) && std::isfinite(s.y) &&
           s.x >= 0 && s.x <= 1 && s.y >= 0 && s.y <= 1;
}

struct Spring {
    double position = .5, velocity = 0;

    void Advance(double target, double responseSeconds, double dt) {
        // Exact solution of p'' + 2*w*p' + w*w*(p-target) = 0 for a
        // constant target. No frame-count-dependent lerp or Euler instability.
        const double w = 6 / responseSeconds;
        const double displacement = position - target;
        const double coefficient = velocity + w * displacement;
        const double decay = std::exp(-w * dt);
        position = target + (displacement + coefficient * dt) * decay;
        velocity = (velocity - w * coefficient * dt) * decay;
    }
};

struct Node { double t, x, y, zoom; };

inline Camera View(double t, double x, double y, double zoom) {
    Camera camera;
    camera.t = t;
    camera.zoom = std::max(1.0, zoom);
    camera.width = camera.height = 1 / camera.zoom;
    const double travel = std::max(0.0, 1 - camera.width);
    camera.left = std::clamp(x - camera.width / 2, 0.0, travel);
    camera.top = std::clamp(y - camera.height / 2, 0.0, travel);
    return camera;
}

struct Motion {
    Config config;
    Spring x, y, zoom{1, 0};
    double aimX = .5, aimY = .5, invalidSince = -1;
    bool enabled = false, hasFocus = false, paused = false;

    void Accept(const Sample& s) {
        paused = s.paused;
        if (paused) return;  // A defensive hold; pause time normally is omitted.
        enabled = s.enabled;
        if (!enabled) {
            hasFocus = false;
            invalidSince = -1;
            return;
        }
        if (!Visible(s)) {
            if (hasFocus && invalidSince < 0) invalidSince = s.t;
            return;
        }
        const double halfDeadZone = config.deadZoneFraction / (2 * config.zoom);
        if (!hasFocus) {
            aimX = s.x;
            aimY = s.y;
        } else {
            // Move only enough to bring the pointer back onto the dead-zone
            // edge. Small motions inside it leave the framing target alone.
            aimX += std::max(0.0, s.x - aimX - halfDeadZone) +
                    std::min(0.0, s.x - aimX + halfDeadZone);
            aimY += std::max(0.0, s.y - aimY - halfDeadZone) +
                    std::min(0.0, s.y - aimY + halfDeadZone);
        }
        if (zoom.position <= 1.000001) {
            // Pre-aim while the full frame is visible, so enabling zoom aims
            // at this cursor instead of visibly travelling from screen centre.
            x = {aimX, 0};
            y = {aimY, 0};
        }
        hasFocus = true;
        invalidSince = -1;
    }

    void Advance(double start, double dt) {
        if (paused) {
            if (invalidSince >= 0) invalidSince += dt;
            return;
        }
        const bool hold = invalidSince < 0 || start - invalidSince < config.missingHoldSeconds;
        const double target = enabled && hasFocus && hold ? config.zoom : 1;
        x.Advance(aimX, config.panSeconds, dt);
        y.Advance(aimY, config.panSeconds, dt);
        zoom.Advance(target, config.zoomSeconds, dt);
        if (zoom.position < 1 || zoom.position > config.zoom) {
            zoom.position = std::clamp(zoom.position, 1.0, config.zoom);
            zoom.velocity = 0;
        }
    }
};

}  // namespace detail

// One crop per output frame, at t = frame / fps, for [0, durationSeconds).
// GIF/video/output resolution does not change the camera motion. Simulation uses
// a shared 120 Hz clock plus exact input-event boundaries, then samples that
// same continuous path at the requested output fps. Input order is immaterial;
// the last sample wins for equal timestamps. Negative/nonfinite times are ignored.
// Hidden/outside pointers hold the last focus briefly, then ease to full frame.
// Positions persist between samples; a logger must explicitly mark a hidden or
// unavailable cursor. No samples, or no valid enabled cursor, means full frame.
inline std::vector<Camera> BuildPath(const std::vector<Sample>& samples,
                                      double durationSeconds, double fps, Config config = {}) {
    if (!std::isfinite(durationSeconds) || !std::isfinite(fps) || durationSeconds <= 0 || fps <= 0)
        return {};
    const double count = std::ceil(std::nextafter(durationSeconds * fps, 0.0));
    const double steps = std::ceil(durationSeconds * 120);
    if (!std::isfinite(count) || !std::isfinite(steps) || count <= 0 ||
        count >= static_cast<double>(std::vector<Camera>().max_size()) ||
        steps >= static_cast<double>(std::vector<double>().max_size()) / 2) return {};
    config = detail::Sanitize(config);

    std::vector<Sample> input;
    for (const Sample& s : samples)
        if (std::isfinite(s.t) && s.t >= 0 && s.t <= durationSeconds) input.push_back(s);
    std::stable_sort(input.begin(), input.end(), [](const Sample& a, const Sample& b) { return a.t < b.t; });
    std::vector<Sample> unique;
    for (const Sample& s : input) {
        if (!unique.empty() && unique.back().t == s.t) unique.back() = s;
        else unique.push_back(s);
    }

    std::vector<double> times{0, durationSeconds};
    for (size_t tick = 1; tick < static_cast<size_t>(steps); ++tick) times.push_back(tick / 120.0);
    for (const Sample& s : unique) {
        times.push_back(s.t);
        const double expiry = s.t + config.missingHoldSeconds;
        if (!detail::Visible(s) && expiry <= durationSeconds) times.push_back(expiry);
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    detail::Motion motion{config};
    std::vector<detail::Node> nodes;
    nodes.reserve(times.size());
    size_t event = 0;
    double previous = 0;
    for (double t : times) {
        motion.Advance(previous, t - previous);
        while (event < unique.size() && unique[event].t <= t) motion.Accept(unique[event++]);
        nodes.push_back({t, motion.x.position, motion.y.position, motion.zoom.position});
        previous = t;
    }

    std::vector<Camera> frames;
    frames.reserve(static_cast<size_t>(count));
    size_t interval = 0;
    for (size_t frame = 0; frame < static_cast<size_t>(count); ++frame) {
        const double t = frame / fps;
        while (interval + 1 < nodes.size() && nodes[interval + 1].t <= t) ++interval;
        const auto& a = nodes[interval];
        const auto& b = nodes[std::min(interval + 1, nodes.size() - 1)];
        const double u = b.t > a.t ? (t - a.t) / (b.t - a.t) : 0;
        frames.push_back(detail::View(t, std::lerp(a.x, b.x, u), std::lerp(a.y, b.y, u),
                                      std::lerp(a.zoom, b.zoom, u)));
    }
    return frames;
}

}  // namespace cursor_follow

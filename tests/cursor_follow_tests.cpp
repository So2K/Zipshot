// Synthetic cursor timelines only. No Windows, real cursor, input or capture APIs.
#include "../src/cursor_follow.h"
#include <cstdio>

namespace {
using cursor_follow::Camera;
using cursor_follow::Config;
using cursor_follow::Sample;
using cursor_follow::BuildPath;
int checks = 0, failures = 0;
void Check(bool ok, const char* message) {
    ++checks;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failures;
}
bool Near(double a, double b, double tolerance = 1e-8) { return std::abs(a - b) <= tolerance; }
bool SamePose(const Camera& a, const Camera& b, double tolerance = 1e-8) {
    return Near(a.left, b.left, tolerance) && Near(a.top, b.top, tolerance) &&
           Near(a.width, b.width, tolerance) && Near(a.height, b.height, tolerance) &&
           Near(a.zoom, b.zoom, tolerance);
}
bool Full(const std::vector<Camera>& frames) {
    return std::all_of(frames.begin(), frames.end(), [](const Camera& c) {
        return Near(c.left, 0) && Near(c.top, 0) && Near(c.width, 1) && Near(c.height, 1) && Near(c.zoom, 1);
    });
}
bool Bounded(const std::vector<Camera>& frames) {
    return std::all_of(frames.begin(), frames.end(), [](const Camera& c) {
        return std::isfinite(c.left) && std::isfinite(c.top) && std::isfinite(c.width) &&
               std::isfinite(c.height) && std::isfinite(c.zoom) &&
               c.left >= 0 && c.top >= 0 && c.width > 0 && c.width <= 1 &&
               c.left + c.width <= 1.000000000001 && c.top + c.height <= 1.000000000001 &&
               Near(c.width, c.height) && Near(c.width * c.zoom, 1);
    });
}
}  // namespace

int main() {
    Config config;
    config.zoom = 2;
    const auto empty = BuildPath({}, 2, 30, config);
    Check(empty.size() == 60 && Full(empty), "missing cursor data keeps complete video full frame");
    Check(Full(BuildPath({{0, .9, .1, false}}, 2, 30, config)), "disabled follow does not crop valid cursor position");
    const auto centre = BuildPath({{0, .5, .5, true}}, 2, 30, config);
    Check(centre.size() == 60 && SamePose(centre.front(), Camera{}), "follow begins at original full frame without a cut");
    Check(centre[3].zoom > 1 && centre[3].zoom < 2 && Near(centre.back().zoom, 2), "zoom eases toward configured amount");
    Check(Near(centre.back().left, .25) && Near(centre.back().top, .25) &&
          Near(centre.back().width, .5), "stationary centred cursor settles on centred 2x crop");
    bool monotonic = true;
    for (size_t i = 1; i < centre.size(); ++i) monotonic &= centre[i].zoom >= centre[i - 1].zoom && centre[i].zoom <= 2;
    Check(monotonic, "single zoom-in has no overshoot or oscillation");
    const auto corner = BuildPath({{0, 0, 0, true}}, 2, 60, config);
    const auto opposite = BuildPath({{0, 1, 1, true}}, 2, 60, config);
    Check(Bounded(corner) && Near(corner.back().left, 0) && Near(corner.back().top, 0), "top-left focus never exposes pixels outside capture");
    Check(Bounded(opposite) && Near(opposite.back().left + opposite.back().width, 1) &&
          Near(opposite.back().top + opposite.back().height, 1), "bottom-right focus remains flush with capture edges");
    const auto off = BuildPath({{0, .2, .3, true}, {1, .2, .3, false}}, 3, 60, config);
    Check(Near(off[60].zoom, off[59].zoom, .00001) && off[72].zoom < 2 && off[72].zoom > 1 &&
          SamePose(off.back(), Camera{}, .000001), "live off toggle eases back to full frame without an abrupt cut");

    std::vector<Sample> jitter{{0, .5, .5, true}};
    for (int i = 0; i < 30; ++i) jitter.push_back({1 + i / 30.0, .5 + (i % 2 ? .02 : -.02), .51, true});
    const auto steady = BuildPath(jitter, 2, 60, config);
    Check(std::all_of(steady.begin(), steady.end(), [](const Camera& c) {
        return Near(c.left + c.width / 2, .5) && Near(c.top + c.height / 2, .5);
    }), "small cursor jitter inside dead zone produces no camera panning");
    const std::vector<Sample> panSamples{{0, .5, .5, true}, {1, .7, .5, true}};
    const auto pan = BuildPath(panSamples, 3, 60, config);
    Check(Near(pan[59].left + pan[59].width / 2, .5) && Near(pan[60].left + pan[60].width / 2, .5),
          "camera does not anticipate a future cursor move");
    const double movingCentre = pan[66].left + pan[66].width / 2;
    Check(movingCentre > .5 && movingCentre < .65 && Near(pan.back().left + pan.back().width / 2, .65),
          "deliberate movement pans smoothly to dead-zone edge, then settles");
    const std::vector<Sample> irregular{{0, .4, .4, true}, {.437, .7, .6, true},
                                         {.713, .7, .6, false}, {1.217, .2, .3, true}};
    const auto thirty = BuildPath(irregular, 2.5, 30, config);
    const auto sixty = BuildPath(irregular, 2.5, 60, config);
    const auto oneTwenty = BuildPath(irregular, 2.5, 120, config);
    bool sameClock = true;
    for (size_t i = 0; i < thirty.size(); ++i)
        sameClock &= SamePose(thirty[i], sixty[i * 2], 1e-12) && SamePose(thirty[i], oneTwenty[i * 4], 1e-12);
    Check(sameClock, "30, 60 and 120 fps sample exactly the same timed camera path");
    const auto smallCapture = BuildPath({{0, 128.0 / 256, 72.0 / 144, true},
                                          {1, 176.0 / 256, 72.0 / 144, true}}, 2, 30, config);
    const auto largeCapture = BuildPath({{0, 1920.0 / 3840, 1080.0 / 2160, true},
                                          {1, 2640.0 / 3840, 1080.0 / 2160, true}}, 2, 30, config);
    bool sameSize = smallCapture.size() == largeCapture.size();
    for (size_t i = 0; i < smallCapture.size(); ++i) sameSize &= SamePose(smallCapture[i], largeCapture[i]);
    Check(sameSize, "equivalent cursor positions on small and 4K captures give identical normalized motion");

    const auto hidden = BuildPath({{0, .5, .5, true}, {1, .5, .5, true, false}}, 3, 60, config);
    Check(hidden[78].zoom > 1.99 && hidden[96].zoom < 1.2 && SamePose(hidden.back(), Camera{}, .000001),
          "hidden cursor holds briefly then gracefully widens to full frame");
    const auto brief = BuildPath({{0, .5, .5, true}, {1, .5, .5, true, false}, {1.1, .5, .5, true}}, 2, 60, config);
    Check(brief[90].zoom > 1.9999, "brief cursor invisibility does not pump zoom out and in");
    const auto outside = BuildPath({{0, .5, .5, true}, {1, -10, 5, true}}, 3, 60, config);
    Check(Bounded(outside) && SamePose(outside.back(), Camera{}, .000001), "cursor leaving capture is not followed onto another screen");
    Check(Full(BuildPath({{0, 1.1, -.1, true}}, 2, 30, config)), "outside cursor before first valid position never activates zoom");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    Check(Full(BuildPath({{0, nan, .5, true}}, 2, 30, config)), "nonfinite cursor position is treated as unavailable");
    const auto paused = BuildPath({{0, .2, .3, true}, {1, .9, .9, true, true, true},
                                    {2, .2, .3, true}}, 3, 60, config);
    Check(SamePose(paused[60], paused[90]) && SamePose(paused[60], paused[120]),
          "explicit pause marker freezes camera and ignores cursor movement during pause");
    const auto neverEnabled = BuildPath({{0, .2, .2, false}, {1, .9, .9, true, false}}, 2, 30, config);
    Check(Full(neverEnabled), "enabling with no available cursor does not reuse stale focus");

    const std::vector<Sample> unordered{{1, .7, .5, true}, {0, .5, .5, true}, {1, .6, .5, true}, {-1, 0, 0, true}, {nan, 0, 0, true}};
    const auto sorted = BuildPath(unordered, 3, 30, config);
    Check(Near(sorted.back().left + sorted.back().width / 2, .55), "unordered samples sort by time and last equal-time sample wins");
    Check(unordered[0].t == 1 && unordered[0].x == .7 && unordered[1].t == 0 && unordered[2].x == .6,
          "path building leaves caller timeline and coordinates unchanged");
    Config bad;
    bad.zoom = nan;
    bad.panSeconds = -100;
    bad.zoomSeconds = std::numeric_limits<double>::infinity();
    bad.deadZoneFraction = -1;
    Check(Full(BuildPath({{0, .2, .3, true}}, 2, 30, bad)), "invalid configuration sanitizes to finite safe full-frame result");
    Check(BuildPath({}, 0, 30).empty() && BuildPath({}, nan, 30).empty() &&
          BuildPath({}, 1, 0).empty() && BuildPath({}, 1, nan).empty(), "invalid duration or frame rate returns no frames");
    const auto fractional = BuildPath({}, 2.1, 29.97);
    Check(fractional.size() == 63 && fractional.back().t < 2.1, "fractional frame rate produces correct half-open frame timeline");
    Check(BuildPath({}, 239.0 / 30, 30).size() == 239, "duration division roundoff does not create an extra terminal frame");
    Config onePointFive;
    onePointFive.zoom = 1.5;
    const auto gentle = BuildPath({{0, .5, .5, true}}, 2, 30, onePointFive);
    Check(Near(gentle.back().zoom, 1.5) && Near(gentle.back().width, 2.0 / 3), "gentle 1.5x preset keeps its configured magnification");
    std::printf("%d assertions, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

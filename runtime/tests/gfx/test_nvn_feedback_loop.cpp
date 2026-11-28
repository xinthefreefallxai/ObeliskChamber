// SPDX-License-Identifier: GPL-3.0-or-later
// G4: the feedback loop decisions ([nvn] feedback_loop_layout, src/gfx/nvn/nvn_feedback_loop.h).
#include <cstdint>

#include "nvn_feedback_loop.h"
#include "test_framework.h"

NXTEST_GROUP(gfx_nvn_feedback_loop)

using namespace nxrt::nvn;

namespace {
// A single-level, single-layer target that the draw samples at the same level and layer (the bloom composite's case).
FbLoopCase pointwise() {
    FbLoopCase c;
    c.supported = true;
    c.image_usage = true;
    return c;
}
}  // namespace

NXTEST(mode_parses_and_defaults_off) {
    CHECK(parse_fb_loop_mode("") == FbLoopMode::Off);
    CHECK(parse_fb_loop_mode("false") == FbLoopMode::Off);
    CHECK(parse_fb_loop_mode("0") == FbLoopMode::Off);
    CHECK(parse_fb_loop_mode("off") == FbLoopMode::Off);
    CHECK(parse_fb_loop_mode("maybe") == FbLoopMode::Off);   // not understood: the default
    CHECK(parse_fb_loop_mode("true") == FbLoopMode::On);
    CHECK(parse_fb_loop_mode(" TRUE ") == FbLoopMode::On);
    CHECK(parse_fb_loop_mode("1") == FbLoopMode::On);
    CHECK(parse_fb_loop_mode("yes") == FbLoopMode::On);
    CHECK(parse_fb_loop_mode("Auto") == FbLoopMode::Auto);
}

NXTEST(intel_compat_forces_it_off) {
    CHECK(!fb_loop_wanted(FbLoopMode::Off, false));
    CHECK(fb_loop_wanted(FbLoopMode::On, false));
    CHECK(fb_loop_wanted(FbLoopMode::Auto, false));
    CHECK(!fb_loop_wanted(FbLoopMode::On, true));
    CHECK(!fb_loop_wanted(FbLoopMode::Auto, true));
}

NXTEST(ranges_overlap_half_open) {
    CHECK(fb_ranges_overlap(0, 1, 0, 1));
    CHECK(!fb_ranges_overlap(0, 1, 1, 2));
    CHECK(!fb_ranges_overlap(1, 2, 0, 1));
    CHECK(fb_ranges_overlap(0, 4, 3, 4));
    CHECK(!fb_ranges_overlap(0, 0, 0, 1));   // empty
}

NXTEST(unsupported_or_unfit_takes_the_copy) {
    FbLoopCase c = pointwise();
    c.supported = false;
    CHECK(fb_loop_decide(c).path == FbLoopPath::Copy);
    c = pointwise();
    c.image_usage = false;
    CHECK(fb_loop_decide(c).path == FbLoopPath::Copy);
    c = pointwise();
    c.samples = 4;
    CHECK(fb_loop_decide(c).path == FbLoopPath::Copy);
    CHECK(fb_loop_decide(c).why[0] != '\0');
}

NXTEST(pointwise_read_needs_its_own_pass) {
    const FbLoopDecision d = fb_loop_decide(pointwise());
    CHECK(d.path == FbLoopPath::Layout);
    CHECK(d.overlap);
}

NXTEST(mip_chain_reads_another_level) {
    // renders level 2 while it samples level 1 (a blur or bloom chain over one mipmapped texture)
    FbLoopCase c = pointwise();
    c.att_level = 2;
    c.lv_lo = 1;
    c.lv_hi = 2;
    FbLoopDecision d = fb_loop_decide(c);
    CHECK(d.path == FbLoopPath::Layout);
    CHECK(!d.overlap);
    // the views name every level: the attachment's level is among them
    c.lv_lo = 0;
    c.lv_hi = 5;
    CHECK(fb_loop_decide(c).overlap);
    // ranges not known exactly (3D, cube, a level drop): overlap assumed
    c.lv_lo = 1;
    c.lv_hi = 2;
    c.exact_ranges = false;
    CHECK(fb_loop_decide(c).overlap);
}

NXTEST(other_layer_does_not_overlap) {
    FbLoopCase c = pointwise();
    c.att_layer = 3;
    c.att_layers = 1;
    c.ly_lo = 0;
    c.ly_hi = 3;
    CHECK(!fb_loop_decide(c).overlap);
    c.ly_hi = 4;
    CHECK(fb_loop_decide(c).overlap);
}

NXTEST(open_pass_serves_only_what_it_can) {
    // no pass, or other targets: never
    CHECK(!fb_pass_serves(false, 0, 0, false, 0));
    CHECK(!fb_pass_serves(false, 1, 1, false, 0));
    // plain pass, plain draw: as before
    CHECK(fb_pass_serves(true, 0, 0, false, 10));
    // plain pass, feedback draw: a feedback pass has to begin
    CHECK(!fb_pass_serves(true, 0, 1, false, 0));
    // feedback pass, plain draw (or clear): a plain pass again
    CHECK(!fb_pass_serves(true, 1, 0, false, 0));
    // feedback pass, feedback draw on another level: stays, whatever was drawn
    CHECK(fb_pass_serves(true, 1, 1, false, 7));
    // feedback pass, pointwise feedback draw: only as the pass's first draw
    CHECK(fb_pass_serves(true, 1, 1, true, 0));
    CHECK(!fb_pass_serves(true, 1, 1, true, 1));
    // the draw needs an attachment the pass does not have in the layout
    CHECK(!fb_pass_serves(true, 1, 3, false, 0));
    CHECK(fb_pass_serves(true, 3, 1, false, 0));
}

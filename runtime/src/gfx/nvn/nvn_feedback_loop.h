// SPDX-License-Identifier: GPL-3.0-or-later
// G4: colour feedback loops through VK_EXT_attachment_feedback_loop_layout ([nvn] feedback_loop_layout, nvn_replay.cpp feedback_prepare).
// The decisions, pure so the tests can call them: what the switch's value means, whether one sampled texture of a draw that is also one
// of its colour attachments can be served in place (attachment and descriptor in VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT)
// or takes the copy (feedback_clone), and whether the open pass can serve the draw or a new one has to begin.
//
// Why a pass still begins for some draws: the replay renders with dynamic rendering (vkCmdBeginRendering), and inside such a render pass
// instance Vulkan allows a pipeline barrier only with dynamicRenderingLocalRead (VUID-vkCmdPipelineBarrier2-None-09553), and even then
// not one that makes attachment writes visible to sampled reads (the images must be in RENDERING_LOCAL_READ or GENERAL, 09555). The
// by-region self-dependency Ryujinx records between draws belongs to its VkRenderPass subpasses. So a draw whose sampled subresources
// overlap what the pass writes (a pointwise read-modify-write of the same level and layer) gets a pass of its own that begins after a full
// barrier, unless the open feedback pass has recorded no draw yet. A draw that samples other levels or layers of its attachment's image
// (a blur or bloom chain over one mipmapped texture) has nothing to wait for inside the pass: it stays in the open feedback pass.
#pragma once

#include <cstdint>
#include <string_view>

namespace nxrt::nvn {

enum class FbLoopMode : uint8_t {
    Off,    // the copy path only (the default); the extension is not even enabled on the device
    On,     // the layout path where the device has the extension and its feature (a warning when it has not)
    Auto,   // the same as On, without the warning
};

// [nvn] feedback_loop_layout: false / 0 / off / no (and anything not understood) -> Off; true / 1 / on / yes -> On; auto -> Auto.
inline FbLoopMode parse_fb_loop_mode(std::string_view v) {
    auto lower_eq = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            char c = a[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != b[i]) return false;
        }
        return true;
    };
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
    if (lower_eq(v, "auto")) return FbLoopMode::Auto;
    if (lower_eq(v, "1") || lower_eq(v, "true") || lower_eq(v, "on") || lower_eq(v, "yes")) return FbLoopMode::On;
    return FbLoopMode::Off;
}

inline const char* fb_loop_mode_name(FbLoopMode m) { return m == FbLoopMode::On ? "true" : m == FbLoopMode::Auto ? "auto" : "false"; }

// Whether the device is asked for the extension at all. [gfx] intel_compat forces the copy path (the layout path was never run on
// Intel's driver).
inline bool fb_loop_wanted(FbLoopMode m, bool intel_compat) { return m != FbLoopMode::Off && !intel_compat; }

// Half-open ranges [a0, a1) and [b0, b1) share an element.
inline bool fb_ranges_overlap(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) { return a0 < b1 && b0 < a1; }

// One sampled texture of a draw that is also a colour attachment of the draw's targets.
struct FbLoopCase {
    bool supported = false;         // the device has the extension and attachmentFeedbackLoopLayout, and the switch is on
    bool image_usage = false;       // the image was made with VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT
    bool att_view_ok = true;        // the attachment view's format can be sampled and rendered to (so the view keeps that usage)
    uint32_t samples = 1;           // of the image
    bool exact_ranges = true;       // false: the subresource ranges below are not known exactly (3D, cube, a level drop): overlap assumed
    uint32_t att_level = 0;         // the attachment's level and layers [att_layer, att_layer + att_layers)
    uint32_t att_layer = 0, att_layers = 1;
    uint32_t lv_lo = 0, lv_hi = 1;  // the levels [lv_lo, lv_hi) and layers [ly_lo, ly_hi) the draw's views of the image sample
    uint32_t ly_lo = 0, ly_hi = 1;
};

enum class FbLoopPath : uint8_t { Copy, Layout };

struct FbLoopDecision {
    FbLoopPath path = FbLoopPath::Copy;
    bool overlap = true;            // Layout: the sampled subresources include what the pass writes (needs a pass begun just for it)
    const char* why = "";           // Copy: why the layout path does not fit (the fallback counter's log line)
};

inline FbLoopDecision fb_loop_decide(const FbLoopCase& c) {
    FbLoopDecision d;
    if (!c.supported) { d.why = "off or not supported"; return d; }
    if (!c.image_usage) { d.why = "image made without the feedback loop usage"; return d; }
    if (!c.att_view_ok) { d.why = "the attachment view's format cannot be sampled (its view has no feedback loop usage)"; return d; }
    if (c.samples > 1) { d.why = "multisampled"; return d; }   // (the copy path leaves those to the dummy as before)
    d.path = FbLoopPath::Layout;
    d.overlap = !c.exact_ranges ||
                (fb_ranges_overlap(c.lv_lo, c.lv_hi, c.att_level, c.att_level + 1) &&
                 fb_ranges_overlap(c.ly_lo, c.ly_hi, c.att_layer, c.att_layer + (c.att_layers ? c.att_layers : 1)));
    return d;
}

// Whether the open pass serves a draw (or a clear). pass_fb_mask: the colour attachments the open pass has in the feedback loop layout
// (non-zero only for a pass begun for a feedback draw, after a full barrier); want_mask: those the draw needs in it (0: not a feedback
// draw); fresh: the draw samples what the pass writes; draws_in_pass: draws recorded in the open pass. A feedback pass serves feedback
// draws only: only those get pipelines made with the feedback loop flag (a draw or a clear's mask pipeline without it gets a plain pass
// again), which keeps the extra pipelines to the few that sample their own target.
inline bool fb_pass_serves(bool open_and_matching, uint32_t pass_fb_mask, uint32_t want_mask, bool fresh, uint32_t draws_in_pass) {
    if (!open_and_matching) return false;
    if ((pass_fb_mask & want_mask) != want_mask) return false;                                // a needed attachment is not in the layout
    if (pass_fb_mask && !want_mask) return false;                                             // a plain draw after the feedback draws
    if (want_mask && fresh && draws_in_pass > 0) return false;                                // it reads what this pass wrote
    return true;
}

}  // namespace nxrt::nvn

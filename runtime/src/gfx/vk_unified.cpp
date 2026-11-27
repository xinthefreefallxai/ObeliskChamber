// SPDX-License-Identifier: GPL-3.0-or-later
// [gfx] unified_layouts (default false; VK_KHR_unified_image_layouts): every image the runtime uses is in VK_IMAGE_LAYOUT_GENERAL.
//
// With the extension's unifiedImageLayouts feature, the general layout is as fast as the specialised ones for every use but
// presenting, so a driver has no layout transition to do between a render target, a sampled texture and a copy. This first version
// keeps all the runtime's own layout tracking and barriers as they are and rewrites the layouts at the Vulkan calls instead (volk's
// function pointers, as the memory counters in vk_device.cpp): barriers keep their stages and accesses (the memory dependencies
// stay) with GENERAL on both sides, and descriptors, attachments, copies, blits, clears and resolves name GENERAL. UNDEFINED,
// PREINITIALIZED and the present layouts are left as they are.
//
// Not covered: third-party SDKs that record with their own function table (DLSS through Streamline, FSR, XeSS) are told the layouts the
// runtime tracks, which then differ from GENERAL. Use the built-in upscaler with it.
#include "vk_unified.h"

#include <atomic>
#include <vector>

#include "nxrt/log.h"

namespace nxrt::gfx {
namespace {

constexpr const char* kTag = "vk.unified";

PFN_vkCmdPipelineBarrier o_barrier;
PFN_vkCmdPipelineBarrier2 o_barrier2;
PFN_vkCmdBeginRendering o_begin_rendering;
PFN_vkUpdateDescriptorSets o_update_sets;
PFN_vkCmdPushDescriptorSetKHR o_push_set;
PFN_vkCmdCopyImage o_copy_image;
PFN_vkCmdCopyImageToBuffer o_copy_i2b;
PFN_vkCmdCopyImageToBuffer2 o_copy_i2b2;
PFN_vkCmdCopyBufferToImage o_copy_b2i;
PFN_vkCmdCopyBufferToImage2 o_copy_b2i2;
PFN_vkCmdBlitImage o_blit;
PFN_vkCmdClearColorImage o_clear_color;
PFN_vkCmdClearDepthStencilImage o_clear_ds;
PFN_vkCmdResolveImage o_resolve;

std::atomic<bool> g_on{false};

inline VkImageLayout gen(VkImageLayout l) {
    switch (l) {
    case VK_IMAGE_LAYOUT_UNDEFINED:
    case VK_IMAGE_LAYOUT_PREINITIALIZED:
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
    case VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR:
        return l;
    default:
        return VK_IMAGE_LAYOUT_GENERAL;
    }
}

bool image_descriptor(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
           t == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}

// Per-thread scratch: the structs the caller passed, copied with the layouts rewritten (no allocation once the vectors have grown).
struct Scratch {
    std::vector<VkImageMemoryBarrier> ib;
    std::vector<VkImageMemoryBarrier2> ib2;
    std::vector<VkDependencyInfo> dep;
    std::vector<VkRenderingAttachmentInfo> att;
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorImageInfo> images;
};
thread_local Scratch t_s;

VKAPI_ATTR void VKAPI_CALL w_barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dep,
                                     uint32_t nm, const VkMemoryBarrier* m, uint32_t nb, const VkBufferMemoryBarrier* b, uint32_t ni,
                                     const VkImageMemoryBarrier* im) {
    t_s.ib.assign(im, im + ni);
    for (VkImageMemoryBarrier& x : t_s.ib) {
        x.oldLayout = gen(x.oldLayout);
        x.newLayout = gen(x.newLayout);
    }
    o_barrier(cb, src, dst, dep, nm, m, nb, b, ni, t_s.ib.data());
}

VKAPI_ATTR void VKAPI_CALL w_barrier2(VkCommandBuffer cb, const VkDependencyInfo* d) {
    if (!d->imageMemoryBarrierCount) {
        o_barrier2(cb, d);
        return;
    }
    t_s.ib2.assign(d->pImageMemoryBarriers, d->pImageMemoryBarriers + d->imageMemoryBarrierCount);
    for (VkImageMemoryBarrier2& x : t_s.ib2) {
        x.oldLayout = gen(x.oldLayout);
        x.newLayout = gen(x.newLayout);
    }
    VkDependencyInfo c = *d;
    c.pImageMemoryBarriers = t_s.ib2.data();
    o_barrier2(cb, &c);
}

VKAPI_ATTR void VKAPI_CALL w_begin_rendering(VkCommandBuffer cb, const VkRenderingInfo* ri) {
    const uint32_t n = ri->colorAttachmentCount;
    t_s.att.resize(n + 2);
    VkRenderingInfo c = *ri;
    for (uint32_t i = 0; i < n; ++i) {
        t_s.att[i] = ri->pColorAttachments[i];
        t_s.att[i].imageLayout = gen(t_s.att[i].imageLayout);
        t_s.att[i].resolveImageLayout = gen(t_s.att[i].resolveImageLayout);
    }
    if (n) c.pColorAttachments = t_s.att.data();
    if (ri->pDepthAttachment) {
        t_s.att[n] = *ri->pDepthAttachment;
        t_s.att[n].imageLayout = gen(t_s.att[n].imageLayout);
        t_s.att[n].resolveImageLayout = gen(t_s.att[n].resolveImageLayout);
        c.pDepthAttachment = &t_s.att[n];
    }
    if (ri->pStencilAttachment) {
        t_s.att[n + 1] = *ri->pStencilAttachment;
        t_s.att[n + 1].imageLayout = gen(t_s.att[n + 1].imageLayout);
        t_s.att[n + 1].resolveImageLayout = gen(t_s.att[n + 1].resolveImageLayout);
        c.pStencilAttachment = &t_s.att[n + 1];
    }
    o_begin_rendering(cb, &c);
}

// The image infos of image-type writes, rewritten; `writes` points at them (the vectors are sized before any pointer is taken).
const VkWriteDescriptorSet* rewrite_writes(uint32_t n, const VkWriteDescriptorSet* w) {
    size_t images = 0;
    for (uint32_t i = 0; i < n; ++i)
        if (image_descriptor(w[i].descriptorType) && w[i].pImageInfo) images += w[i].descriptorCount;
    if (!images) return w;
    t_s.writes.assign(w, w + n);
    t_s.images.resize(images);
    size_t k = 0;
    for (VkWriteDescriptorSet& x : t_s.writes) {
        if (!image_descriptor(x.descriptorType) || !x.pImageInfo) continue;
        VkDescriptorImageInfo* dst = t_s.images.data() + k;
        for (uint32_t j = 0; j < x.descriptorCount; ++j) {
            dst[j] = x.pImageInfo[j];
            dst[j].imageLayout = gen(dst[j].imageLayout);
        }
        x.pImageInfo = dst;
        k += x.descriptorCount;
    }
    return t_s.writes.data();
}

VKAPI_ATTR void VKAPI_CALL w_update_sets(VkDevice d, uint32_t nw, const VkWriteDescriptorSet* w, uint32_t nc, const VkCopyDescriptorSet* c) {
    o_update_sets(d, nw, rewrite_writes(nw, w), nc, c);
}

VKAPI_ATTR void VKAPI_CALL w_push_set(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set, uint32_t nw,
                                      const VkWriteDescriptorSet* w) {
    o_push_set(cb, bp, layout, set, nw, rewrite_writes(nw, w));
}

VKAPI_ATTR void VKAPI_CALL w_copy_image(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy* r) {
    o_copy_image(cb, s, gen(sl), d, gen(dl), n, r);
}
VKAPI_ATTR void VKAPI_CALL w_copy_i2b(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkBuffer d, uint32_t n, const VkBufferImageCopy* r) {
    o_copy_i2b(cb, s, gen(sl), d, n, r);
}
VKAPI_ATTR void VKAPI_CALL w_copy_i2b2(VkCommandBuffer cb, const VkCopyImageToBufferInfo2* i) {
    VkCopyImageToBufferInfo2 c = *i;
    c.srcImageLayout = gen(c.srcImageLayout);
    o_copy_i2b2(cb, &c);
}
VKAPI_ATTR void VKAPI_CALL w_copy_b2i(VkCommandBuffer cb, VkBuffer s, VkImage d, VkImageLayout dl, uint32_t n, const VkBufferImageCopy* r) {
    o_copy_b2i(cb, s, d, gen(dl), n, r);
}
VKAPI_ATTR void VKAPI_CALL w_copy_b2i2(VkCommandBuffer cb, const VkCopyBufferToImageInfo2* i) {
    VkCopyBufferToImageInfo2 c = *i;
    c.dstImageLayout = gen(c.dstImageLayout);
    o_copy_b2i2(cb, &c);
}
VKAPI_ATTR void VKAPI_CALL w_blit(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit* r,
                                  VkFilter f) {
    o_blit(cb, s, gen(sl), d, gen(dl), n, r, f);
}
VKAPI_ATTR void VKAPI_CALL w_clear_color(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue* v, uint32_t n,
                                         const VkImageSubresourceRange* r) {
    o_clear_color(cb, i, gen(l), v, n, r);
}
VKAPI_ATTR void VKAPI_CALL w_clear_ds(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearDepthStencilValue* v, uint32_t n,
                                      const VkImageSubresourceRange* r) {
    o_clear_ds(cb, i, gen(l), v, n, r);
}
VKAPI_ATTR void VKAPI_CALL w_resolve(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageResolve* r) {
    o_resolve(cb, s, gen(sl), d, gen(dl), n, r);
}

template <typename F>
bool swap(F& slot, F& orig, F wrapper) {
    if (!slot) return false;   // not loaded (an extension the device lacks): the call is never made, nothing to rewrite
    if (slot == wrapper) return true;
    orig = slot;
    slot = wrapper;
    return true;
}

}  // namespace

void unified_layouts_install() {   // after every volkLoadDevice (a wrapper already in place is kept, a new driver pointer is wrapped)
    const bool again = g_on.exchange(true);
    int n = 0;
    n += swap(vkCmdPipelineBarrier, o_barrier, &w_barrier);
    n += swap(vkCmdPipelineBarrier2, o_barrier2, &w_barrier2);
    n += swap(vkCmdBeginRendering, o_begin_rendering, &w_begin_rendering);
    n += swap(vkUpdateDescriptorSets, o_update_sets, &w_update_sets);
    n += swap(vkCmdPushDescriptorSetKHR, o_push_set, &w_push_set);
    n += swap(vkCmdCopyImage, o_copy_image, &w_copy_image);
    n += swap(vkCmdCopyImageToBuffer, o_copy_i2b, &w_copy_i2b);
    n += swap(vkCmdCopyImageToBuffer2, o_copy_i2b2, &w_copy_i2b2);
    n += swap(vkCmdCopyBufferToImage, o_copy_b2i, &w_copy_b2i);
    n += swap(vkCmdCopyBufferToImage2, o_copy_b2i2, &w_copy_b2i2);
    n += swap(vkCmdBlitImage, o_blit, &w_blit);
    n += swap(vkCmdClearColorImage, o_clear_color, &w_clear_color);
    n += swap(vkCmdClearDepthStencilImage, o_clear_ds, &w_clear_ds);
    n += swap(vkCmdResolveImage, o_resolve, &w_resolve);
    if (!again) NXLOG_I(kTag, "[gfx] unified_layouts: every image in the general layout (VK_KHR_unified_image_layouts; %d calls rewritten)", n);
}

bool unified_layouts_on() { return g_on.load(std::memory_order_relaxed); }

}  // namespace nxrt::gfx

// SPDX-License-Identifier: GPL-3.0-or-later
// [gfx] unified_layouts: every image in VK_IMAGE_LAYOUT_GENERAL (VK_KHR_unified_image_layouts); see vk_unified.cpp.
#pragma once
#include <volk.h>

namespace nxrt::gfx {
// Wraps volk's device-level pointers that take image layouts so they name GENERAL. Call after volkLoadDevice, only on a device created
// with the unifiedImageLayouts feature.
void unified_layouts_install();
bool unified_layouts_on();
}  // namespace nxrt::gfx

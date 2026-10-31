/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#define CINTERFACE
#include <d3d12.h>
#include <cstddef>
#include <cmd_slots.hpp>

namespace addon = renodx::addons::dlss5;
static_assert(offsetof(ID3D12GraphicsCommandListVtbl, CopyResource) / sizeof(void*) == addon::kCmdSlotCopyResource);
static_assert(offsetof(ID3D12GraphicsCommandListVtbl, CopyTextureRegion) / sizeof(void*) == addon::kCmdSlotCopyTextureRegion);
int main() { return 0; }

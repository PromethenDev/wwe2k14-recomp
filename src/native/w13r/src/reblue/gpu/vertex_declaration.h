/**
 * @file    gpu/vertex_declaration.h
 * @brief   Guest D3DVERTEXELEMENT9 array -> GuestVertexDeclaration (host input
 *          layout), plus the element hash registry for PSO replay.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/types.h>

namespace bd::gpu {

struct GuestVertexElement;
struct GuestVertexDeclaration;

// Walks a guest D3DVERTEXELEMENT9 array (terminated by stream=0xFF,
// type=D3DDECLTYPE_UNUSED) into a GuestVertexDeclaration.
GuestVertexDeclaration *CreateVertexDeclaration(GuestVertexElement *elements);

// Resolve a serialized PSO's decl back to its live host object by element hash
// (== GuestVertexDeclaration::hash). nullptr if not yet created.
GuestVertexDeclaration *FindVertexDeclarationByHash(u64 hash);

} // namespace bd::gpu

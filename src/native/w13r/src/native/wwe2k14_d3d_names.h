// WWE 2K14 addresses for the XDK D3D functions the w13r renderer (WWE '13 v1.6, decision 0008) hooks by name.
// Each 2K14 function was matched to its WWE '13 counterpart (portmap.tsv) and checked 2026-11-22: identical PowerPC
// instruction sequences in both games' generated code (22/22), identical device-struct offsets (only global-data and
// import call addresses differ). The WWE 2K15 notes below are from the WWE '13 table this was made from.
// WWE '13 addresses for the XDK D3D functions that re:Blue's renderer hooks by name.
//
// WWE '13's D3D library is WWE 2K15's (same newer XDK, private device block +0x18 from WWE '12): the 23
// functions WWE 2K15 PC hooks were matched to WWE '13 by the structure of their SDK 0.10 generated code
// (port/tools/match_functions.py against WWE 2K15 PC's generated code, 2026-11-17); 15 are identical
// including constants ("body"), 8 apart from address constants ("shape"). The first 16 agree with the
// independent match against WWE '12 PC. Same hook set and switches as WWE 2K15 PC (the renderer code here
// comes from it). Force-included into the native renderer sources (cmake/native_renderer.cmake).

#pragma once

#define WWE13_NATIVE 1

// Not identified in WWE 2K15 / WWE '13 yet: re:Blue compiles these hooks out.
#define SVR_NO_DRAW_VERTICES 1   // D3DDevice_DrawVertices
#define WWE12_NO_DESC_HOOKS 1    // the resource description getters

#define D3DDevice_Present                           sub_82CB7818  // WWE 2K15 sub_82BAE688 (shape)
#define D3DDevice_Swap                              sub_82CB70B0  // WWE 2K15 sub_82BADF20 (shape)
#define D3DDevice_SynchronizeToPresentationInterval sub_82CB6BF0  // WWE 2K15 sub_82BADA60 (shape)
#define D3DDevice_Resolve                           sub_82CB3478  // WWE 2K15 sub_82BAA2E8 (body)
#define D3DDevice_Clear                             sub_82CB8BA8  // WWE 2K15 sub_82BAFA18 (shape)
#define D3DDevice_SetRenderTarget                   sub_82CAF5E8  // WWE 2K15 sub_82BA6458 (body)
#define D3DDevice_SetDepthStencilSurface            sub_82CAF978  // WWE 2K15 sub_82BA67E8 (body)
#define D3DDevice_SetScissorRect                    sub_82CAEB70  // WWE 2K15 sub_82BA59E0 (body)
#define D3DDevice_BeginVertices                     sub_82CB4E10  // WWE 2K15 sub_82BABC80 (body)
#define D3DDevice_EndVertices                       sub_82CB4E00  // WWE 2K15 sub_82BABC70 (body)
#define D3DDevice_DrawIndexedVertices               sub_82CB52D0  // WWE 2K15 sub_82BAC140 (shape)
#define D3DVertexBuffer_Unlock                      sub_82CB0740  // WWE 2K15 sub_82BA75B0 (body)
#define D3DIndexBuffer_Unlock                       sub_82CAA398  // WWE 2K15 sub_82BA1208 (body)
#define D3DDevice_CreateVertexDeclaration           sub_82CB47C8  // WWE 2K15 sub_82BAB638 (body)
#define D3DDevice_CreateVertexShader                sub_82CB4CC0  // WWE 2K15 sub_82BABB30 (shape)
#define D3DDevice_CreatePixelShader                 sub_82CB4AD8  // WWE 2K15 sub_82BAB948 (shape)
#define D3DDevice_SetTexture                        sub_82CAC430  // WWE 2K15 sub_82BA32A0 (body)
#define D3DDevice_SetVertexShader                   sub_82CB44A8  // WWE 2K15 sub_82BAB318 (body)
#define D3DDevice_SetPixelShader                    sub_82CB42A0  // WWE 2K15 sub_82BAB110 (body)
#define D3DDevice_SetVertexDeclaration              sub_82CB46C0  // WWE 2K15 sub_82BAB530 (body)
#define D3DDevice_SetStreamSource                   sub_82CAECB8  // WWE 2K15 sub_82BA5B28 (body)
#define D3DDevice_SetIndices                        sub_82CAEE60  // WWE 2K15 sub_82BA5CD0 (body)

// Original bodies (__imp__ = the recompiled function, for hooks that call through).
#define __imp__D3DDevice_Present                   __imp__sub_82CB7818
#define __imp__D3DDevice_Swap                      __imp__sub_82CB70B0
#define __imp__D3DDevice_SynchronizeToPresentationInterval__imp__sub_82CB6BF0
#define __imp__D3DDevice_Resolve                   __imp__sub_82CB3478
#define __imp__D3DDevice_Clear                     __imp__sub_82CB8BA8
#define __imp__D3DDevice_SetRenderTarget           __imp__sub_82CAF5E8
#define __imp__D3DDevice_SetDepthStencilSurface    __imp__sub_82CAF978
#define __imp__D3DDevice_SetScissorRect            __imp__sub_82CAEB70
#define __imp__D3DDevice_BeginVertices             __imp__sub_82CB4E10
#define __imp__D3DDevice_EndVertices               __imp__sub_82CB4E00
#define __imp__D3DDevice_DrawIndexedVertices       __imp__sub_82CB52D0
#define __imp__D3DVertexBuffer_Unlock              __imp__sub_82CB0740
#define __imp__D3DIndexBuffer_Unlock               __imp__sub_82CAA398
#define __imp__D3DDevice_CreateVertexDeclaration   __imp__sub_82CB47C8
#define __imp__D3DDevice_CreateVertexShader        __imp__sub_82CB4CC0
#define __imp__D3DDevice_CreatePixelShader         __imp__sub_82CB4AD8
#define __imp__D3DDevice_SetTexture                __imp__sub_82CAC430
#define __imp__D3DDevice_SetVertexShader           __imp__sub_82CB44A8
#define __imp__D3DDevice_SetPixelShader            __imp__sub_82CB42A0
#define __imp__D3DDevice_SetVertexDeclaration      __imp__sub_82CB46C0
#define __imp__D3DDevice_SetStreamSource           __imp__sub_82CAECB8
#define __imp__D3DDevice_SetIndices                __imp__sub_82CAEE60

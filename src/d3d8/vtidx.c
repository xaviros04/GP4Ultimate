/* Vtable slot numbers of the D3D8 methods we hook, taken from the C declarations of the
 * interfaces (CINTERFACE) so they are computed by the compiler, never counted by hand. */
#define CINTERFACE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <d3d8.h>

#define D3D_IDX(m) (int)(offsetof(IDirect3D8Vtbl, m) / sizeof(void*))
#define DEV_IDX(m) (int)(offsetof(IDirect3DDevice8Vtbl, m) / sizeof(void*))

const int gp4_vt_CreateDevice = D3D_IDX(CreateDevice);
const int gp4_vt_GetAdapterModeCount = D3D_IDX(GetAdapterModeCount);
const int gp4_vt_EnumAdapterModes = D3D_IDX(EnumAdapterModes);

const int gp4_vt_Reset = DEV_IDX(Reset);
const int gp4_vt_Present = DEV_IDX(Present);
const int gp4_vt_SetRenderTarget = DEV_IDX(SetRenderTarget);
const int gp4_vt_BeginScene = DEV_IDX(BeginScene);
const int gp4_vt_EndScene = DEV_IDX(EndScene);
const int gp4_vt_Clear = DEV_IDX(Clear);
const int gp4_vt_SetTransform = DEV_IDX(SetTransform);
const int gp4_vt_MultiplyTransform = DEV_IDX(MultiplyTransform);
const int gp4_vt_SetViewport = DEV_IDX(SetViewport);
const int gp4_vt_SetRenderState = DEV_IDX(SetRenderState);
const int gp4_vt_SetTexture = DEV_IDX(SetTexture);
const int gp4_vt_DrawPrimitive = DEV_IDX(DrawPrimitive);
const int gp4_vt_DrawIndexedPrimitive = DEV_IDX(DrawIndexedPrimitive);
const int gp4_vt_DrawPrimitiveUP = DEV_IDX(DrawPrimitiveUP);
const int gp4_vt_DrawIndexedPrimitiveUP = DEV_IDX(DrawIndexedPrimitiveUP);
const int gp4_vt_SetVertexShader = DEV_IDX(SetVertexShader);
const int gp4_vt_SetVertexShaderConstant = DEV_IDX(SetVertexShaderConstant);
const int gp4_vt_CreateVertexShader = DEV_IDX(CreateVertexShader);

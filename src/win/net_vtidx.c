/* Vtable slots of the IDirectPlay4A methods the network log hooks, computed from the C
 * declaration of the interface (CINTERFACE), never counted by hand. */
#define CINTERFACE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <dplay.h>

#define DP_IDX(m) (int)(offsetof(IDirectPlay4Vtbl, m) / sizeof(void*))

const int gp4_dp_QueryInterface = DP_IDX(QueryInterface);
const int gp4_dp_Close = DP_IDX(Close);
const int gp4_dp_CreatePlayer = DP_IDX(CreatePlayer);
const int gp4_dp_DestroyPlayer = DP_IDX(DestroyPlayer);
const int gp4_dp_EnumSessions = DP_IDX(EnumSessions);
const int gp4_dp_GetMessageCount = DP_IDX(GetMessageCount);
const int gp4_dp_Open = DP_IDX(Open);
const int gp4_dp_Receive = DP_IDX(Receive);
const int gp4_dp_Send = DP_IDX(Send);
const int gp4_dp_SetSessionDesc = DP_IDX(SetSessionDesc);
const int gp4_dp_EnumConnections = DP_IDX(EnumConnections);
const int gp4_dp_InitializeConnection = DP_IDX(InitializeConnection);
const int gp4_dp_SendEx = DP_IDX(SendEx);

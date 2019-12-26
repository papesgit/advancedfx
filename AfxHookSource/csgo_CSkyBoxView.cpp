#include "stdafx.h"

#include "csgo_CSkyBoxView.h"

#include "addresses.h"
#include "AfxStreams.h"

#include <shared/AfxDetours.h>

typedef void (__stdcall *csgo_CSkyBoxView_Draw_t)(DWORD *this_ptr);

csgo_CSkyBoxView_Draw_t detoured_csgo_CSkyBoxView_Draw;

void __stdcall touring_csgo_CSkyBoxView_Draw(DWORD *this_ptr)
{
	g_AfxStreams.OnDrawingSkyBoxViewBegin();

	detoured_csgo_CSkyBoxView_Draw(this_ptr);
	
	g_AfxStreams.OnDrawingSkyBoxViewEnd();
}

bool csgo_CSkyBoxView_Draw_Install(void)
{
	static bool firstResult = false;
	static bool firstRun = true;
	if(!firstRun) return firstResult;
	firstRun = false;

	if(AFXADDR_GET(csgo_CSkyboxView_Draw))
	{
		detoured_csgo_CSkyBoxView_Draw = (csgo_CSkyBoxView_Draw_t)DetourClassFunc((BYTE *)AFXADDR_GET(csgo_CSkyboxView_Draw), (BYTE *)touring_csgo_CSkyBoxView_Draw, (int)AFXADDR_GET(csgo_CSkyboxView_Draw_DSZ));

		firstResult = true;
	}

	return firstResult;
}

float csgo_CSkyBoxView_GetScale(void)
{
	if(AFXADDR_GET(csgo_pLocalPlayer)
		&& AFXADDR_GET(csgo_C_BasePlayer_OFS_m_skybox3d_scale) != (AfxAddr)-1
		&& 0 != *(unsigned char **)AFXADDR_GET(csgo_pLocalPlayer))
	{
		int skyBoxScale = *(int *)(*(unsigned char **)AFXADDR_GET(csgo_pLocalPlayer) +AFXADDR_GET(csgo_C_BasePlayer_OFS_m_skybox3d_scale));

		return skyBoxScale ? 1.0f / skyBoxScale : 0.0f;
	}

	return 1.0f;
}
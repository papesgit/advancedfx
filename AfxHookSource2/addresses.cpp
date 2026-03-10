#include "stdafx.h"

#include "addresses.h"
#include "Globals.h"

#include "../shared/binutils.h"

using namespace Afx::BinUtils;

static AfxAddr DecodeRipRel32Target(AfxAddr insnAddress, size_t rel32Offset, size_t insnSize)
{
      const int32_t rel = *(const int32_t *)(insnAddress + rel32Offset);
      return insnAddress + insnSize + rel;
}

AFXADDR_DEF(cs2_engine_HostStateRequest_Start)
AFXADDR_DEF(cs2_engine_CRenderService_OnClientOutput);

AFXADDR_DEF(cs2_SceneSystem_WaitForRenderingToComplete_vtable_idx);
AFXADDR_DEF(cs2_SceneSystem_FrameUpdate_vtable_idx);

AFXADDR_DEF(cs2_deathmsg_lifetime_offset)
AFXADDR_DEF(cs2_deathmsg_lifetimemod_offset)

AFXADDR_DEF(cs2_client_CCSGOVScriptGameSystem_GetMode)
AFXADDR_DEF(cs2_client_ClientScriptVM)
AFXADDR_DEF(cs2_ScriptVM_CompileString_vtable_offset)
AFXADDR_DEF(cs2_ScriptVM_RunScript_vtable_offset)
AFXADDR_DEF(cs2_ScriptVM_FreeScript_vtable_offset)
AFXADDR_DEF(cs2_client_TraceShape)
AFXADDR_DEF(cs2_client_TraceCollideableShape)
AFXADDR_DEF(cs2_client_TraceContextPtr)
AFXADDR_DEF(cs2_client_TraceFilterVft)
AFXADDR_DEF(cs2_client_InitTraceFilter)
AFXADDR_DEF(cs2_client_BuildTraceHullShape)
AFXADDR_DEF(cs2_client_TraceCollideableFilterPtr)

void Addresses_InitEngine2Dll(AfxAddr engine2Dll)
{
	MemRange textRange = MemRange(0, 0);
	{
		ImageSectionsReader imageSectionsReader((HMODULE)engine2Dll);
		if (!imageSectionsReader.Eof())
		{
			textRange = imageSectionsReader.GetMemRange();
		}
		else ErrorBox(MkErrStr(__FILE__, __LINE__));
	}

    /*  cs2_engine_HostStateRequest_Start
        The function in question references this string: "HostStateRequest::Start(HSR_QUIT)\n"
                                FUN_180217fc0                                   XREF[4]:     FUN_18021a010:18021a18e(c), 
                                                                                            1805ba12c(*), 1805ba13c(*), 
                                                                                            18091557c(*)  
        180217fc0 40 53           PUSH       RBX
        180217fc2 48 83 ec 40     SUB        RSP,0x40
        180217fc6 8b 01           MOV        EAX,dword ptr [RCX]
        180217fc8 48 8b d9        MOV        RBX,RCX
        180217fcb c6 41 18 01     MOV        byte ptr [RCX + 0x18],0x1
        180217fcf 83 f8 02        CMP        EAX,0x2
        180217fd2 74 07           JZ         LAB_180217fdb
        180217fd4 83 f8 04        CMP        EAX,0x4
        180217fd7 75 21           JNZ        LAB_180217ffa
        180217fd9 eb 0d           JMP        LAB_180217fe8
        [....]
        180218037 84 c0           TEST       AL,AL
        180218039 74 18           JZ         LAB_180218053
        18021803b 8b 0d f7        MOV        ECX,dword ptr [DAT_1808ec238]
                    41 6d 00
        180218041 4c 8d 05        LEA        R8,[s_HostStateRequest::Start(HSR_QUIT_1805648   = "HostStateRequest::Start(HSR_Q
                    d8 c7 34 00
        180218048 ba 02 00        MOV        EDX,0x2
                    00 00
        18021804d ff 15 fd        CALL       qword ptr [->TIER0.DLL::LoggingSystem_Log]       = 005e034a
                    17 25 00
        [....]
    */
    {
		MemRange result = FindPatternString(textRange, "40 53 48 83 ec 40 8b 01 48 8b d9 c6 41 18 01 83 f8 02 74 07 83 f8 04 75 21 eb 0d");
																	  
		if (!result.IsEmpty()) {
            AFXADDR_SET(cs2_engine_HostStateRequest_Start, result.Start);
		}
		else
			ErrorBox(MkErrStr(__FILE__, __LINE__));
    }

    /*  cs2_engine_CRenderService_OnClientOutput

        To find search for references to the strings given bellow.

                 24 18
       1801e5715 55              PUSH       RBP
       1801e5716 56              PUSH       RSI
       1801e5717 57              PUSH       RDI
       1801e5718 41 54           PUSH       R12
       1801e571a 41 56           PUSH       R14
       1801e571c 48 83 ec 70     SUB        RSP,0x70
       1801e5720 48 8d 05        LEA        RAX,[s_C:\buildworker\csgo_rel_win64\bu_18055a   = "C:\\buildworker\\csgo_rel_win
                 49 53 37 00
       1801e5727 48 c7 44        MOV        qword ptr [RSP + local_68[8]],0x19d
                 24 38 9d 
                 01 00 00
       1801e5730 48 89 44        MOV        qword ptr [RSP + local_68[0]],RAX=>s_C:\buildw   = "C:\\buildworker\\csgo_rel_win
                 24 30
       1801e5735 4c 8d 44        LEA        R8=>local_48,[RSP + 0x50]
                 24 50
       1801e573a 0f 10 44        MOVUPS     XMM0,xmmword ptr [RSP + local_68[0]]
                 24 30
       1801e573f 48 8d 05        LEA        RAX,[s_OnClientOutput_18055a7c8]                 = "OnClientOutput"
                 82 50 37 00
       1801e5746 4c 8b f2        MOV        R14,RDX
       1801e5749 48 89 44        MOV        qword ptr [RSP + local_58],RAX=>s_OnClientOutp   = "OnClientOutput"
                 24 40
       1801e574e 48 8d 15        LEA        RDX,[PTR_s_Client_Rendering_1805f1050]           = 18055aab8
                 fb b8 40 00
       1801e5755 f2 0f 10        MOVSD      XMM1,qword ptr [RSP + local_58]
                 4c 24 40
       1801e575b 48 8b f1        MOV        RSI,RCX
       1801e575e 48 8d 0d        LEA        RCX,[s_RenderService::OnClientOutput_18055aa48]  = "RenderService::OnClientOutput"
                 e3 52 37 00
       1801e5765 f2 0f 11        MOVSD      qword ptr [RSP + local_38],XMM1=>s_OnClientOut   = "OnClientOutput"
                 4c 24 60
       1801e576b 33 ff           XOR        EDI,EDI
       1801e576d 0f 29 44        MOVAPS     xmmword ptr [RSP + local_48[0]],XMM0=>DAT_1805
                 24 50
       1801e5772 ff 15 f8        CALL       qword ptr [->TIER0.DLL::VProfScopeHelper<0,0>:   = 005e0b32
                 3e 28 00
       1801e5778 48 8b 96        MOV        RDX,qword ptr [RSI + 0x1c0]
                 c0 01 00 00
    */
	{
		MemRange result = FindPatternString(textRange, "48 89 5C 24 18 55 56 57 41 54 41 56 48 83 EC 70 48 8D 05 ?? ?? ?? ??");
																	  
		if (!result.IsEmpty()) {
            AFXADDR_SET(cs2_engine_CRenderService_OnClientOutput, result.Start);
		}
		else
			ErrorBox(MkErrStr(__FILE__, __LINE__));
	}
}

void Addresses_InitSceneSystemDll(AfxAddr sceneSystemDll) {

    /*cs2_SceneSystem_WaitForRenderingToComplete_vtable_idx 
    
       To find the right function search for references to strings:
       - "WaitForRenderingToComplete".
    */
    AFXADDR_SET(cs2_SceneSystem_WaitForRenderingToComplete_vtable_idx, 26);

    /*cs2_SceneSystem_WaitForRenderingToComplete_vtable_idx 
    
       To find the right function search for references to strings:
       - "FrameUpdate"
       - "CSceneSystem::FrameUpdate"
       - "Invalid width/height for ScratchTarget, Size=%i, Width=%i/Height=%i"
    */
    AFXADDR_SET(cs2_SceneSystem_FrameUpdate_vtable_idx, 73);
}

void Addresses_InitClientDll(AfxAddr clientDll) {
	MemRange textRange = MemRange(0, 0);
      MemRange dataRange = MemRange(0, 0);
      MemRange moduleRange = MemRange(0, 0);
	{
            PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)clientDll;
            PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(clientDll + dos->e_lfanew);
            moduleRange = MemRange::FromSize(clientDll, nt->OptionalHeader.SizeOfImage);

		ImageSectionsReader imageSectionsReader((HMODULE)clientDll);
		if (!imageSectionsReader.Eof())
		{
			textRange = imageSectionsReader.GetMemRange();
                  imageSectionsReader.Next();
                  if (!imageSectionsReader.Eof()) {
                        dataRange = imageSectionsReader.GetMemRange();
                  }
      		else ErrorBox(MkErrStr(__FILE__, __LINE__));
		}
		else ErrorBox(MkErrStr(__FILE__, __LINE__));
	}

	// in the end of g_Original_handlePlayerDeath function
	{
		MemRange result = FindPatternString(textRange, "0F B7 15 ?? ?? ?? ?? F3 41 0F 10 54 24 ??");
		if (!result.IsEmpty()) {
            AFXADDR_SET(cs2_deathmsg_lifetime_offset, *(uint8_t*)(result.Start + 13));
		}
		else
			ErrorBox(MkErrStr(__FILE__, __LINE__));
	}
	{
		MemRange result = FindPatternString(textRange, "44 38 7C 24 ?? 74 ?? F3 41 0F 10 74 24 ??");
		if (!result.IsEmpty()) {
            AFXADDR_SET(cs2_deathmsg_lifetimemod_offset, *(uint8_t*)(result.Start + 13));
		}
		else
			ErrorBox(MkErrStr(__FILE__, __LINE__));
	}

      // cs2_client_CCSGOVScriptGameSystem_GetMode
      //
      // For more info see DevWarning("VM Did not start!\n");
      {
            if(size_t * vtable = (size_t*)Afx::BinUtils::FindClassVtable((HMODULE)clientDll,".?AVCCSGOVScriptGameSystem@@", 0, 0x0)) {
                  AFXADDR_SET(cs2_client_CCSGOVScriptGameSystem_GetMode, vtable[63]);
            }
            else ErrorBox(MkErrStr(__FILE__, __LINE__));
      }

      // cs2_client_ClientScriptVM
      // cs2_ScriptVM_CompileString_vtable_offset
      // see LoggingSystem_Log(DAT_18251b6dc,3,"Error running script named %s\n",param_2);
      {
            MemRange result = FindCString(dataRange, "VM Did not start!\n");
            if (!result.IsEmpty())
            {
                  size_t strAddr = result.Start;
                  MemRange result2 = FindAddrInt32OffsetRefInContext(textRange, strAddr, (int32_t)sizeof(int32_t), "48 8d 0d", nullptr);
                  if(!result2.IsEmpty()) {
                        size_t addr = result2.Start - 0xF;
                        unsigned char pattern[3] = { 0x48, 0x89, 0x05 };
                        size_t patternSize = sizeof(pattern) / sizeof(pattern[0]);
                        MemRange patternRange(addr, addr + patternSize);
                        MemRange result3 = FindBytes(textRange.And(patternRange), (char *)pattern, patternSize);
                        if (result3.Start != patternRange.Start || result3.End != patternRange.End)
                        {
                              addr = 0;
                              ErrorBox(MkErrStr(__FILE__, __LINE__));
                        } else {
                              addr = addr+7+*(int32_t*)(addr+3);
                              AFXADDR_SET(cs2_client_ClientScriptVM, addr);
                              AFXADDR_SET(cs2_ScriptVM_CompileString_vtable_offset, 15);
                              AFXADDR_SET(cs2_ScriptVM_RunScript_vtable_offset, 13);
                              AFXADDR_SET(cs2_ScriptVM_FreeScript_vtable_offset, 16);
                        }                 
                  }
                  else ErrorBox(MkErrStr(__FILE__, __LINE__));
            }
            else ErrorBox(MkErrStr(__FILE__, __LINE__));
      }

      // - TraceShape and TraceCollideableShape are resolved from their own function signatures below.
      // - Trace context pointer is resolved from a native TraceShape callsite (most common callsite) block:
      //     MOV RCX,[PTR_DAT_...]
      //     ...
      //     CALL TraceShape
      // - Trace filter globals are resolved from native initializer:
      //     LEA RAX,[PTR_FUN_...]
      //     MOV [PTR_PTR_...],RAX
      //     RET
      //   This initializer was located by following arg#6 filter pointer references used by
      //   Script_TraceCollideable's TraceCollideableShape call path.

      // cs2_client_TraceShape
      // Search for "Physics/TraceShape (Client)" or Script wrappers ("Didnt supply startpos...")
      // - Starts with stack setup + __chkstk call and then:
      //   MOV R10D,dword ptr [_tls_index]
      {
            MemRange result = FindPatternString(
                  textRange,
                  "48 89 5C 24 20 48 89 4C 24 08 55 57 41 54 41 55 41 56 48 8D AC 24 10 E0 FF FF B8 F0 20 00 00 E8 ?? ?? ?? ?? 48 2B E0 44 8B 15 ?? ?? ?? ??"
            );

            if (!result.IsEmpty()) {
                  AFXADDR_SET(cs2_client_TraceShape, result.Start);
            }
            else ErrorBox(MkErrStr(__FILE__, __LINE__));
      }

      // cs2_client_TraceCollideableShape
      // - Found easiest from Script wrapper callsite ("Didnt supply ... Script TraceCollideable")
      // - Starts with:
      //   MOV RAX,RSP
      //   MOV [RAX+18],RBX
      //   ...
      //   TEST RBX,RBX
      //   JZ ...
      {
            MemRange result = FindPatternString(
                  textRange,
                  "48 8B C4 48 89 58 18 55 56 57 41 54 41 56 48 8D 68 B9 48 81 EC A0 00 00 00 48 8B 5D 6F 49 8B F9 49 8B F0 4C 8B F2 4C 8B E1 48 85 DB 0F 84 ?? ?? ?? ?? 4C 89 68 08 48 8B CB 4C 89 78 10 48 8B 03 FF 90 08 02 00 00"
            );

            if (!result.IsEmpty()) {
                  AFXADDR_SET(cs2_client_TraceCollideableShape, result.Start);
            }
            else ErrorBox(MkErrStr(__FILE__, __LINE__));
      }

      // cs2_client_BuildTraceHullShape
      // - Starts with min/max equality checks then writes shape type byte at +0x28.
      // - Also used in native TraceShape callsite right before:
      //     MOV RCX,[PTR_DAT_...] ... CALL TraceShape.
      {
            MemRange result = FindPatternString(
                  textRange,
                  "F3 0F 10 42 0C 0F 2E 02 7A 36 75 34 F3 0F 10 42 10 0F 2E 42 04 7A 29 75 27 F3 0F 10 42 14 0F 2E 42 08 7A 1C 75 1A C6 41 28 00 F2 0F 10 02 F2 0F 11 01 8B 42 08 89 41 08 C7 41 0C 00 00 00 00 C3 C6 41 28 02 0F 10 02 0F 11 01"
            );

            if (!result.IsEmpty()) {
                  AFXADDR_SET(cs2_client_BuildTraceHullShape, result.Start);
            }
            else ErrorBox(MkErrStr(__FILE__, __LINE__));
      }

      // cs2_client_InitTraceFilter
      // Native filter initializer/helper used by many TraceShape callsites.
      // Common callsite form:
      //   FUN_180319690(local_618, uVar5, 4, 3, 0x0f);
      // Then local_618 is passed as TraceShape arg5 or TraceShapeWrapper arg4 (filter):
      //   TraceShape(PTR_DAT_18203a928, local_5c8, &local_728, &local_734, local_618, local_6e8)
      
      //  180319690 48 89 5c        MOV        qword ptr [RSP + local_res8],RBX
      //            24 08
      //  180319695 48 89 74        MOV        qword ptr [RSP + local_res10],RSI
      //            24 10
      //  18031969a 57              PUSH       RDI
      //  18031969b 48 83 ec 20     SUB        RSP,0x20
      //  18031969f 0f b6 41 39     MOVZX      EAX,byte ptr [RCX + 0x39]
      //  1803196a3 33 ff           XOR        EDI,EDI
      //  1803196a5 24 c9           AND        AL,0xc9
      //  1803196a7 c7 41 34        MOV        dword ptr [RCX + 0x34],0xf00ffff
      //            ff ff 00 0f
      //  1803196ae 0c 49           OR         AL,0x49
      //  1803196b0 c6 41 38 03     MOV        byte ptr [RCX + 0x38],0x3
      //  1803196b4 88 41 39        MOV        byte ptr [RCX + 0x39],AL
      //  1803196b7 48 8b d9        MOV        RBX,RCX
      //  1803196ba 48 8d 05        LEA        RAX,[PTR_FUN_181927fa8]                          = 1801e6ad0
      //            e7 e8 60 01
      {
            MemRange result = FindPatternString(
                  textRange,
                  "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 0F B6 41 39 33 FF 24 C9 C7 41 34 FF FF 00 0F 0C 49 C6 41 38 03 88 41 39 48 8B D9 48 8D 05 ?? ?? ?? ?? 48 89 79 10 48 89 01 48 8B F2 0F B6 44 24 50"
            );

            if (!result.IsEmpty()) {
                  AFXADDR_SET(cs2_client_InitTraceFilter, result.Start);
            }
            else ErrorBox(MkErrStr(__FILE__, __LINE__));
      }

      // cs2_client_TraceContextPtr
      // From native TraceShape callsite:
      //   MOV RCX,[PTR_DAT_...]
      //   ...
      //   CALL TraceShape
      {
            MemRange result = FindPatternString(
                  textRange,
                  "48 8B 0D ?? ?? ?? ?? 4C 8D 4C 24 30 4C 8D 44 24 40 4C 89 6C 24 28 48 8D 54 24 70 48 89 74 24 20 E8 ?? ?? ?? ??"
            );

            if (!result.IsEmpty()) {
                  const AfxAddr movContextInsn = result.Start + 0; // 48 8B 0D rel32
                  const AfxAddr callInsn = result.Start + 32;      // E8 rel32

                  const AfxAddr traceContextPtr = DecodeRipRel32Target(movContextInsn, 3, 7);
                  const AfxAddr callTarget = DecodeRipRel32Target(callInsn, 1, 5);

                  if (callTarget != AFXADDR_GET(cs2_client_TraceShape)
                        || traceContextPtr < moduleRange.Start || traceContextPtr >= moduleRange.End)
                        ErrorBox(MkErrStr(__FILE__, __LINE__));
                  else
                        AFXADDR_SET(cs2_client_TraceContextPtr, traceContextPtr);
            }
            else ErrorBox(MkErrStr(__FILE__, __LINE__));
      }

      // cs2_client_TraceFilterVft
      // cs2_client_TraceCollideableFilterPtr
      // From native initializer:
      //   LEA RAX,[PTR_FUN_...]
      //   MOV [PTR_PTR_...],RAX
      //   RET
      {
            MemRange result = FindPatternString(
                  textRange,
                  "48 8D 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? C3 CC 48 83 EC 28 48 8B 15 ?? ?? ?? ?? 48 83 FA 0F 76 30"
            );

            if (!result.IsEmpty()) {
                  const AfxAddr leaFilterVftInsn = result.Start + 0; // 48 8D 05 rel32
                  const AfxAddr movFilterPtrInsn = result.Start + 7; // 48 89 05 rel32

                  const AfxAddr filterVftAddress = DecodeRipRel32Target(leaFilterVftInsn, 3, 7);
                  const AfxAddr filterPtrAddress = DecodeRipRel32Target(movFilterPtrInsn, 3, 7);

                  if (filterVftAddress < moduleRange.Start || filterVftAddress >= moduleRange.End
                        || filterPtrAddress < moduleRange.Start || filterPtrAddress >= moduleRange.End)
                        ErrorBox(MkErrStr(__FILE__, __LINE__));
                  else {
                        AFXADDR_SET(cs2_client_TraceFilterVft, filterVftAddress);
                        AFXADDR_SET(cs2_client_TraceCollideableFilterPtr, filterPtrAddress);
                  }
            }
            else ErrorBox(MkErrStr(__FILE__, __LINE__));
      }
}

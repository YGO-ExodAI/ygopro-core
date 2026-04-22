/*
 * Copyright (c) 2019, Dylam De La Torre, (DyXel)
 * Copyright (c) 2019-2025, Edoardo Lolletti (edo9300) <edoardo762@gmail.com>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef OCGAPI_H
#define OCGAPI_H
#include "ocgapi_types.h"

#ifdef __cplusplus
#define EXTERN_C extern "C"
#else
#define EXTERN_C
#endif

#if defined(OCGCORE_EXPORT_FUNCTIONS)
#if defined(_WIN32)
#define OCGAPI EXTERN_C __declspec(dllexport)
#else
#define OCGAPI EXTERN_C __attribute__ ((visibility ("default")))
#endif
#else
#define OCGAPI EXTERN_C
#endif

/*** CORE INFORMATION ***/
OCGAPI void OCG_GetVersion(int* major, int* minor);
/* OCGAPI void OCG_GetName(const char** name); Maybe created by git hash? */

/*** DUEL CREATION AND DESTRUCTION ***/
OCGAPI int OCG_CreateDuel(OCG_Duel* out_ocg_duel, const OCG_DuelOptions* options_ptr);
OCGAPI void OCG_DestroyDuel(OCG_Duel ocg_duel);
OCGAPI void OCG_DuelNewCard(OCG_Duel ocg_duel, const OCG_NewCardInfo* info_ptr);
OCGAPI void OCG_StartDuel(OCG_Duel ocg_duel);

/*** DUEL PROCESSING AND QUERYING ***/
OCGAPI int OCG_DuelProcess(OCG_Duel ocg_duel);
OCGAPI void* OCG_DuelGetMessage(OCG_Duel ocg_duel, uint32_t* length);
OCGAPI void OCG_DuelSetResponse(OCG_Duel ocg_duel, const void* buffer, uint32_t length);
OCGAPI int OCG_LoadScript(OCG_Duel ocg_duel, const char* buffer, uint32_t length, const char* name);

OCGAPI uint32_t OCG_DuelQueryCount(OCG_Duel ocg_duel, uint8_t team, uint32_t loc);
OCGAPI void* OCG_DuelQuery(OCG_Duel ocg_duel, uint32_t* length, const OCG_QueryInfo* info_ptr);
OCGAPI void* OCG_DuelQueryLocation(OCG_Duel ocg_duel, uint32_t* length, const OCG_QueryInfo* info_ptr);
OCGAPI void* OCG_DuelQueryField(OCG_Duel ocg_duel, uint32_t* length);

/*** STATE SERIALIZATION — ExodAI Phase P1 Primitive 1 ***
 *
 * See src/docs/phase_p1_primitive_1_plan.md §3.1 for the full design.
 *
 * Save-time invariant: must be called at an MSG boundary
 * (post-OCG_DuelProcess + post-OCG_DuelGetMessage drain, pre-next
 * OCG_DuelSetResponse). Calling mid-Process or from inside a Lua
 * callback returns OCG_SAVE_ERR_NOT_MSG_BOUNDARY without producing
 * a buffer.
 *
 * Returns OCG_SaveStatus (cast as int for ABI). On OCG_SAVE_OK,
 * *buffer is allocated and *size set; caller must free via
 * OCG_FreeSaveBuffer. On any error, *buffer is set to NULL and
 * *size to 0.
 *
 * Chunk 3 status: implementation captures the C++ duel/field/cards/
 * effects/groups/chain/processor/RNG tree. Lua reconstruction (closure
 * args for Type-C scripts) is left empty pending chunk 5.
 */
OCGAPI int OCG_DuelSaveState(OCG_Duel ocg_duel, void** buffer, uint32_t* size);
OCGAPI void OCG_FreeSaveBuffer(void* buffer);

#endif /* OCGAPI_H */

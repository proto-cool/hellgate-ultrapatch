# `.\Source\DxC\dxC_device.cpp`

| VA | name | line | self? |
|---|---|---|---|
| 0x0077c33e | `FUN_0077c33e` | 69 | dxC_MeshListsInit dxC_MoviesInit dxC_TargetBuffersInit e_ModelsInit e_PrimitiveInit e_RegionsInit e_ViewersInit |
| 0x0077b809 | `FUN_0077b809` | 331 |  |
| 0x0077ba49 | `e_DeviceInit` | 485 | dx9_DetectNonD3DCapabilities dxC_InitializeDeviceObjects |
| 0x0077bd0b | `FUN_0077bd0b` | 730 |  |
| 0x0077c686 | `FUN_0077c686` | 1179 | dx9_EffectCreateNonMaterialEffects dx9_PrimitiveDrawRestore dx9_ScreenDrawRestore dxC_QuadVBsCreate dxC_RestoreRenderTargets e_ParticleSystemsRestore |
| 0x0077c832 | `FUN_0077c832` | 1264 | dx9_DefaultStatesInit dx9_RestoreD3DResources dx9_VertexDeclarationsRestore dxC_RestoreStatQueries e_GatherCaps e_ModelBudgetUpdate e_RestoreDefaultTextures e_TextureBudgetUpdate e_UpdateGamma |
| 0x0077d057 | `FUN_0077d057` |  |  |
| 0x0077bc5e | `dxC_DeviceCreate` |  |  |
| 0x0077e4da | `FUN_0077e4da` |  | dx9_DetectNonD3DCapabilities e_DeviceRelease e_GatherCaps e_GatherGeneralCaps |
| 0x0077d0c8 | `FUN_0077d0c8` |  |  |
| 0x0077b939 | `e_DeviceCreateMinimal` |  |  |
| 0x0077bdd7 | `dxC_ClearBackBufferPrimaryZ` |  |  |
| 0x0077be71 | `FUN_0077be71` |  |  |
| 0x0077b642 | `FUN_0077b642` |  |  |
| 0x0077ce4d | `FUN_0077ce4d` |  | dxC_QuadVBsDestroy dxC_ReleaseStatQueries e_EnvironmentReleaseAll |
| 0x0077c2cf | `dxC_GetD3DDisplayMode` |  |  |
| 0x0077cabf | `FUN_0077cabf` |  | dxC_MeshListsDestroy dxC_MoviesDestroy dxC_TargetBuffersDestroy dxC_TargetBuffersRelease e_ModelsDestroy e_PrimitiveDestroy e_RegionsDestroy e_TexturesRemoveAll e_UIFree e_ViewersDestroy |
| 0x007d6fac | `sCreateRT` |  |  |
| 0x0077bb8e | `dxC_GrabSwapChainAndBackbuffer` |  |  |
| 0x0077cf89 | `e_Cleanup` |  | e_ModelDefinitionsCleanup e_ModelDumpList e_ParticleSystemsReleaseDefinitionResources e_TextureDumpList e_TexturesCleanup |

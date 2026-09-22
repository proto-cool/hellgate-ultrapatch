#!/usr/bin/env python3
"""
Uncook the game's .xml.cooked definition files ('CO0k') to readable text.

    hguncook.py <file.xml.cooked> [...]      # indented name: value dump, present fields only
    hguncook.py --defaults <file> [...]      # also show absent fields with their file defaults
    hguncook.py --xml <file> [...]           # Reanimator-style XML instead of the dump
    hguncook.py --check <dir> [...]          # parse every .xml.cooked below dir; report ok/fail

Format (ported from Reanimator-steam, hellgate/XmlCookedFile*.cs -- see ref/):
header 'CO0k', version 8, root definition hash, element count; then a
DEFINITION section -- per element: name hash (u32), type token (u16), and a
type-specific default payload (nested tables recurse here) -- then 'DATA' and
the values: one presence bitfield per table instance, then the present values
in definition order. The file is therefore self-describing except for the
element names, which are 32-bit hashes; the table below maps every name that
Reanimator's hellgate/Xml/*.cs knows about. Unknown hashes print as hex.

Element type tokens (low byte = kind, high byte = array-ness):
  0000 int  0006 int[N]  0007 int[n]   0100 float  0106 float[N]  0107 float[n]
  0200 str  0206 str[N]  0207 str[n]   0308 table  0309 table[N]  030A table[n]
  0500 float3[n]  0600 float4[n]  0903 excel row (string)  0905 excel[N]
  0B01 flag bit of a shared dword  0C02 bit-index flag  0700/0707/0800 not cooked
  0A00 int  0A06 int[N]  0D00 pointer  1007 bytes[n]
"""
import os
import struct
import sys

MAGIC, VERSION, DATA = 0x6B304F43, 8, 0x41544144

# Hash: CRC-32 table (poly 0x04C11DB7, non-reflected) driven as
#   h = table[(h >> 24) ^ byte] ^ (h << 8), starting from 0.
_TAB = []
for _i in range(256):
    _c = _i << 24
    for _ in range(8):
        _c = ((_c << 1) ^ (0x04C11DB7 if _c & 0x80000000 else 0)) & 0xFFFFFFFF
    _TAB.append(_c)


def strhash(s):
    h = 0
    for ch in s.encode("latin1"):
        h = (_TAB[(h >> 24) ^ ch] ^ ((h << 8) & 0xFFFFFFFF)) & 0xFFFFFFFF
    return h


# ---------------------------------------------------------------- names ---
NAMES = """
AI_BEHAVIOR_DEFINITION
AI_BEHAVIOR_DEFINITION_TABLE
AI_BEHAVIOR_FLAG_DONT_STOP
AI_BEHAVIOR_FLAG_FLY
AI_BEHAVIOR_FLAG_ONCE
AI_BEHAVIOR_FLAG_RUN
AI_BEHAVIOR_FLAG_WARP
AI_DEFINITION
ANIMATION_DEFINITION
ANIM_EVENT
APPEARANCE_DEFINITION
AirAbsorptionHF
BLEND_RLE
BLEND_RUN
COLOR_DEFINITION
COLOR_SET_DEFINITION
CONDITION_BIT_CHECK_OWNER
CONDITION_BIT_CHECK_STATE_SOURCE
CONDITION_BIT_CHECK_TARGET
CONDITION_BIT_CHECK_WEAPON
CONDITION_BIT_IS_YOUR_PLAYER
CONDITION_BIT_NOT_DEAD_OR_DYING
CONDITION_BIT_OWNER_IS_YOUR_PLAYER
CONDITION_DEFINITION
DEMO_LEVEL_DEFINITION
DecayHFRatio
DecayLFRatio
DecayTime
Density
Diffusion
ENVIRONMENTDEF_FLAG_DIR1_OPPOSITE_DIR0
ENVIRONMENTDEF_FLAG_FLASHLIGHT_EMISSIVE
ENVIRONMENTDEF_FLAG_HAS_APP_SH_COEFS
ENVIRONMENTDEF_FLAG_HAS_BG_SH_COEFS
ENVIRONMENTDEF_FLAG_SPECULAR_FAVOR_FACING
ENVIRONMENTDEF_FLAG_USE_BLOB_SHADOWS
ENVIRONMENT_DEFINITION
ENV_LIGHT_DEFINITION
EchoDepth
EchoTime
EnvDiffusion
EnvSize
Environment
FMOD_REVERB_FLAGS_DECAYHFLIMIT
FMOD_REVERB_FLAGS_DECAYTIMESCALE
FMOD_REVERB_FLAGS_ECHOTIMESCALE
FMOD_REVERB_FLAGS_MODULATIONTIMESCALE
FMOD_REVERB_FLAGS_REFLECTIONSDELAYSCALE
FMOD_REVERB_FLAGS_REFLECTIONSSCALE
FMOD_REVERB_FLAGS_REVERBDELAYSCALE
FMOD_REVERB_FLAGS_REVERBSCALE
FMOD_REVERB_PROPERTIES
Flags
Flags1
Flags2
HFReference
INVENTORY_VIEW_INFO
Instance
LFReference
LIGHT_DEFINITION
MATERIAL
ModulationDepth
ModulationTime
PARTICLE_SYSTEM_DEFINITION
ROOM_LAYOUT_FLAG_AI_NODE_CROUCH
ROOM_LAYOUT_FLAG_AI_NODE_DOORWAY
ROOM_LAYOUT_FLAG_AI_NODE_LARGE_COVER
ROOM_LAYOUT_FLAG_AI_NODE_STONE
ROOM_LAYOUT_FLAG_EXPANDED
ROOM_LAYOUT_FLAG_NOT_THEME
ROOM_LAYOUT_FLAG_NO_THEME
ROOM_LAYOUT_FLAG_RANDOM_ROTATIONS
ROOM_LAYOUT_FLAG_TESTCENTER
ROOM_LAYOUT_FLAG_WEIGHT_PERCENTAGE
ROOM_LAYOUT_GROUP
ROOM_LAYOUT_GROUP_DEFINITION
ROOM_PATH_NODE
ROOM_PATH_NODE_CONNECTION
ROOM_PATH_NODE_CONNECTION_REF
ROOM_PATH_NODE_DEFINITION
ROOM_PATH_NODE_DEF_INDOOR_FLAG
ROOM_PATH_NODE_DEF_NO_PATHNODES_FLAG
ROOM_PATH_NODE_DEF_USE_TUGBOAT
ROOM_PATH_NODE_DEF_USE_TUGBOAT_FLAG
ROOM_PATH_NODE_SET
Reflections
ReflectionsDelay
ReflectionsPan
Reverb
ReverbDelay
ReverbPan
Room
RoomHF
RoomLF
RoomRolloffFactor
SCREEN_EFFECT_DEFINITION
SCREEN_EFFECT_DEF_FLAG_DX10_ONLY
SCREEN_EFFECT_DEF_FLAG_EXCLUSIVE
SKILL_EVENT
SKILL_EVENTS_DEFINITION
SKILL_EVENT_FLAG2_AIM_WITH_WEAPON_ZERO
SKILL_EVENT_FLAG2_CHARGE_POWER_AND_COOLDOWN
SKILL_EVENT_FLAG2_DONT_EXECUTE_STATS
SKILL_EVENT_FLAG2_LASER_DONT_TARGET_UNITS
SKILL_EVENT_FLAG2_LASER_INCLUDE_IN_UI
SKILL_EVENT_FLAG2_MARK_SKILL_AS_SUCCESSFUL
SKILL_EVENT_FLAG2_USE_PARAM0_PCODE
SKILL_EVENT_FLAG2_USE_PARAM1_PCODE
SKILL_EVENT_FLAG2_USE_PARAM2_PCODE
SKILL_EVENT_FLAG2_USE_PARAM3_PCODE
SKILL_EVENT_FLAG2_USE_ULTIMATE_OWNER
SKILL_EVENT_FLAG_360_TARGETING
SKILL_EVENT_FLAG_ADD_TO_CENTER
SKILL_EVENT_FLAG_AIM_WITH_WEAPON
SKILL_EVENT_FLAG_AT_NEXT_COOLDOWN
SKILL_EVENT_FLAG_AUTOAIM_PROJECTILE
SKILL_EVENT_FLAG_CLIENT_ONLY
SKILL_EVENT_FLAG_DONT_VALIDATE_TARGET
SKILL_EVENT_FLAG_DO_WHEN_TARGET_IN_RANGE
SKILL_EVENT_FLAG_FACE_TARGET
SKILL_EVENT_FLAG_FLOAT
SKILL_EVENT_FLAG_FORCE_CONDITION_ON_EVENT
SKILL_EVENT_FLAG_FORCE_NEW
SKILL_EVENT_FLAG_LASER_ATTACKS_LOCATION
SKILL_EVENT_FLAG_LASER_SEEKS_SURFACES
SKILL_EVENT_FLAG_LASER_TURNS
SKILL_EVENT_FLAG_LOOP
SKILL_EVENT_FLAG_PLACE_ON_TARGET
SKILL_EVENT_FLAG_RANDOM_FIRING_DIRECTION
SKILL_EVENT_FLAG_REQUIRES_TARGET
SKILL_EVENT_FLAG_SERVER_ONLY
SKILL_EVENT_FLAG_TARGET_WEAPON
SKILL_EVENT_FLAG_TRANSFER_STATS
SKILL_EVENT_FLAG_USE_AI_TARGET
SKILL_EVENT_FLAG_USE_ANIM_CONTACT_POINT
SKILL_EVENT_FLAG_USE_CHANCE_PCODE
SKILL_EVENT_FLAG_USE_EVENT_OFFSET
SKILL_EVENT_FLAG_USE_EVENT_OFFSET_ABSOLUTE
SKILL_EVENT_FLAG_USE_HOLY_RADIUS_FOR_RANGE
SKILL_EVENT_FLAG_USE_OFFHAND_WEAPON
SKILL_EVENT_FLAG_USE_SKILL_TARGET_LOCATION
SKILL_EVENT_FLAG_USE_UNIT_TARGET
SKILL_EVENT_FLAG_USE_WEAPON_FOR_CONDITION
SKILL_EVENT_HOLDER
SKYBOX_DEFINITION
SKYBOX_MODEL
SOUND_EFFECT
SOUND_EFFECT_DEFINITION
SOUND_REVERB_DEFINITION
STATE_DEFINITION
STATE_EVENT
STATE_EVENT_FLAG_ADD_TO_CENTER
STATE_EVENT_FLAG_CHECK_CONDITION_ON_CLEAR
STATE_EVENT_FLAG_CLEAR_IMMEDIATELY
STATE_EVENT_FLAG_CONTROL_UNIT_ONLY
STATE_EVENT_FLAG_FIRST_PERSON
STATE_EVENT_FLAG_FLOAT
STATE_EVENT_FLAG_FORCE_NEW
STATE_EVENT_FLAG_IGNORE_CAMERA
STATE_EVENT_FLAG_NOT_CONTROL_UNIT
STATE_EVENT_FLAG_ON_CLEAR
STATE_EVENT_FLAG_ON_WEAPONS
STATE_EVENT_FLAG_ON_WEAPONS_ATTACHMENT
STATE_EVENT_FLAG_OTHER_TEAM_ONLY
STATE_EVENT_FLAG_OWNED_BY_CONTROL
STATE_EVENT_FLAG_SAME_TEAM_ONLY
STATE_EVENT_FLAG_SET_IMMEDIATELY
STATE_EVENT_FLAG_SHARE_DURATION
TEXTURE_DEFINITION
bDisplayAppearance
bEnabled
bExists
bFixup
bFollowed
bInitialized
bLoaded
bNoCastShadow
bOff
bPropIsChecked
bPropIsValid
bReadOnly
bShowIcons
dwCode
dwConvertFlags
dwDRLGSeed
dwDefFlags
dwFlags
dwFlags2
dwFlags3
dwRuntimeFlags
dwUnitType
dwUpdateFlags
dwViewFlags
eType
fAltitude
fAmbientIntensity
fAppearanceSHIntensity
fBackgroundSHIntensity
fBlurFactor
fBuffer
fCamDistance
fCamFOV
fCamPitch
fCamRotation
fChance
fCharacterLight_Distance
fCharacterLight_FalloffEnd
fCharacterLight_FalloffStart
fCircleRadius
fCullDistance
fDiagDistBetweenNodes
fDiagDistBetweenNodesSq
fDistance
fDuration
fEaseIn
fEaseOut
fEndTime
fEnvMapBlurriness
fEnvMapGlossThreshold
fEnvMapPower
fFalloffFar
fFalloffNear
fFloats[0]
fFloats[1]
fFloats[2]
fFloats[3]
fGlossiness[0]
fGlossiness[1]
fHeadTurnBoneLimit
fHeadTurnPercentVsTorso
fHeadTurnSpeed
fHeadTurnTotalLimit
fHeight
fHemiLightIntensity
fIntensity
fLODScreensize
fLightmapGlow
fLoopTime
fMaxRopeBendXY
fMaxRopeBendZ
fMaxX
fMaxY
fMetersPerSecond
fMinFacingTargetZ
fMinParticlesPercentDropRate
fMinX
fMinY
fNodeFrequencyX
fNodeFrequencyY
fNodeMaxZ
fNodeMinZ
fNodeOffsetX
fNodeOffsetY
fNormalPower
fParam
fPreviewScale
fPriority
fRadius
fRandChance
fRenderScale
fRopeEndSpeed
fRotation
fScale
fScatterIntensity
fScatterRad
fScatterSharpness
fScrollAmtLCM
fScrollAmt[SAMPLER_DIFFUSE]
fScrollAmt[SAMPLER_NORMAL]
fScrollAmt[SAMPLER_SELFILLUM]
fScrollAmt[SAMPLER_SPECULAR]
fScrollPhaseU[0]
fScrollPhaseU[1]
fScrollPhaseV[0]
fScrollPhaseV[1]
fScrollRateU[0]
fScrollRateU[1]
fScrollRateV[0]
fScrollRateV[1]
fScrollTileU[0]
fScrollTileU[1]
fScrollTileV[0]
fScrollTileV[1]
fSegmentSize
fSelfIlluminationMax
fSelfIlluminationMin
fSelfIlluminationNoiseAmp
fSelfIlluminationNoiseFreq
fSelfIlluminationWaveAmp
fSelfIlluminationWaveHertz
fShadowIntensity
fSizeMultiplier
fSoftParticleContrast
fSoftParticleScale
fSoundPlayChance
fSpawnClassRadius
fSpecularGlow
fSpecularLevel[0]
fSpecularMaskOverride
fSpecularPower
fSpinSpeed
fSpotAngleDeg
fStanceFadeTimePercent
fStartDelay
fStartOffset
fStartTime
fTime
fTrailKnotDuration
fTransitionIn
fTransitionOut
fTurnSpeed
fVelocity
fVelocityClamp
fVelocityMultiplier
fViewCircleRadius
fViewHosePressure
fViewNovaAngle
fViewRangePercent
fViewSpeed
fVorticityConfinementScale
fWindMax
fWindMin
fWorldScale
iSpawnClassExecuteXTimes
nAimBone
nAnimGroupCount
nAnimationCondition
nAppearanceLightingEnvMap
nArraySize
nBackgroundLightingEnvMap
nBehaviorId
nBlendHeight
nBlendWidth
nBlockRun
nBlockStart
nBone
nBoneCount
nBoneWeights
nCameraType
nClipDistance
nConnectionIndex
nCubeMapMIPLevels
nCubeMapTextureID
nCullPriority
nDRLGDefinition
nData
nDelay
nDensityTextureId
nDrawOrder
nDurationType
nDyingParticleSystem
nEdgeIndex
nEdgeNodeCount
nEditType
nEndStance
nEnvMapMIPLevels
nEnvMapTextureID
nExcelIndex
nFileIndex
nFileSize
nFogEnd
nFogStart
nFogStartDistance
nFollowParticleSystem
nFootstep
nFormat
nFramesU
nFramesV
nGPUShader
nGridBorder
nGridDensityTextureIndex
nGridDepth
nGridHeight
nGridObstructorTextureIndex
nGridVelocityTextureIndex
nGridWidth
nGrid_FileNameOffset
nGroup
nHashLengths
nHeadTurnBoneLimitDegrees
nHeadTurnTotalLimitDegrees
nHeight
nId
nIndex
nInitialized
nItemClass
nKnotCount
nKnotCountMax
nLaunchParticleCount
nLayer
nLayoutId
nLevelDefinition
nLightDefId
nLighting
nLocation
nMIPFilter
nMeshLODPriority
nMeshesPerBatchMax
nMipMapLevels
nMipMapUsed
nModelDefId
nModelID
nModelId
nMonsterClass
nMonsterId
nMonsterLevelAdjust
nNeck
nNextParticleSystem
nObjectClass
nObstructorTextureId
nOverrideTextureID[ TEXTURE_DIFFUSE ]
nOverrideTextureID[ TEXTURE_DIFFUSE2 ]
nOverrideTextureID[ TEXTURE_ENVMAP ]
nOverrideTextureID[ TEXTURE_LIGHTMAP ]
nOverrideTextureID[ TEXTURE_NORMAL ]
nOverrideTextureID[ TEXTURE_SELFILLUM ]
nOverrideTextureID[ TEXTURE_SPECULAR ]
nParticleSystemDefId
nPass
nPreviewAppearance
nPreviewHeight
nPreviewMode
nPreviewWardrobeBody
nPreviewWeight
nPriority
nPriorityBoost
nQuest
nRegionID
nRopeEndParticleSystem
nRopePathId
nShaderLineId
nShaderType
nSharpenFilter
nSharpenPasses
nSilhouetteDistance
nSkill
nSkillId
nSkillId2
nSkyboxDefID
nSoundGroup
nSoundId
nSpecularLUTId
nSphereMapTextureID
nSpineBottom
nSpineTop
nSpotUmbraTextureID
nStartStance
nStartStance2
nStartStance3
nStat
nStatId
nState
nStateId
nTechniqueInEffect
nTextureIDs[0]
nTextureIDs[1]
nTextureId
nTheme
nTotalAlpha
nType
nUnitMode
nUnitType
nUnittype
nVelocityTextureId
nViewCameraMode
nViewParticleSpawnThrottle
nVolume
nWardrobeAppearanceGroup
nWardrobeBaseId
nWeaponAnimationGroupId
nWeight
nWidth
pAnimGroups
pAnimations
pBehaviors
pBinding
pBlendRLE
pColorDefinitions
pConnection
pConnections
pEdgeNodes
pEffects
pEventHolders
pEvents
pGrannyAnimation
pGrannyFile
pGrannyModel
pGroups
pHappyNodes
pInventoryViews
pLoader
pLongConnections
pMeshBinding
pModels
pNextInGroup
pNodeHashArray
pPathNodeSets
pPathNodes
pReverb
pRuns
pShortConnections
pbAlpha
pdwColors
pfParams
pfParamsBase
pfParamsVariation
pfWeights
pnSpineSidesTop
pnTextureOverrides
pnWardrobeLayerIds
pnWardrobeLayerParams
pszAimBone
pszAnimationFilePrefix
pszBoneName
pszBoneNames
pszCopyTemplate
pszData
pszDyingParticleSystem
pszEnvName
pszExcelString
pszFile
pszFollowParticleSystem
pszHavokShape
pszIndex
pszLeftFootBone
pszLeftWeaponBone
pszLightName
pszMaterialName
pszModel
pszModelDefName
pszName
pszNeck
pszNextParticleSystem
pszRagdoll
pszRightFootBone
pszRightWeaponBone
pszRopeEndParticleSystem
pszRopePath
pszShaderName
pszSkeleton
pszSpineBottom
pszSpineSidesTop
pszSpineTop
pszString
pszTextureDensityName
pszTextureName
pszTextureObstructorName
pszTextureOverrides
pszTextureVelocityName
pszWeightGroups
pvMuzzleOffset
szAppearanceLightingEnvMapFileName
szBackgroundLightingEnvMapFileName
szDiffuse2OverrideFileName
szDiffuseOverrideFileName
szEnvMapFileName
szModelFile
szNormalOverrideFileName
szParticleSystemDef
szPreviewAppearance
szSelfIllumOverrideFileName
szSkyBoxFileName
szSpecularOverrideFileName
szSpotUmbraTexture
szTechniqueName
szTextureFilenames[0]
szTextureFilenames[1]
tAlphaMin
tAlphaRef
tAmbientColor
tAnimationRate
tAnimationSlidingRateX
tAnimationSlidingRateY
tAppearanceSHCoefsLin_O2.pfBlue[0]
tAppearanceSHCoefsLin_O2.pfBlue[1]
tAppearanceSHCoefsLin_O2.pfBlue[2]
tAppearanceSHCoefsLin_O2.pfBlue[3]
tAppearanceSHCoefsLin_O2.pfGreen[0]
tAppearanceSHCoefsLin_O2.pfGreen[1]
tAppearanceSHCoefsLin_O2.pfGreen[2]
tAppearanceSHCoefsLin_O2.pfGreen[3]
tAppearanceSHCoefsLin_O2.pfRed[0]
tAppearanceSHCoefsLin_O2.pfRed[1]
tAppearanceSHCoefsLin_O2.pfRed[2]
tAppearanceSHCoefsLin_O2.pfRed[3]
tAppearanceSHCoefsLin_O20
tAppearanceSHCoefsLin_O21
tAppearanceSHCoefsLin_O22
tAppearanceSHCoefsLin_O23
tAppearanceSHCoefsLin_O3.pfBlue[0]
tAppearanceSHCoefsLin_O3.pfBlue[1]
tAppearanceSHCoefsLin_O3.pfBlue[2]
tAppearanceSHCoefsLin_O3.pfBlue[3]
tAppearanceSHCoefsLin_O3.pfBlue[4]
tAppearanceSHCoefsLin_O3.pfBlue[5]
tAppearanceSHCoefsLin_O3.pfBlue[6]
tAppearanceSHCoefsLin_O3.pfBlue[7]
tAppearanceSHCoefsLin_O3.pfBlue[8]
tAppearanceSHCoefsLin_O3.pfGreen[0]
tAppearanceSHCoefsLin_O3.pfGreen[1]
tAppearanceSHCoefsLin_O3.pfGreen[2]
tAppearanceSHCoefsLin_O3.pfGreen[3]
tAppearanceSHCoefsLin_O3.pfGreen[4]
tAppearanceSHCoefsLin_O3.pfGreen[5]
tAppearanceSHCoefsLin_O3.pfGreen[6]
tAppearanceSHCoefsLin_O3.pfGreen[7]
tAppearanceSHCoefsLin_O3.pfGreen[8]
tAppearanceSHCoefsLin_O3.pfRed[0]
tAppearanceSHCoefsLin_O3.pfRed[1]
tAppearanceSHCoefsLin_O3.pfRed[2]
tAppearanceSHCoefsLin_O3.pfRed[3]
tAppearanceSHCoefsLin_O3.pfRed[4]
tAppearanceSHCoefsLin_O3.pfRed[5]
tAppearanceSHCoefsLin_O3.pfRed[6]
tAppearanceSHCoefsLin_O3.pfRed[7]
tAppearanceSHCoefsLin_O3.pfRed[8]
tAppearanceSHCoefsLin_O30
tAppearanceSHCoefsLin_O31
tAppearanceSHCoefsLin_O32
tAppearanceSHCoefsLin_O33
tAppearanceSHCoefsLin_O34
tAppearanceSHCoefsLin_O35
tAppearanceSHCoefsLin_O36
tAppearanceSHCoefsLin_O37
tAppearanceSHCoefsLin_O38
tAppearanceSHCoefs_O2.pfBlue[0]
tAppearanceSHCoefs_O2.pfBlue[1]
tAppearanceSHCoefs_O2.pfBlue[2]
tAppearanceSHCoefs_O2.pfBlue[3]
tAppearanceSHCoefs_O2.pfGreen[0]
tAppearanceSHCoefs_O2.pfGreen[1]
tAppearanceSHCoefs_O2.pfGreen[2]
tAppearanceSHCoefs_O2.pfGreen[3]
tAppearanceSHCoefs_O2.pfRed[0]
tAppearanceSHCoefs_O2.pfRed[1]
tAppearanceSHCoefs_O2.pfRed[2]
tAppearanceSHCoefs_O2.pfRed[3]
tAppearanceSHCoefs_O20
tAppearanceSHCoefs_O21
tAppearanceSHCoefs_O22
tAppearanceSHCoefs_O23
tAppearanceSHCoefs_O3.pfBlue[0]
tAppearanceSHCoefs_O3.pfBlue[1]
tAppearanceSHCoefs_O3.pfBlue[2]
tAppearanceSHCoefs_O3.pfBlue[3]
tAppearanceSHCoefs_O3.pfBlue[4]
tAppearanceSHCoefs_O3.pfBlue[5]
tAppearanceSHCoefs_O3.pfBlue[6]
tAppearanceSHCoefs_O3.pfBlue[7]
tAppearanceSHCoefs_O3.pfBlue[8]
tAppearanceSHCoefs_O3.pfGreen[0]
tAppearanceSHCoefs_O3.pfGreen[1]
tAppearanceSHCoefs_O3.pfGreen[2]
tAppearanceSHCoefs_O3.pfGreen[3]
tAppearanceSHCoefs_O3.pfGreen[4]
tAppearanceSHCoefs_O3.pfGreen[5]
tAppearanceSHCoefs_O3.pfGreen[6]
tAppearanceSHCoefs_O3.pfGreen[7]
tAppearanceSHCoefs_O3.pfGreen[8]
tAppearanceSHCoefs_O3.pfRed[0]
tAppearanceSHCoefs_O3.pfRed[1]
tAppearanceSHCoefs_O3.pfRed[2]
tAppearanceSHCoefs_O3.pfRed[3]
tAppearanceSHCoefs_O3.pfRed[4]
tAppearanceSHCoefs_O3.pfRed[5]
tAppearanceSHCoefs_O3.pfRed[6]
tAppearanceSHCoefs_O3.pfRed[7]
tAppearanceSHCoefs_O3.pfRed[8]
tAppearanceSHCoefs_O30
tAppearanceSHCoefs_O31
tAppearanceSHCoefs_O32
tAppearanceSHCoefs_O33
tAppearanceSHCoefs_O34
tAppearanceSHCoefs_O35
tAppearanceSHCoefs_O36
tAppearanceSHCoefs_O37
tAppearanceSHCoefs_O38
tAttachmentDef.dwFlags
tAttachmentDef.eType
tAttachmentDef.fPitch
tAttachmentDef.fRoll
tAttachmentDef.fRotation
tAttachmentDef.fScale
tAttachmentDef.fYaw
tAttachmentDef.nAttachedDefId
tAttachmentDef.nBoneId
tAttachmentDef.nVolume
tAttachmentDef.pszAttached
tAttachmentDef.pszBone
tAttachmentDef.vNormal
tAttachmentDef.vNormal.fX
tAttachmentDef.vNormal.fY
tAttachmentDef.vNormal.fZ
tAttachmentDef.vPosition
tAttachmentDef.vPosition.fX
tAttachmentDef.vPosition.fY
tAttachmentDef.vPosition.fZ
tAttractorDestructionRadius
tAttractorForceOverRadius
tAttractorForceRadius
tAttractorOffsetNormal
tAttractorOffsetSideX
tAttractorOffsetSideY
tAttractorWorldOffsetZ
tBackgroundColor
tBackgroundSHCoefsLin_O2.pfBlue[0]
tBackgroundSHCoefsLin_O2.pfBlue[1]
tBackgroundSHCoefsLin_O2.pfBlue[2]
tBackgroundSHCoefsLin_O2.pfBlue[3]
tBackgroundSHCoefsLin_O2.pfGreen[0]
tBackgroundSHCoefsLin_O2.pfGreen[1]
tBackgroundSHCoefsLin_O2.pfGreen[2]
tBackgroundSHCoefsLin_O2.pfGreen[3]
tBackgroundSHCoefsLin_O2.pfRed[0]
tBackgroundSHCoefsLin_O2.pfRed[1]
tBackgroundSHCoefsLin_O2.pfRed[2]
tBackgroundSHCoefsLin_O2.pfRed[3]
tBackgroundSHCoefsLin_O20
tBackgroundSHCoefsLin_O21
tBackgroundSHCoefsLin_O22
tBackgroundSHCoefsLin_O23
tBackgroundSHCoefsLin_O3.pfBlue[0]
tBackgroundSHCoefsLin_O3.pfBlue[1]
tBackgroundSHCoefsLin_O3.pfBlue[2]
tBackgroundSHCoefsLin_O3.pfBlue[3]
tBackgroundSHCoefsLin_O3.pfBlue[4]
tBackgroundSHCoefsLin_O3.pfBlue[5]
tBackgroundSHCoefsLin_O3.pfBlue[6]
tBackgroundSHCoefsLin_O3.pfBlue[7]
tBackgroundSHCoefsLin_O3.pfBlue[8]
tBackgroundSHCoefsLin_O3.pfGreen[0]
tBackgroundSHCoefsLin_O3.pfGreen[1]
tBackgroundSHCoefsLin_O3.pfGreen[2]
tBackgroundSHCoefsLin_O3.pfGreen[3]
tBackgroundSHCoefsLin_O3.pfGreen[4]
tBackgroundSHCoefsLin_O3.pfGreen[5]
tBackgroundSHCoefsLin_O3.pfGreen[6]
tBackgroundSHCoefsLin_O3.pfGreen[7]
tBackgroundSHCoefsLin_O3.pfGreen[8]
tBackgroundSHCoefsLin_O3.pfRed[0]
tBackgroundSHCoefsLin_O3.pfRed[1]
tBackgroundSHCoefsLin_O3.pfRed[2]
tBackgroundSHCoefsLin_O3.pfRed[3]
tBackgroundSHCoefsLin_O3.pfRed[4]
tBackgroundSHCoefsLin_O3.pfRed[5]
tBackgroundSHCoefsLin_O3.pfRed[6]
tBackgroundSHCoefsLin_O3.pfRed[7]
tBackgroundSHCoefsLin_O3.pfRed[8]
tBackgroundSHCoefsLin_O30
tBackgroundSHCoefsLin_O31
tBackgroundSHCoefsLin_O32
tBackgroundSHCoefsLin_O33
tBackgroundSHCoefsLin_O34
tBackgroundSHCoefsLin_O35
tBackgroundSHCoefsLin_O36
tBackgroundSHCoefsLin_O37
tBackgroundSHCoefsLin_O38
tBackgroundSHCoefs_O2.pfBlue[0]
tBackgroundSHCoefs_O2.pfBlue[1]
tBackgroundSHCoefs_O2.pfBlue[2]
tBackgroundSHCoefs_O2.pfBlue[3]
tBackgroundSHCoefs_O2.pfGreen[0]
tBackgroundSHCoefs_O2.pfGreen[1]
tBackgroundSHCoefs_O2.pfGreen[2]
tBackgroundSHCoefs_O2.pfGreen[3]
tBackgroundSHCoefs_O2.pfRed[0]
tBackgroundSHCoefs_O2.pfRed[1]
tBackgroundSHCoefs_O2.pfRed[2]
tBackgroundSHCoefs_O2.pfRed[3]
tBackgroundSHCoefs_O20
tBackgroundSHCoefs_O21
tBackgroundSHCoefs_O22
tBackgroundSHCoefs_O23
tBackgroundSHCoefs_O3.pfBlue[0]
tBackgroundSHCoefs_O3.pfBlue[1]
tBackgroundSHCoefs_O3.pfBlue[2]
tBackgroundSHCoefs_O3.pfBlue[3]
tBackgroundSHCoefs_O3.pfBlue[4]
tBackgroundSHCoefs_O3.pfBlue[5]
tBackgroundSHCoefs_O3.pfBlue[6]
tBackgroundSHCoefs_O3.pfBlue[7]
tBackgroundSHCoefs_O3.pfBlue[8]
tBackgroundSHCoefs_O3.pfGreen[0]
tBackgroundSHCoefs_O3.pfGreen[1]
tBackgroundSHCoefs_O3.pfGreen[2]
tBackgroundSHCoefs_O3.pfGreen[3]
tBackgroundSHCoefs_O3.pfGreen[4]
tBackgroundSHCoefs_O3.pfGreen[5]
tBackgroundSHCoefs_O3.pfGreen[6]
tBackgroundSHCoefs_O3.pfGreen[7]
tBackgroundSHCoefs_O3.pfGreen[8]
tBackgroundSHCoefs_O3.pfRed[0]
tBackgroundSHCoefs_O3.pfRed[1]
tBackgroundSHCoefs_O3.pfRed[2]
tBackgroundSHCoefs_O3.pfRed[3]
tBackgroundSHCoefs_O3.pfRed[4]
tBackgroundSHCoefs_O3.pfRed[5]
tBackgroundSHCoefs_O3.pfRed[6]
tBackgroundSHCoefs_O3.pfRed[7]
tBackgroundSHCoefs_O3.pfRed[8]
tBackgroundSHCoefs_O30
tBackgroundSHCoefs_O31
tBackgroundSHCoefs_O32
tBackgroundSHCoefs_O33
tBackgroundSHCoefs_O34
tBackgroundSHCoefs_O35
tBackgroundSHCoefs_O36
tBackgroundSHCoefs_O37
tBackgroundSHCoefs_O38
tBinding
tCharacterLight_Color
tColor
tColor0
tColor1
tCondition
tDirLights
tEffect
tFalloff
tFluidSmokeAmbientLight
tFluidSmokeDensityModifier
tFluidSmokeThickness
tFluidSmokeVelocityModifier
tFogColor
tGlowMinDensity
tGroup
tHemiLightColors[0]
tHemiLightColors[1]
tInitAnimation
tIntensity
tLaunchCylinderHeight
tLaunchCylinderRadius
tLaunchDirPitch
tLaunchDirRotation
tLaunchOffsetX
tLaunchOffsetY
tLaunchOffsetZ
tLaunchRopeAlpha
tLaunchRopeColor
tLaunchRopeGlow
tLaunchRopeScale
tLaunchRopeSpringiness
tLaunchRopeStiffness
tLaunchRotation
tLaunchScale
tLaunchSpeed
tLaunchSpeedFromSystemForward
tLaunchSphereRadius
tLaunchVelocityFromSystem
tParam[0].flValue
tParam[1].flValue
tParam[2].flValue
tParam[3].flValue
tParams[0].fValue
tParams[1].fValue
tParticleAcceleration
tParticleAlpha
tParticleAttractorAcceleration
tParticleBounce
tParticleBurst
tParticleCenterRotation
tParticleCenterX
tParticleCenterY
tParticleColor
tParticleDistortionStrength
tParticleDurationPath
tParticleGlow
tParticleRotation
tParticleScale
tParticleSpeedBounds
tParticleStretchBox
tParticleStretchDiamond
tParticleTurnSpeed
tParticleWindInfluence
tParticleWorldAccelerationZ
tParticlesPerMeter
tParticlesPerMeterPerSecond
tParticlesPerSecondPath
tRagdollBlend
tRagdollPower
tRopeAlpha
tRopeDampening
tRopeGlow
tRopeMetersPerTexture
tRopePathScale
tRopeWaveAmplitudeSide
tRopeWaveAmplitudeUp
tRopeWaveFrequency
tRopeWaveSpeed
tRopeWorldAccelerationZ
tRopeZOffsetOverTime
tScatterColor
tSelfIllumation
tSelfIllumationBlend
tTable
vAimOffset
vAimOffset.fX
vAimOffset.fY
vAimOffset.fZ
vCamFocus
vCamFocus.fX
vCamFocus.fY
vCamFocus.fZ
vCorner
vGlowCompensationColor.x
vGlowCompensationColor.y
vGlowCompensationColor.z
vLightOffset
vNeckAim
vNeckAim.fX
vNeckAim.fY
vNeckAim.fZ
vNormal
vPosition
vScale
vVec
vVec.fX
vVec.fY
vVec.fZ
vWindDirection
vWindDirection.fX
vWindDirection.fY
vWindDirection.fZ
"""

CUSTOM = {'bDisplayAppearance': 'B', 'bEnabled': 'B', 'bExists': 'B', 'bFixup': 'B', 'bFollowed': 'B', 'bInitialized': 'B', 'bLoaded': 'B', 'bNoCastShadow': 'B', 'bOff': 'B', 'bPropIsChecked': 'B', 'bPropIsValid': 'B', 'bReadOnly': 'B', 'bShowIcons': 'B', 'dwConvertFlags': 'U', 'dwDRLGSeed': 'U', 'dwDefFlags': 'U', 'dwFlags': 'U', 'dwFlags2': 'U', 'dwFlags3': 'U', 'dwRuntimeFlags': 'U', 'dwUpdateFlags': 'U', 'dwViewFlags': 'U', 'pdwColors': 'U', 'tAttachmentDef.dwFlags': 'U'}

FLAG_GROUP = {'SKILL_EVENT_FLAG2_AIM_WITH_WEAPON_ZERO': 2, 'SKILL_EVENT_FLAG2_USE_PARAM0_PCODE': 2, 'SKILL_EVENT_FLAG2_USE_PARAM1_PCODE': 2, 'SKILL_EVENT_FLAG2_USE_PARAM2_PCODE': 2, 'SKILL_EVENT_FLAG2_USE_PARAM3_PCODE': 2, 'SKILL_EVENT_FLAG2_USE_ULTIMATE_OWNER': 2, 'SKILL_EVENT_FLAG2_CHARGE_POWER_AND_COOLDOWN': 2, 'SKILL_EVENT_FLAG2_MARK_SKILL_AS_SUCCESSFUL': 2, 'SKILL_EVENT_FLAG2_LASER_INCLUDE_IN_UI': 2, 'SKILL_EVENT_FLAG2_LASER_DONT_TARGET_UNITS': 2, 'SKILL_EVENT_FLAG2_DONT_EXECUTE_STATS': 2}

TABLE_CODES = {
    0x0: 'Null',
    0x3031: 'BACKGROUNDSOUNDS3D',
    0x3032: 'MONSTERS',
    0x3033: 'LEVEL_AREAS_GOTHIC_NOUNS',
    0x3130: 'STRING_FILES',
    0x3131: 'BACKGROUNDSOUNDS2D',
    0x3132: 'MONSTER_NAMES',
    0x3133: 'GLOBAL_THEMES',
    0x3230: 'PALETTES',
    0x3231: 'BACKGROUNDSOUNDS',
    0x3232: 'OBJECTTRIGGERS',
    0x3233: 'MOVIES',
    0x3330: 'FONTCOLORS',
    0x3331: 'MUSICSTINGERS',
    0x3332: 'OBJECTS',
    0x3333: 'MOVIE_SUBTITLES',
    0x3430: 'COLORSETS',
    0x3431: 'MUSICSTINGERSETS',
    0x3432: 'LEVEL_AREAS',
    0x3433: 'MOVIELISTS',
    0x3530: 'TEXTURE_TYPES',
    0x3531: 'MUSICGROOVELEVELS',
    0x3532: 'PLAYER_RACE',
    0x3533: 'GAME_GLOBALS',
    0x3631: 'MUSICCONDITIONS',
    0x3632: 'PLAYERS',
    0x3633: 'LEVEL_SCALING',
    0x3731: 'MUSIC',
    0x3732: 'PLAYERLEVELS',
    0x3733: 'LEVEL_AREAS_LINKER',
    0x3831: 'FOOTSTEPS',
    0x3832: 'CHARACTER_CLASS',
    0x3833: 'QUEST_REWARD_BY_LEVEL_TUGBOAT',
    0x3931: 'NPC',
    0x3932: 'SPAWN_CLASS',
    0x3933: 'MONSTER_NAME_TYPES',
    0x4132: 'BONES',
    0x4133: 'CHAT_INSTANCED_CHANNELS',
    0x4141: 'ACHIEVEMENTS',
    0x4231: 'INTERACT_MENU',
    0x4232: 'BONEWEIGHTS',
    0x4233: 'SKILL_STATS',
    0x4241: 'ACT',
    0x4331: 'INTERACT',
    0x4332: 'RENDER_FLAGS',
    0x4333: 'MUSIC_SCRIPT_DEBUG',
    0x4341: 'ACHIEVEMENT_SLOTS',
    0x4431: 'ITEM_QUALITY',
    0x4432: 'DEBUG_BARS',
    0x4433: 'BADGE_REWARDS',
    0x4441: 'CRAFTING_SLOTS',
    0x4530: 'INVENTORY_TYPES',
    0x4531: 'AI_BEHAVIOR',
    0x4532: 'OFFER',
    0x4533: 'DIFFICULTY',
    0x4541: 'FILTER_NAMEFILTER',
    0x4630: 'QUEST_TEMPLATE',
    0x4631: 'AI_START',
    0x4632: 'QUEST_CAST',
    0x4633: 'LEVEL_DRLG_CHOICE',
    0x4641: 'MUSICGROOVELEVELTYPES',
    0x4730: 'SOUNDOVERRIDES',
    0x4731: 'AI_INIT',
    0x4732: 'QUEST',
    0x4733: 'QUEST_RANDOM_FOR_TUGBOAT',
    0x4741: 'LEVEL_AREAS_DESERTGOTHIC_NOUNS',
    0x4831: 'FONT',
    0x4832: 'QUEST_STATE',
    0x4833: 'QUEST_RANDOM_TASKS_FOR_TUGBOAT',
    0x4841: 'LEVEL_AREAS_GOBLIN_NOUNS',
    0x4931: 'EFFECTS',
    0x4932: 'TASKS',
    0x4933: 'QUEST_COUNT_TUGBOAT',
    0x4941: 'LEVEL_AREAS_HEATH_NOUNS',
    0x4A31: 'EFFECTS_SHADERS',
    0x4A32: 'GOSSIP',
    0x4A33: 'WARDROBE_LAYERSET',
    0x4A41: 'LEVEL_AREAS_FOREST_NOUNS',
    0x4B31: 'EFFECTS_FILES',
    0x4B32: 'MATERIALS_COLLISION',
    0x4B33: 'PETLEVEL',
    0x4B41: 'LEVEL_AREAS_FARMLAND_NOUNS',
    0x4C32: 'MATERIALS_GLOBAL',
    0x4C33: 'UI_COMPONENT',
    0x4C41: 'LEVEL_AREAS_CANYON_NOUNS',
    0x4D31: 'LEVEL_FILE_PATHS',
    0x4D32: 'BUDGETS_TEXTURE_MIPS',
    0x4D41: 'LEVEL_AREAS_ADJ_BRIGHT',
    0x4E31: 'ROOM_INDEX',
    0x4E33: 'FILTER_CHATFILTER',
    0x4E41: 'LEVEL_AREAS_PROPERNAMEZONE2',
    0x4F31: 'PROPS',
    0x4F33: 'LEVEL_ENVIRONMENTS',
    0x4F41: 'LEVEL_AREAS_GOBLIN_NAMES',
    0x5031: 'LEVEL_THEMES',
    0x5130: 'SOUNDS',
    0x5131: 'WEATHER',
    0x5230: 'UNITTYPES',
    0x5231: 'WEATHER_SETS',
    0x5330: 'INVLOCIDX',
    0x5331: 'LEVEL_RULES',
    0x5341: 'CMD_MENUS',
    0x5431: 'LEVEL_DRLGS',
    0x5432: 'EXCELTABLES',
    0x5531: 'WARDROBE_APPEARANCE_GROUP',
    0x5532: 'AICOMMON_STATE',
    0x5541: 'PLAYERRANKS',
    0x5630: 'UNIT_EVENT_TYPES',
    0x5631: 'WARDROBE_MODEL_GROUP',
    0x5641: 'DONATION_REWARDS',
    0x5730: 'STATE_EVENT_TYPES',
    0x5731: 'WARDROBE_MODEL',
    0x5732: 'QUEST_STATUS',
    0x5741: 'EMOTES',
    0x5830: 'STATE_LIGHTING',
    0x5831: 'WARDROBE_TEXTURESET_GROUP',
    0x5832: 'QUEST_STATE_VALUE',
    0x5841: 'AFFIX_PICK',
    0x5930: 'STATES',
    0x5931: 'WARDROBE_TEXTURESET',
    0x5932: 'TASK_STATUS',
    0x5941: 'ITEM_SETITEM_GROUPS',
    0x5A30: 'STATS',
    0x5A31: 'WARDROBE_PART',
    0x5A32: 'TAG',
    0x5A41: 'INVLOC_MT',
    0x6130: 'STATS_FUNC',
    0x6131: 'WARDROBE_BLENDOP',
    0x6141: 'ITEM_UPGRADE',
    0x6230: 'DAMAGETYPES',
    0x6232: 'BOOKMARKS',
    0x6241: 'ITEM_UPGRADE_QUALITY',
    0x6330: 'DAMAGE_EFFECTS',
    0x6332: 'SOUNDVCAS',
    0x6341: 'MINIGAME_TYPES',
    0x6430: 'PROPERTIES',
    0x6431: 'WARDROBE_LAYER',
    0x6432: 'SOUNDVCASETS',
    0x6441: 'MINIGAME_TAGS',
    0x6530: 'UNITMODE_GROUPS',
    0x6531: 'WARDROBE_BODY',
    0x6532: 'SOUNDBUSES',
    0x6541: 'MINIGAMES',
    0x6630: 'UNITMODES',
    0x6631: 'MELEEWEAPONS',
    0x6632: 'STATS_SELECTOR',
    0x6641: 'TUTORIALS',
    0x6730: 'ANIMATION_CONDITION',
    0x6731: 'CONDITION_FUNCTIONS',
    0x6732: 'MUSIC_REF',
    0x6741: 'ITEM_EVENT',
    0x6830: 'ANIMATION_STANCE',
    0x6831: 'ITEM_LEVELS',
    0x6832: 'SOUND_MIXSTATES',
    0x6841: 'MONSCALING_FIELDLEVEL',
    0x6930: 'ANIMATION_GROUP',
    0x6932: 'SOUND_MIXSTATE_VALUES',
    0x6941: 'PVP',
    0x6A30: 'SKILLTABS',
    0x6A41: 'PVP_EXP_BASE',
    0x6B30: 'SKILLGROUPS',
    0x6B32: 'LEVEL_ZONES',
    0x6B41: 'PVP_EXP_SCALE',
    0x6C30: 'PROCS',
    0x6C32: 'BUDGETS_MODEL',
    0x6C41: 'PVP_EXP_PER_ENEMY',
    0x6D30: 'SKILLS',
    0x6D31: 'ITEMS',
    0x6D32: 'INITDB',
    0x6D41: 'PVP_EXP_WEIGHT_MATCH_RESULT',
    0x6E30: 'SKILLEVENTTYPES',
    0x6E31: 'ITEM_LOOKS',
    0x6E32: 'QUESTS_TASKS_FOR_TUGBOAT',
    0x6E41: 'PVP_RANKS',
    0x6F30: 'SKILL_LEVELS',
    0x6F31: 'TREASURE',
    0x6F32: 'QUEST_DICTIONARY_FOR_TUGBOAT',
    0x6F41: 'PVP_EXP_PVPPOINT_PER_GAP',
    0x7030: 'INVLOC',
    0x7031: 'QUEST_TITLES_FOR_TUGBOAT',
    0x7032: 'LOADING_TIPS',
    0x7041: 'WEAPON_TEMPERED',
    0x7130: 'INVENTORY',
    0x7131: 'SUBLEVEL',
    0x7132: 'GLOBAL_INDEX',
    0x7141: 'PVP_EXP_PVPPOINT_PER_TIME',
    0x7230: 'CHARDISPLAY',
    0x7231: 'LEVEL',
    0x7232: 'GLOBAL_STRING',
    0x7241: 'PVP_ENTRY_CONDITION',
    0x7330: 'ITEMDISPLAY',
    0x7331: 'FACTION_STANDING',
    0x7332: 'SKU',
    0x7341: 'FATIGUE',
    0x7430: 'ITEM_LOOK_GROUPS',
    0x7431: 'FACTION',
    0x7432: 'LEVEL_AREAS_MADLIB',
    0x7441: 'PVP_EXP_PVPPOINT_PER_PROGRESSTIME',
    0x7530: 'AFFIXTYPES',
    0x7531: 'RECIPES',
    0x7532: 'LEVEL_AREAS_CAVE_NOUNS',
    0x7541: 'PLAYER_EVENT_BUFF',
    0x7630: 'RARENAMES',
    0x7631: 'RECIPELISTS',
    0x7632: 'LEVEL_AREAS_ADJECTIVES',
    0x7641: 'DEFENSEGAME_WAVE',
    0x7730: 'AFFIXES',
    0x7731: 'MISSILES',
    0x7732: 'LEVEL_AREAS_AFFIXS',
    0x7741: 'DEFENSEGAME_MONSTER_BUFF',
    0x7831: 'MONLEVEL',
    0x7832: 'LEVEL_AREAS_SUFFIXS',
    0x7841: 'FLASHBACK_CLASS_TREASURE',
    0x7931: 'MONSCALING',
    0x7932: 'LEVEL_AREAS_PROPERNAMEZONE1',
    0x7941: 'COMBINE',
    0x7A30: 'DIALOG',
    0x7A31: 'MONSTER_QUALITY',
    0x7A32: 'LEVEL_AREAS_TEMPLE_NOUNS',
    0x7A41: 'PLAYER_CONDITION',
    0xFFFFFFF: 'None',
}

HASHES = {strhash(n): n for n in NAMES.split()}


def name_of(h):
    return HASHES.get(h, "0x%08X" % h)


class Elem(object):
    __slots__ = ("hash", "name", "type", "default", "count", "child", "mask",
                 "bitindex", "table")

    def __init__(self, h, t):
        self.hash, self.name, self.type = h, name_of(h), t
        self.default = self.count = self.child = self.mask = self.bitindex = None
        self.table = None


class Reader(object):
    def __init__(self, buf):
        self.b, self.o = buf, 0

    def u32(self):
        v, = struct.unpack_from("<I", self.b, self.o); self.o += 4; return v

    def i32(self):
        v, = struct.unpack_from("<i", self.b, self.o); self.o += 4; return v

    def u16(self):
        v, = struct.unpack_from("<H", self.b, self.o); self.o += 2; return v

    def f32(self):
        v, = struct.unpack_from("<f", self.b, self.o); self.o += 4; return v

    def u8(self):
        v = self.b[self.o]; self.o += 1; return v

    def raw(self, n):
        v = self.b[self.o:self.o + n]; self.o += n; return v


class Cooked(object):
    """One parsed file: .root is the definition tree, .data the values."""

    def __init__(self, buf):
        self.r = Reader(buf)
        self.trees = {}          # definition hash -> first tree seen (for -1 refs)
        r = self.r
        if r.u32() != MAGIC:
            raise ValueError("not a CO0k file")
        if r.i32() != VERSION:
            raise ValueError("unsupported cooked version")
        root_hash, root_count = r.u32(), r.i32()
        self.root = self._parse_def(root_hash, root_count)
        if r.u32() != DATA:
            raise ValueError("'DATA' token missing at 0x%x" % (r.o - 4))
        self.data = self._parse_data(self.root)
        self.trailing = len(buf) - r.o

    # -- definition section -------------------------------------------------
    def _parse_def(self, def_hash, count):
        r = self.r
        tree = {"hash": def_hash, "name": name_of(def_hash), "elems": []}
        self.trees.setdefault(def_hash, tree)
        for _ in range(count):
            e = Elem(r.u32(), r.u16())
            t = e.type
            if t == 0x0D00:
                pass
            elif t in (0x0200, 0x0206, 0x0207):
                n = r.u8()
                e.default = r.raw(n + 1)[:-1].decode("latin1") if n else None
                if t == 0x0206:
                    e.count = r.i32()
            elif t in (0x0000, 0x0007, 0x0700, 0x0707, 0x0A00):
                e.default = r.i32()
            elif t in (0x0006, 0x0A06):
                e.default, e.count = r.i32(), r.i32()
            elif t in (0x0100, 0x0107, 0x0500, 0x0600, 0x0800):
                e.default = r.f32()
            elif t == 0x0106:
                e.default, e.count = r.f32(), r.i32()
            elif t == 0x0B01:
                e.default, e.mask = r.i32() != 0, r.u32()
            elif t == 0x0C02:
                e.bitindex, e.count = r.i32(), r.i32()
                e.default = False
            elif t == 0x0903:
                e.table = r.i32()
            elif t == 0x0905:
                e.table, e.count = r.i32(), r.i32()
            elif t in (0x0308, 0x0309, 0x030A):
                if t == 0x0309:
                    e.count = r.i32()
                child_hash, child_count = r.u32(), r.i32()
                if child_count == -1:
                    if child_hash not in self.trees:
                        raise ValueError("table %s refers to unseen definition %s"
                                         % (e.name, name_of(child_hash)))
                    e.child = self.trees[child_hash]
                else:
                    e.child = self._parse_def(child_hash, child_count)
            elif t == 0x1007:
                e.default = r.i32()
            else:
                raise ValueError("unknown definition token 0x%04X for %s at 0x%x"
                                 % (t, e.name, r.o - 2))
            tree["elems"].append(e)
        return tree

    # -- data section -------------------------------------------------------
    def _string(self):
        n = self.r.i32()
        raw = self.r.raw(n)
        body = raw[:-1] if raw.endswith(b"\0") else raw
        if all(0x20 <= c < 0x7F for c in body):
            return body.decode("latin1")
        return "hex:" + raw.hex()

    def _excel(self):
        n = self.r.u8()
        if n == 0xFF:
            return None
        return self.r.raw(n).decode("latin1")

    def _parse_data(self, tree):
        r = self.r
        elems = tree["elems"]
        nbytes = ((len(elems) - 1) >> 3) + 1 if elems else 0
        bits = r.raw(nbytes)
        out = []                       # list of (Elem, value, present)
        flagword = {}                  # flag group -> dword read once
        for i, e in enumerate(elems):
            if not (bits[i >> 3] >> (i & 7)) & 1:
                out.append((e, e.default, False))
                continue
            t = e.type
            if t in (0x0000, 0x0A00):
                v = r.i32()
                if e.name == "dwDefFlags":
                    # ENVIRONMENT_DEFINITION: this dword *is* the Flags word;
                    # the flag bits that follow share it.
                    flagword[FLAG_GROUP.get(e.name, 1)] = v & 0xFFFFFFFF
                if CUSTOM.get(e.name) == "U":
                    v &= 0xFFFFFFFF
                elif CUSTOM.get(e.name) == "B":
                    v = bool(v)
            elif t in (0x0006, 0x0A06):
                v = [r.i32() for _ in range(e.count)]
            elif t == 0x0007:
                v = [r.i32() for _ in range(r.i32())]
            elif t == 0x0100:
                v = r.f32()
            elif t == 0x0106:
                v = [r.f32() for _ in range(e.count)]
            elif t == 0x0107:
                v = [r.f32() for _ in range(r.i32())]
            elif t == 0x0500:
                v = [tuple(r.f32() for _ in range(3)) for _ in range(r.i32())]
            elif t == 0x0600:
                v = [tuple(r.f32() for _ in range(4)) for _ in range(r.i32())]
            elif t == 0x0200:
                v = self._string()
            elif t == 0x0206:
                v = [self._string() for _ in range(e.count)]
            elif t == 0x0207:
                v = [self._string() for _ in range(r.i32())]
            elif t == 0x0308:
                v = self._parse_data(e.child)
            elif t == 0x0309:
                v = [self._parse_data(e.child) for _ in range(e.count)]
            elif t == 0x030A:
                v = [self._parse_data(e.child) for _ in range(r.i32())]
            elif t == 0x0903:
                v = self._excel()
            elif t == 0x0905:
                v = [self._excel() for _ in range(e.count)]
            elif t in (0x0B01, 0x0C02):
                g = FLAG_GROUP.get(e.name, 1)
                if g not in flagword:
                    flagword[g] = r.u32()
                if t == 0x0B01:
                    v = bool(flagword[g] & e.mask)
                else:
                    v = bool((flagword[g] >> e.bitindex) & 1)
            elif t == 0x1007:
                v = r.raw(r.i32()).hex()
            else:
                raise ValueError("element %s (token 0x%04X) is marked present but is not a cooked type"
                                 % (e.name, t))
            out.append((e, v, True))
        return {"tree": tree, "values": out}


# -------------------------------------------------------------- output ---
def fmt(v):
    if isinstance(v, float):
        return "%g" % v
    if isinstance(v, tuple):
        return "(" + ", ".join(fmt(x) for x in v) + ")"
    if isinstance(v, list):
        return "[" + ", ".join(fmt(x) for x in v) + "]"
    if v is None:
        return "-"
    return str(v)


def dump(node, indent=0, defaults=False, out=sys.stdout):
    pad = "  " * indent
    for e, v, present in node["values"]:
        if not present and not defaults:
            continue
        tag = "" if present else "   (default)"
        if e.type in (0x0308, 0x0309, 0x030A):
            if not present:
                out.write("%s%s: <%s> absent\n" % (pad, e.name, e.child["name"]))
                continue
            subs = v if isinstance(v, list) else [v]
            for k, sub in enumerate(subs):
                idx = "[%d]" % k if isinstance(v, list) else ""
                out.write("%s%s%s: <%s>\n" % (pad, e.name, idx, e.child["name"]))
                dump(sub, indent + 1, defaults, out)
            continue
        if e.type == 0x0903 and e.table is not None:
            tag += "   {%s}" % TABLE_CODES.get(e.table, "table 0x%X" % e.table)
        if e.type in (0x0000, 0x0A00) and isinstance(v, int) and e.name.startswith("dw"):
            tag += "   (0x%08X)" % (v & 0xFFFFFFFF)
        out.write("%s%s: %s%s\n" % (pad, e.name, fmt(v), tag))


def to_xml(node, indent=1, out=sys.stdout):
    pad = "  " * indent
    for e, v, present in node["values"]:
        if not present:
            continue
        if e.type in (0x0308, 0x0309, 0x030A):
            subs = v if isinstance(v, list) else [v]
            if e.type == 0x030A:
                out.write("%s<%sCount>%d</%sCount>\n" % (pad, e.name, len(subs), e.name))
            for sub in subs:
                out.write("%s<%s></%s>\n%s<%s>\n" % (pad, e.name, e.name, pad, e.child["name"]))
                to_xml(sub, indent + 1, out)
                out.write("%s</%s>\n" % (pad, e.child["name"]))
            continue
        items = v if isinstance(v, list) else [v]
        if e.type in (0x0007, 0x0107, 0x0207, 0x0500, 0x0600):
            out.write("%s<%sCount>%d</%sCount>\n" % (pad, e.name, len(items), e.name))
        for it in items:
            if isinstance(it, tuple):
                it = ", ".join(fmt(x) for x in it)
            elif isinstance(it, bool):
                it = "1" if it else "0"
            else:
                it = fmt(it)
            out.write("%s<%s>%s</%s>\n" % (pad, e.name, it, e.name))


def load(path):
    return Cooked(open(path, "rb").read())


def check(dirs):
    ok = bad = 0
    fails = {}
    for d in dirs:
        for root, _, files in os.walk(d):
            for f in files:
                if not f.endswith(".xml.cooked"):
                    continue
                p = os.path.join(root, f)
                try:
                    c = load(p)
                    if c.trailing:
                        raise ValueError("%d trailing byte(s) after data" % c.trailing)
                    ok += 1
                except Exception as ex:      # report, don't stop
                    bad += 1
                    fails.setdefault(str(ex), []).append(p)
    for msg, ps in sorted(fails.items(), key=lambda kv: -len(kv[1])):
        print("FAIL x%d: %s\n    e.g. %s" % (len(ps), msg, ps[0]))
    print("%d ok, %d failed" % (ok, bad))
    return bad == 0


def main():
    args = sys.argv[1:]
    mode = "dump"
    if args and args[0] in ("--defaults", "--xml", "--check"):
        mode = args.pop(0)[2:]
    if not args:
        sys.exit(__doc__)
    if mode == "check":
        sys.exit(0 if check(args) else 1)
    for p in args:
        c = load(p)
        if mode == "xml":
            print("<%s>" % c.root["name"])
            to_xml(c.data)
            print("</%s>" % c.root["name"])
        else:
            if len(args) > 1:
                print("== %s" % p)
            print("%s  (%d element(s) in definition%s)" % (
                c.root["name"], len(c.root["elems"]),
                ", %d trailing bytes!" % c.trailing if c.trailing else ""))
            dump(c.data, 1, defaults=(mode == "defaults"))


if __name__ == "__main__":
    main()

#pragma once
// Layout of the engine's global command-line options object. The fields are engine data, so the order and
// the explicit sizes are reproduced exactly; the instance lives at file VA 0x02ABECC0, which is
// moduleBase + 0x26BECC0 (ASLR-safe, resolved in GetInstance).

#include <windows.h>
#include <cstdint>

// Minimal vector/quaternion shims with the engine's size and alignment (this header is self-contained and
// is only included by the windowed-mode code, so it does not collide with HookMath.h).
namespace HookMath {
    struct __declspec(align(16)) Vector4    { float x = 0, y = 0, z = 0, w = 0; };
    struct __declspec(align(16)) Quaternion { float x = 0, y = 0, z = 0, w = 1; };
}

enum Platform : int {
    Platform_Xenon = 0x0, Platform_PC = 0x1, Platform_PC_DX11 = 0x2, Platform_PS3 = 0x3,
    Platform_WiiU = 0x4, Platform_Durango = 0x5, Platform_Orbis = 0x6, NbPlatforms = 0x7,
    Platform_None = 0x8,
};

enum Language : int {
    Language_None = 0x0, Language_English = 0x1, Language_French = 0x2, Language_Spanish = 0x3,
    Language_Polish = 0x4, Language_German = 0x5, Language_ChineseTrad = 0x6, Language_Hungarian = 0x7,
    Language_Italian = 0x8, Language_Japanese = 0x9, Language_Czech = 0xA, Language_Korean = 0xB,
    Language_Russian = 0xC, Language_Dutch = 0xD, Language_Danish = 0xE, Language_Norwegian = 0xF,
    Language_Swedish = 0x10, Language_Portuguese = 0x11, Language_Brazil = 0x12, Language_Finnish = 0x13,
    Language_Arabic = 0x14, Language_LocTest = 0x15, Language_NbLanguages = 0x16,
};

enum scimitarCommandLineOptionsE3Flow : int { E3Flow_None, E3Flow_SonyStage, E3Flow_SonyTheater, E3Flow_UbiBooth };
enum scimitarCommandLineOptionsE3Demo : int { E3Demo_None, E3Demo_SonyStage, E3Demo_Toy };

class LanguageSetting { public: int m_Languages; Language m_DefaultLanguage; };

class CommandLineOptionsWorld {
public:
    char     m_WorldName[256];
    uint64_t m_WorldID;
    uint64_t m_WorldAreaIDs[16];
    BYTE     m_NumWorldAreaIDs;
    bool     m_Initial;
};

class scimitarCommandLineOptions {
public:
    char System[0x3C];
    bool m_NoSound;
    bool m_NavigationErrorOutput;
    bool m_QuietMode;
    char m_DataSourceName[256];
    bool m_PrintHelpMessageIfNoArguments;
    float m_NearDist;
    float m_FarDist;
    bool m_NoShadows;
    bool m_NoPostFX;
    int m_LightMode;
    float m_LayersDepthBias;
    int m_MultisampleType;
    char m_SyncVersion[256];
    bool m_RefreshBigFileCache;
    bool m_Sync;
    bool m_Revert;
    bool m_SyncSound;
    char m_SyncSoundPlatforms[0x1C];
    int m_SyncSoundPlatformsCount;
    bool m_ExportChanges;
    bool m_ExportHijackedChanges;
    bool m_Import;
    char m_ImportExportForgeFile[256];
    bool m_GetDataChangelist;
    bool m_LogRegisteredClasses;
    bool m_IndexDatabase;
    bool m_StartTool;
    bool m_FastLoad;
    int m_LoadMemorySnapShotFrequency;
    int m_ValidateMemoryFrequency;
    bool m_ConsoleOut;
    bool m_GenerateFastLoad;
    bool m_GenerateFastLoadBootstrap;
    bool m_ValidateFastLoadBootstrap;
    bool m_GenerateFastLoadCharacterForge;
    bool m_ValidateFastLoadCharacterForge;
    bool m_UseFastLoadCharacterForge;
    bool m_LoadBuildersForFastLoadCharacterForge;
    bool m_BuildLoadingGrid;
    bool m_MergeInertComponents;
    bool m_ForceInertComponentsOnMainGrid;
    int m_NumberOfWorld;
    CommandLineOptionsWorld m_Worlds[10];
    bool m_WalkThroughAllCells;
    int m_NumberOfFastLoadPlatform;
    Platform m_FastLoadPlatforms[7];
    LanguageSetting m_TextLanguageSetting;
    bool m_QuitAfterLoad;
    int m_QuitAfterLoadTimeInSec;
    int m_StartAnvil;
    bool m_LocalBigfile;
    char m_MissionRootName[256];
    char m_StartupMissionName[256];
    bool m_NoConsole;
    bool m_MemTypeStats;
    bool m_MemWorldStats;
    bool m_SanityCheck;
    bool m_PrintPerInfo;
    int m_PrintPerThreshold;
    bool m_EnableDynamicOutdoorResources;
    bool m_EnableEngineMetrics;
    bool m_EnableDPD;
    bool m_EnableDebugDisplayLogFile;
    bool m_EnableDebugPadDisplay;
    bool m_EnableSecondaryDebugPadDisplay;
    bool m_EnableStateDebugPadDisplay;
    bool m_EnableDebugPadDisplayLineCounter;
    bool m_KeepDPDInputName;
    int m_DebugPadDisplayFontSize;
    int m_DebugPadDisplayNBLine;
    bool m_EnableProfiling;
    bool m_LoadingHysteresis;
    bool m_WWiseStreamsSeekRemoval;
    bool m_ExportFastload;
    char m_ExportFastloadBF[256];
    char m_ExportFastloadServer[256];
    bool m_AutoProfile;
    int m_AutoProfileThreshold;
    int m_AutoFpsPelThreshold;
    int m_MaxNumThreads;
    bool m_DumpMemoryInfo;
    bool m_CustomPhysMemAlloc;
    bool m_NoOverlappedRegions;
    bool m_NoDefrag;
    int m_HeartbeatFreq;
    bool m_NoTTY;
    bool m_GodMode;
    bool m_NoTriggerGhost;
    bool m_XMLAccomplishmentFileDump;
    char m_ScimitarIniFile[256];
    char m_Ps3HddDataFile[256];
    bool m_DisableVSync;
    bool m_UseTriangleStrips;
    bool m_CompressTexturesMCT;
    bool m_UseEdgeKCacheOptim;
    int m_GlobalBrightnessOverride;
    bool m_SmallFakeEntitiesActivationRange;
    bool m_MinimalShaderMemory;
    char m_GrindJobFile[256];
    bool m_InitializeDataService;
    bool m_InitializeBootStrap;
    bool m_InitializeWindowsThread;
    bool m_ShowFPS;
    bool m_ShowErrorCount;
    bool m_TextDisplay;
    bool m_NoXboxLive;
    bool m_bNoBink;
    bool m_MultithreadRendering;
    bool m_FlushDiskCache;
    bool m_NoDiskCaching;
    bool m_GenerateAllPS3Save;
    bool m_RunUnitTest;
    bool m_NoPink;
    bool m_PauseTimeOfDay;
    int m_OverrideCrowdMaxNpc;
    int m_InitialTimeOfDayTransition;
    bool m_PauseTimeOfDayTransition;
    float m_TimeOfDayDurationModifier;
    bool m_DisplayMapMenu;
    bool m_SetTemplateIniMenu;
    __declspec(align(16)) HookMath::Vector4 m_ForcedCameraPosition;
    HookMath::Quaternion m_ForcedCameraOrientation;
    bool m_AutoCollectGameSpeedStats;
    bool m_CollectAnimFrequencies;
    bool m_GenereteFloorHeightForWorldLightMap;
    bool m_FullScreen;
    bool m_FPP;
    bool m_ShowSingapore;
    scimitarCommandLineOptionsE3Flow m_E3Flow;
    scimitarCommandLineOptionsE3Demo m_E3Demo;
    bool m_E3;
    bool m_comicon;
    bool m_GamesCom;
    bool m_Reveal;
    bool m_EnableWhiteRoom;
    bool m_ValidateNeedResetData;
    bool m_UseAnimus3UIKit;
    bool m_InGameQuestionnaire;
    bool m_LaunchedFromOtherExec;
    bool m_LaunchedFromStore;
    BYTE m_LaunchUserIndex;
    bool m_LaunchUserIndexSet;
    bool m_LostLiveConnection;
    bool m_EthernetDisconnect;
    bool m_ServerMaintenance;
    bool m_RdvDisconnection;
    bool m_CreateRandomizedShaders;
    bool m_IgnoreRandomizedShaders;
    bool m_CreateCgPS3DebugInfo;
    bool m_EnableFogOfWar;
    bool m_EnablePS3TrophiesErrorManagment;
    bool m_EnableOnlineTracking;
    BYTE m_TrackingUserGroup;
    bool m_EnableOnlineServices;
    bool m_EnableOnlinePass;
    bool m_EnableUplayLinking;
    bool m_DisableExclusiveContent;
    bool m_NoFxForShots;
    bool m_EnableFacebook;
    bool m_IsDominoHistoryLogActive;
    uint64_t m_DominoScriptsToLog[16];
    WORD m_DominoScriptsToLogSize;
    Language m_SelectedLanguageAtBoot;
    bool m_SynchronizedRenderer;
    bool m_ParseSoundMetadata;
    bool m_DisableRobotVoices;
    bool m_PlayTest;

    // Resolved from the live module base, so it survives ASLR.
    static scimitarCommandLineOptions* GetInstance()
    {
        return (scimitarCommandLineOptions*)((uintptr_t)GetModuleHandleW(nullptr) + 0x26BECC0);
    }
};

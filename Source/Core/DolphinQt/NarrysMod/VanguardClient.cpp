// A basic test implementation of Netcore for IPC in Dolphin

#pragma warning(disable : 4564)

#include "stdafx.h"

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/State.h"

#include <string>

#include "DolphinMemoryDomain.h"
#include "DolphinQT/MainWindow.h"
#include "NarrysMod/VanguardClient.h"
#include "NarrysMod/VanguardClientInitializer.h"
#include "NarrysMod/VanguardConfigLoader.h"
#include "NarrysMod/VanguardSettingsWrapper.h"

#include <msclr/marshal_cppstd.h>
#include "Core/ConfigLoaders/MovieConfigLoader.h"
#include "Core/ConfigLoaders/NetPlayConfigLoader.h"

ref class VanguardSettingsWrapper;
using namespace cli;
using namespace System;
using namespace RTCV;
using namespace NetCore;
using namespace CorruptCore;
using namespace Vanguard;
using namespace Runtime::InteropServices;
using namespace Threading;
using namespace Collections::Generic;
using namespace Diagnostics;

#using <system.dll>
#using <system.reflection.dll>
#using <system.windows.forms.dll>

#define SRAM_SIZE 25165824
#define ARAM_SIZE 16777216
#define EXRAM_SIZE 67108864

static int CPU_STEP_Count = 0;
static void EmuThreadExecute(Action ^ callback);

// Define this in here as it's managed and weird stuff happens if it's in a header
public
ref class VanguardClient
{
public:
  static NetCoreReceiver ^ receiver;
  static VanguardConnector ^ connector;

  void OnMessageReceived(Object ^ sender, NetCoreEventArgs ^ e);
  void SpecUpdated(Object ^ sender, SpecUpdateEventArgs ^ e);
  void RegisterVanguardSpec();

  void StartClient();
  void RestartClient();

  bool LoadRom(String ^ filename);
  bool LoadState(String ^ filename, StashKeySavestateLocation ^ location);
  bool SaveState(String ^ filename, bool wait);

  String ^ GetConfigAsJson(VanguardSettingsWrapper ^ settings);
  VanguardSettingsWrapper ^ GetConfigFromJson(String ^ json);

  String ^ emuDir = System::IO::Path::GetDirectoryName(System::Reflection::Assembly::GetExecutingAssembly()->Location);
  array<String ^> ^ configPaths;

  const std::string GetFilePath(String ^);
  volatile bool loading = false;
};

ref class ManagedGlobals
{
public:
  static VanguardClient ^ client = nullptr;
};

static void EmuThreadExecute(Action ^ callback)
{
  IntPtr callbackPtr = Marshal::GetFunctionPointerForDelegate(callback);
  std::function<void(void)> nativeCallback =
      static_cast<void(__stdcall*)(void)>(callbackPtr.ToPointer());

  Core::RunAsCPUThread(nativeCallback);
}

static PartialSpec ^ getDefaultPartial() {
      PartialSpec ^ partial = gcnew PartialSpec("RTCSpec");
      partial->Set(VSPEC::SUPPORTS_RENDERING, false);
      partial->Set(VSPEC::SUPPORTS_CONFIG_MANAGEMENT, true);
      partial->Set(VSPEC::SUPPORTS_CONFIG_HANDOFF, true);
      partial->Set(VSPEC::CONFIG_PATHS, ManagedGlobals::client->configPaths);
      partial->Set(VSPEC::SYSTEM, String::Empty);
      partial->Set(VSPEC::SYSTEM, String::Empty);
      return partial;
    }

void VanguardClient::SpecUpdated(Object ^ sender, SpecUpdateEventArgs ^ e)
{
  PartialSpec ^ partial = e->partialSpec;

  LocalNetCoreRouter::Route(NetcoreCommands::CORRUPTCORE, NetcoreCommands::REMOTE_PUSHVANGUARDSPECUPDATE, partial, true);
  LocalNetCoreRouter::Route(NetcoreCommands::UI, NetcoreCommands::REMOTE_PUSHVANGUARDSPECUPDATE, partial, true);
}

void VanguardClient::RegisterVanguardSpec()
{
  PartialSpec ^ emuSpecTemplate = gcnew PartialSpec("VanguardSpec");

  emuSpecTemplate->Insert(getDefaultPartial());

  AllSpec::VanguardSpec =
      gcnew FullSpec(emuSpecTemplate, true);  // You have to feed a partial spec as a template

  // if (VanguardCore.attached)
  // RTCV.Vanguard.VanguardConnector.PushVanguardSpecRef(VanguardCore.VanguardSpec);

  LocalNetCoreRouter::Route(NetcoreCommands::CORRUPTCORE, NetcoreCommands::REMOTE_PUSHVANGUARDSPEC, emuSpecTemplate, true);
  LocalNetCoreRouter::Route(NetcoreCommands::UI, NetcoreCommands::REMOTE_PUSHVANGUARDSPEC, emuSpecTemplate, true);
  AllSpec::VanguardSpec->SpecUpdated += gcnew EventHandler<SpecUpdateEventArgs ^>(this, &VanguardClient::SpecUpdated);
}


// Initialize it
void VanguardClient::StartClient()
{
  receiver = gcnew NetCoreReceiver();
  receiver->MessageReceived +=
      gcnew EventHandler<NetCoreEventArgs ^>(this, &VanguardClient::OnMessageReceived);
  connector = gcnew VanguardConnector(receiver);
}

void VanguardClient::RestartClient()
{
  connector->Kill();
  connector = nullptr;
  StartClient();
}

// Create our VanguardClient
void VanguardClientInitializer::Initialize()
{
  System::Windows::Forms::Form ^ dummy = gcnew System::Windows::Forms::Form();
  IntPtr Handle = dummy->Handle;
  SyncObjectSingleton::SyncObject = dummy;

  SyncObjectSingleton::EmuInvokeDelegate =
      gcnew SyncObjectSingleton::ActionDelegate(&EmuThreadExecute);

  // Start everything
  ManagedGlobals::client = gcnew VanguardClient;

  ManagedGlobals::client->configPaths = gcnew array<String ^>{
      System::IO::Path::Combine(ManagedGlobals::client->emuDir, "User", "Config", "Dolphin.ini"),
      System::IO::Path::Combine(ManagedGlobals::client->emuDir, "User", "Config", "GFX.ini"),
      System::IO::Path::Combine(ManagedGlobals::client->emuDir, "User", "Config", "UI.ini"),
      System::IO::Path::Combine(ManagedGlobals::client->emuDir, "User", "Config", "WiimoteNew.ini"),
      System::IO::Path::Combine(ManagedGlobals::client->emuDir, "User", "Config", "GCKeyNew.ini"),
      System::IO::Path::Combine(ManagedGlobals::client->emuDir, "User", "Config", "GCPadNew.ini")};

  ManagedGlobals::client->StartClient();
  ManagedGlobals::client->RegisterVanguardSpec();

  RTCV::CorruptCore::CorruptCore::StartEmuSide();
}

#pragma region MemoryDomains
  static bool isWii()
  {
    if (SConfig::GetInstance().bWii)
      return true;
    return false;
  }
  
  static array<MemoryDomainProxy ^> ^ GetInterfaces() {
        array<MemoryDomainProxy ^> ^ interfaces = gcnew array<MemoryDomainProxy ^>(2);
        interfaces[0] = (gcnew MemoryDomainProxy(gcnew SRAM));
        if (isWii())
          interfaces[1] = (gcnew MemoryDomainProxy(gcnew EXRAM));
        else
          interfaces[1] = (gcnew MemoryDomainProxy(gcnew ARAM));
  
        return interfaces;
     }
  
  static bool RefreshDomains()
  {
    auto interfaces = GetInterfaces();
    AllSpec::VanguardSpec->Update(VSPEC::MEMORYDOMAINS_INTERFACES, interfaces, true, true);
    LocalNetCoreRouter::Route(NetcoreCommands::CORRUPTCORE,
                              NetcoreCommands::REMOTE_EVENT_DOMAINSUPDATED, true, true);
    return true;
  }

#pragma endregion

String ^ VanguardClient::GetConfigAsJson(VanguardSettingsWrapper ^ settings)
{
  return JsonHelper::Serialize(settings);
}

VanguardSettingsWrapper ^ VanguardClient::GetConfigFromJson(String ^ str)
{
  return JsonHelper::Deserialize<VanguardSettingsWrapper ^>(str);
}

static void STEP_CORRUPT()  // errors trapped by CPU_STEP
{
  StepActions::Execute();
  CPU_STEP_Count++;
  bool autoCorrupt = RTCV::CorruptCore::CorruptCore::AutoCorrupt;
  long errorDelay = RTCV::CorruptCore::CorruptCore::ErrorDelay;
  if (autoCorrupt && CPU_STEP_Count >= errorDelay)
  {
    CPU_STEP_Count = 0;
    array<String ^> ^ domains = AllSpec::UISpec->Get<array<String ^> ^>("SELECTEDDOMAINS");

    BlastLayer ^ bl = RTCV::CorruptCore::CorruptCore::GenerateBlastLayer(domains);
    if (bl != nullptr)
      bl->Apply(false, true);
  }
}

#pragma region Hooks
  void VanguardClientUnmanaged::CORE_STEP()
  {
    // Any step hook for corruption
    STEP_CORRUPT();
  }

  // This is on the main thread not the emu thread
  void VanguardClientUnmanaged::LOAD_GAME_START(std::string romPath)
  {
    StepActions::ClearStepBlastUnits();
    CPU_STEP_Count = 0;

    String ^ gameName = gcnew String(romPath.c_str());
    AllSpec::VanguardSpec->Update(VSPEC::OPENROMFILENAME, gameName, true, true);
  }

  void VanguardClientUnmanaged::LOAD_GAME_DONE()
  {
    PartialSpec ^ gameDone = gcnew PartialSpec("VanguardSpec");

    try
    {
      gameDone->Set(VSPEC::SYSTEM, "Dolphin");
      gameDone->Set(VSPEC::SYSTEMPREFIX, "Dolphin");
      gameDone->Set(VSPEC::SYSTEMCORE, isWii() ? "Wii" : "Gamecube");
      gameDone->Set(VSPEC::SYNCSETTINGS, "");
      gameDone->Set(VSPEC::MEMORYDOMAINS_BLACKLISTEDDOMAINS, "");
      gameDone->Set(VSPEC::MEMORYDOMAINS_INTERFACES, GetInterfaces());
      gameDone->Set(VSPEC::CORE_DISKBASED, true);

      String ^ gameName = gcnew String(SConfig::GetInstance().GetTitleDescription().c_str());
      char replaceChar = L'-';
      gameDone->Set(VSPEC::GAMENAME,
                    CorruptCore_Extensions::MakeSafeFilename(gameName, replaceChar));

      String ^ syncsettings = ManagedGlobals::client->GetConfigAsJson(
          VanguardSettings::GetVanguardSettingsFromDolphin());
      gameDone->Set(VSPEC::SYNCSETTINGS, syncsettings);

      AllSpec::VanguardSpec->Update(gameDone, true, false);
      // This is local. If the domains changed it propgates over netcore
      LocalNetCoreRouter::Route(NetcoreCommands::CORRUPTCORE,
                                NetcoreCommands::REMOTE_EVENT_DOMAINSUPDATED, true, true);
    }
    catch (Exception^ e)
    {
      Trace::WriteLine(e->ToString());
    }
    ManagedGlobals::client->loading = false;
  }

  void VanguardClientUnmanaged::GAME_CLOSED()
  {
    AllSpec::VanguardSpec->Update(VSPEC::OPENROMFILENAME, "", true, true);
  }
#pragma endregion

/*ENUMS FOR THE SWITCH STATEMENT*/
enum COMMANDS
{
  SAVESAVESTATE,
  LOADSAVESTATE,
  REMOTE_LOADROM,
  REMOTE_CLOSEGAME,
  REMOTE_DOMAIN_GETDOMAINS,
  REMOTE_KEY_SETSYNCSETTINGS,
  REMOTE_KEY_SETSYSTEMCORE,
  REMOTE_EVENT_EMU_MAINFORM_CLOSE,
  REMOTE_EVENT_EMUSTARTED,
  REMOTE_ISNORMALADVANCE,
  REMOTE_EVENT_CLOSEEMULATOR,
  REMOTE_ALLSPECSSENT,
  UNKNOWN
};

inline COMMANDS CheckCommand(String ^ inString)
{
  if (inString == "LOADSAVESTATE")
    return LOADSAVESTATE;
  if (inString == "SAVESAVESTATE")
    return SAVESAVESTATE;
  if (inString == "REMOTE_LOADROM")
    return REMOTE_LOADROM;
  if (inString == "REMOTE_CLOSEGAME")
    return REMOTE_CLOSEGAME;
  if (inString == "REMOTE_ALLSPECSSENT")
    return REMOTE_ALLSPECSSENT;
  if (inString == "REMOTE_DOMAIN_GETDOMAINS")
    return REMOTE_DOMAIN_GETDOMAINS;
  if (inString == "REMOTE_KEY_SETSYSTEMCORE")
    return REMOTE_KEY_SETSYSTEMCORE;
  if (inString == "REMOTE_EVENT_EMU_MAINFORM_CLOSE")
    return REMOTE_EVENT_EMU_MAINFORM_CLOSE;
  if (inString == "REMOTE_EVENT_EMUSTARTED")
    return REMOTE_EVENT_EMUSTARTED;
  if (inString == "REMOTE_ISNORMALADVANCE")
    return REMOTE_ISNORMALADVANCE;
  if (inString == "REMOTE_EVENT_CLOSEEMULATOR")
    return REMOTE_EVENT_CLOSEEMULATOR;
  if (inString == "REMOTE_ALLSPECSSENT")
    return REMOTE_ALLSPECSSENT;
  return UNKNOWN;
}

/* IMPLEMENT YOUR COMMANDS HERE */

const std::string VanguardClient::GetFilePath(String ^ filename)
{
  std::string converted_filename = msclr::interop::marshal_as<std::string>(filename);
  return converted_filename;
}

bool VanguardClient::LoadRom(String ^ filename)
{
  String ^ currentOpenRom = "";
  if (AllSpec::VanguardSpec->Get<String ^>(VSPEC::OPENROMFILENAME) != "")
    currentOpenRom = AllSpec::VanguardSpec->Get<String ^>(VSPEC::OPENROMFILENAME);

  // Game is not running
  if (currentOpenRom != filename)
  {
    const std::string& path = GetFilePath(filename);
    ManagedGlobals::client->loading = true;
    VanguardClientInitializer::win->StartGame(path);

    // We have to do it this way to prevent deadlock due to synced calls. It sucks but it's required at the moment
    while (ManagedGlobals::client->loading)
    {
      Thread::Sleep(20);
      System::Windows::Forms::Application::DoEvents();
    }
  }
  return true;
}

bool VanguardClient::LoadState(String ^ filename, StashKeySavestateLocation ^ location)
{
  StepActions::ClearStepBlastUnits();
  std::string converted_filename = msclr::interop::marshal_as<std::string>(filename);
  State::LoadAs(converted_filename);
  return true;
}

bool VanguardClient::SaveState(String ^ filename, bool wait)
{
  std::string converted_filename = msclr::interop::marshal_as<std::string>(filename);
  if (Core::IsRunningAndStarted())
  {
    State::SaveAs(converted_filename, wait);
    return true;
  }
  return false;
}

//No fun anonymous classes with closure here
#pragma region Delegates
  void StopGame()
  {
    Core::Stop();
  }

  void AllSpecsSent()
  {
    AllSpec::VanguardSpec->Update(VSPEC::EMUDIR, ManagedGlobals::client->emuDir, true, true);
    VanguardClientInitializer::win->Show();
  }
#pragma endregion


// Provides functionality equivalent to the "is" keyword
template <class T, class U>
Boolean isinst(U u)
{
  return dynamic_cast<T>(u) != nullptr;
}

/* THIS IS WHERE YOU HANDLE ANY RECEIVED MESSAGES */
void VanguardClient::OnMessageReceived(Object ^ sender, NetCoreEventArgs ^ e)
{
  NetCoreMessage ^ message = e->message;

  // Can't define this unless it's used as SLN is set to treat warnings as errors.
  // NetCoreSimpleMessage^ simpleMessage = (NetCoreSimpleMessage^)message;

  NetCoreSimpleMessage ^ simpleMessage;
  if (isinst<NetCoreSimpleMessage ^>(message))
  {
    simpleMessage = static_cast<NetCoreSimpleMessage ^>(message);
  }

  NetCoreAdvancedMessage ^ advancedMessage;
  if (isinst<NetCoreAdvancedMessage ^>(message))
  {
    advancedMessage = static_cast<NetCoreAdvancedMessage ^>(message);
  }

  switch (CheckCommand(message->Type))
  {
  case REMOTE_ALLSPECSSENT:
  {
    SyncObjectSingleton::GenericDelegate ^ g = gcnew SyncObjectSingleton::GenericDelegate(&AllSpecsSent);
    SyncObjectSingleton::FormExecute(g);
  }
  break;

  case LOADSAVESTATE:
  {
    NetCoreAdvancedMessage ^ advancedMessage = (NetCoreAdvancedMessage ^) e->message;
    array<Object ^> ^ cmd = static_cast<array<Object ^> ^>(advancedMessage->objectValue);
    String ^ path = static_cast<String ^>(cmd[0]);
    StashKeySavestateLocation ^ location = safe_cast<StashKeySavestateLocation ^>(cmd[1]);

    // Clear out any old settings
    Config::ClearCurrentVanguardLayer();

    // Load up the sync settings
    String ^ settingStr = AllSpec::VanguardSpec->Get<String ^>(VSPEC::SYNCSETTINGS);
    if (settingStr != nullptr)
    {
      VanguardSettingsWrapper ^ settings = GetConfigFromJson(settingStr);
      if (settings != nullptr)
        Config::AddLayer(ConfigLoaders::GenerateVanguardConfigLoader(&VanguardSettings::GetVanguardSettingFromVanguardSettingsWrapper(settings)));
    }
    e->setReturnValue(ManagedGlobals::client->LoadState(path, location));
  }
  break;

  case SAVESAVESTATE:
  {
    String ^ Key = (String ^)(advancedMessage->objectValue);
    // Build the shortname
    String ^ quickSlotName = Key + ".timejump";

    // Get the prefix for the state
    String ^ gameName = gcnew String(SConfig::GetInstance().GetTitleDescription().c_str());

    char replaceChar = L'-';
    String ^ prefix = CorruptCore_Extensions::MakeSafeFilename(gameName, replaceChar);
    prefix = prefix->Substring(prefix->LastIndexOf('\\') + 1);

    String ^ path = nullptr;
    // Build up our path
    path = RTCV::CorruptCore::CorruptCore::workingDir + IO::Path::DirectorySeparatorChar +
           "SESSION" + IO::Path::DirectorySeparatorChar + prefix + "." + quickSlotName + ".State";

    // If the path doesn't exist, make it
    IO::FileInfo ^ file = gcnew IO::FileInfo(path);
    if (file->Directory != nullptr && file->Directory->Exists == false)
      file->Directory->Create();

    if (ManagedGlobals::client->SaveState(path, false) && Core::IsRunningAndStarted())
      e->setReturnValue(path);
  }
  break;

  case REMOTE_LOADROM:
  {
    // Clear out any old settings
    Config::ClearCurrentVanguardLayer();
    String ^ filename = (String ^) advancedMessage->objectValue;
    ManagedGlobals::client->LoadRom(filename);
  }
  break;

  case REMOTE_CLOSEGAME:
  {
    SyncObjectSingleton::GenericDelegate ^ g = gcnew SyncObjectSingleton::GenericDelegate(&StopGame);
    SyncObjectSingleton::FormExecute(g);
  }
  break;

  case REMOTE_DOMAIN_GETDOMAINS:
  {
    RefreshDomains();
  }
  break;

  case REMOTE_KEY_SETSYNCSETTINGS:
  {
    String ^ settings = (String ^)(advancedMessage->objectValue);
    AllSpec::VanguardSpec->Set(VSPEC::SYNCSETTINGS, settings);
  }
  break;

  case REMOTE_KEY_SETSYSTEMCORE:
  {
    // Do nothing
  }
  break;

  case REMOTE_EVENT_EMUSTARTED:
  {

  }
  break;

  case REMOTE_ISNORMALADVANCE:
  {
    // Todo - Dig out fast forward?
    e->setReturnValue(true);
  }
  break;

  case REMOTE_EVENT_EMU_MAINFORM_CLOSE:
  case REMOTE_EVENT_CLOSEEMULATOR:
  {
    Environment::Exit(0);
  }
  break;

  default:
    break;
  }
}

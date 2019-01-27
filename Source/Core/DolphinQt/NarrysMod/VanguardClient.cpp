//A basic test implementation of Netcore for IPC in Dolphin

#pragma warning(disable:4564) 

#include "stdafx.h"

#include "Core/State.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/DSP.h"
#include "Core/PatchEngine.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Common/SPSCQueue.h"

#include <string>  
#include <iostream>

#include "VanguardClient.h"

#include <msclr/marshal_cppstd.h>

using namespace cli;
using namespace System;
using namespace RTCV;
using namespace RTCV::NetCore;
using namespace RTCV::Vanguard;
using namespace System::Runtime::InteropServices;
using namespace System::Threading;
using namespace System::Collections::Generic;

#using <system.dll>
using namespace System::Diagnostics;

#define SRAM_SIZE 25165824
#define ARAM_SIZE 16777216
#define EXRAM_SIZE 67108864

/*
Trace::Listeners->Add(gcnew TextWriterTraceListener(Console::Out));
Trace::AutoFlush = true;
Trace::WriteLine(filename);
*/


delegate void MessageDelegate(Object ^);



public ref class MemoryDomain
{
public:
  int size = 0;
  int offset = 0;
  String ^ name = "NULL";
};


//Define this in here as it's managed and it can't be in VanguardClient.h as that's included in unmanaged code. Could probably move this to a header
public ref class VanguardClient
{
  public:
   static RTCV::NetCore::NetCoreReceiver ^ receiver;
   static RTCV::Vanguard::VanguardConnector ^ connector;

  void OnMessageReceived(Object^  sender, RTCV::NetCore::NetCoreEventArgs^  e);
  
  void StartClient();
  void RestartClient();

  void LoadState(String^ filename);
  void SaveState(String^ filename, bool wait);
  Byte PeekByte(long address, MemoryDomain ^ domain);
  void PokeByte(Object ^ data);
  array<Byte>^ PeekBytes(long address, int range, MemoryDomain ^ domain);
  void PokeBytes(Object ^ data);


  array<Byte>^ PeekAddresses(array<long>^ addresses);
  void PokeAddresses(Object ^ data);

  
	static System::Collections::Generic::Queue<Delegate ^> ^ actionQueue = gcnew System::Collections::Generic::Queue<Delegate ^>();
  static System::Collections::Generic::Queue<Object ^> ^ parameterQueue = gcnew System::Collections::Generic::Queue<Object ^>();


  MemoryDomain^ GetDomain(long address, int range, MemoryDomain ^ domain);

  static Mutex ^ mutex = gcnew Mutex(false, "VanguardMutex");

};



void VanguardClientUnmanaged::CORE_STEP()
{
  // Consume the action queue

  Stopwatch ^ watch1 = Stopwatch::StartNew();
  if (VanguardClient::actionQueue->Count != 0 && VanguardClient::mutex->WaitOne())
  {
    Stopwatch ^ watch2 = Stopwatch::StartNew();
    int count = VanguardClient::actionQueue->Count;
    for (int i = 0; i < count; i++)
    {
      MessageDelegate ^ temp = (MessageDelegate ^)(VanguardClient::actionQueue->Dequeue());
      Object ^ temp2 = (VanguardClient::parameterQueue->Dequeue());
      temp(temp2);

    }
    Trace::WriteLine("Took " + watch2->ElapsedTicks + " ticks / " + watch2->ElapsedMilliseconds + " ms to empty the queue");
    VanguardClient::mutex->ReleaseMutex();
  }
  if (watch1->ElapsedMilliseconds > 0)
    Trace::WriteLine("Took " + watch1->ElapsedTicks + " ticks / " + watch1->ElapsedMilliseconds +
                   " ms to CORE_Step");
}

//Create our VanguardClient
void VanguardClientInitializer::Initialize()
{
  VanguardClient^ client = gcnew VanguardClient;
  client->StartClient();
}

void VanguardClientInitializer::isWii()
{
  if (SConfig::GetInstance().bWii)
    VanguardClient::connector->SendSyncedMessage("WII");
  else
    VanguardClient::connector->SendSyncedMessage("GAMECUBE");
}

//Initialize it 
void VanguardClient::StartClient() 
{
  VanguardClient::receiver = gcnew RTCV::NetCore::NetCoreReceiver();
  VanguardClient::receiver->MessageReceived += gcnew EventHandler<NetCore::NetCoreEventArgs ^>(this, &VanguardClient::OnMessageReceived);
  VanguardClient::connector = gcnew RTCV::Vanguard::VanguardConnector(receiver);
}

void VanguardClient::RestartClient()
{
  VanguardClient::connector->Kill();
  VanguardClient::connector = nullptr;
  StartClient();
}

/* IMPLEMENT YOUR COMMANDS HERE */
void VanguardClient::LoadState(String^ filename) {

  std::string converted_filename = msclr::interop::marshal_as< std::string >(filename);
  State::LoadAs(converted_filename);

  Trace::Listeners->Insert(0, gcnew TextWriterTraceListener(Console::Out));
  Trace::AutoFlush = true;
  Trace::WriteLine(filename);
}
void VanguardClient::SaveState(String^ filename, bool wait) {

  std::string converted_filename = msclr::interop::marshal_as< std::string >(filename);
  State::SaveAs(converted_filename, wait);
}

//MEMORY OFFSETS ARE ABSOLUTE
//THIS IS CURRENTLY BEING HANDLED ON THE DOLPHIN SIDE
//SRAM = 0x80000000-0x817FFFFF
//EXRAM = 0x90000000-0x93FFFFFF

//ARAM = 0x80000000-0x81000000
//ARAM HAS TO BE WRITTEN TO WITH A DIFFERENT FUNCTION THAN THE OTHERS

//Memory::Write_U8 and Memory::Read_U8 for SRAM and EXRAM
//DSP::ReadARAM and DSP::WriteAram for ARAM


//THE INTERNAL FUNCTIONS TAKE VALUE, ADDRESS NOT ADDRESS,VALUE

void VanguardClient::PokeByte(Object ^ data)
{
 // Stopwatch ^ unbox = Stopwatch::StartNew();
  long long address = Convert::ToInt64(((array<Object ^> ^) data)[0]);
  Byte value = Convert::ToByte(((array<Object ^> ^) data)[1]);
  MemoryDomain ^ domain = (MemoryDomain ^)((array<Object ^> ^) data)[2];
 // unbox->Stop();
 // Trace::WriteLine("Took " + unbox->ElapsedTicks + " ms to unbox PokeByte params");

  //Stopwatch ^ pokebyte = Stopwatch::StartNew();
  if (domain->name == "SRAM" && (address - domain->offset) < domain->size)
    Memory::Write_U8((Convert::ToByte(value)), Convert::ToUInt32(address));
  else if (domain->name == "EXRAM" && (address - domain->offset) < domain->size)
    Memory::Write_U8(Convert::ToByte(value), Convert::ToUInt32(address));
  else if (domain->name == "ARAM" && (address - domain->size - domain->offset) < domain->size)
    DSP::WriteARAM(Convert::ToByte(value), Convert::ToUInt32(address - SRAM_SIZE));
 // pokebyte->Stop();
 // Trace::WriteLine("Took " + pokebyte->ElapsedTicks + " ms to PokeByte params");
}

Byte VanguardClient::PeekByte(long address, MemoryDomain ^ domain) {

  if (domain->name == "SRAM" && (address - domain->offset) < domain->size)
    return Memory::Read_U8(Convert::ToUInt32(address));

  else if (domain->name == "EXRAM" && (address - domain->offset) < domain->size)
    return Memory::Read_U8(Convert::ToUInt32(address));

  else if (domain->name == "ARAM" && (address - domain->size - domain->offset) < domain->size)
    return DSP::ReadARAM(Convert::ToUInt32(address - SRAM_SIZE));

  return -1;
}


void VanguardClient::PokeBytes(Object ^ data){

  /**/
    long long address = Convert::ToInt64(((array<Object ^> ^) data)[0]);
    array<Byte>^ values = (array<Byte>^)((array<Object ^> ^) data)[1];
    int range = Convert::ToInt32(((array<Object ^> ^) data)[2]);
    MemoryDomain ^ domain = (MemoryDomain ^)((array<Object ^> ^) data)[3];

   for (int i = 0; i < range; i++)
   {
     array<Object ^> ^ temp = {address, values[i], domain};
     PokeByte(temp);
   }
   

}


array<Byte>^ VanguardClient::PeekBytes(long address, int range, MemoryDomain ^ domain) {

  array<Byte>^ byte = gcnew array<Byte>(range);
  for (int i = 0; i < range; i++)
    byte[i] = PeekByte(address + i, domain);

  return byte;
}



array<Byte>^ VanguardClient::PeekAddresses(array<long>^ addresses) {

  MemoryDomain ^ domain = gcnew MemoryDomain;
  array<Byte>^ bytes = gcnew array<Byte>(sizeof(addresses));

  for (int i = 0; i < sizeof(addresses); i++) {
    domain = GetDomain(addresses[i], 1, domain);
    bytes[i] = PeekByte(addresses[i], domain);
  }
    
  return bytes;
}

void VanguardClient::PokeAddresses(Object ^ data){

  array<long long> ^ addresses = (array<long long> ^)((array<Object ^> ^) data)[0];
  array<Byte> ^ values = (array<Byte> ^)((array<Object ^> ^) data)[1];
  MemoryDomain^ domain = gcnew MemoryDomain;

  for (int i = 0; i < sizeof(addresses); i++) {
    domain = GetDomain(addresses[i], 1, domain);
    array<Object ^> ^ temp = {addresses[i], values[i], domain};
    PokeByte(temp);
  }
    
}

MemoryDomain^ VanguardClient::GetDomain(long address, int range, MemoryDomain ^ domain) {
  //Hardcode this logic for now.
  if (address + range <= SRAM_SIZE) {
    domain->name = ("SRAM");
    domain->size = SRAM_SIZE;
  }
  else if (SConfig::GetInstance().bWii) {
    domain->name = "EXRAM";
    domain->offset = SRAM_SIZE;
    domain->size = EXRAM_SIZE;
  }
  else {
    domain->name = "ARAM";
    domain->size = ARAM_SIZE;
  }
  return domain;
}

/*ENUMS FOR THE SWITCH STATEMENT*/
enum COMMANDS {
  LOADSTATE,
  SAVESTATE,
  POKEBYTE,
  PEEKBYTE,
  POKEBYTES,
  PEEKBYTES,
  POKEADDRESSES,
  PEEKADDRESSES,
  UNKNOWN
};

inline COMMANDS CheckCommand(String^ inString) {
  if (inString == "LOADSTATE") return LOADSTATE;
  if (inString == "SAVESTATE") return SAVESTATE;
  if (inString == "POKEBYTE") return POKEBYTE;
  if (inString == "PEEKBYTE") return PEEKBYTE;
  if (inString == "POKEBYTES") return POKEBYTES;
  if (inString == "PEEKBYTES") return PEEKBYTES;
  if (inString == "POKEADDRESSES") return POKEADDRESSES;
  if (inString == "PEEKADDRESSES") return PEEKADDRESSES;
  return UNKNOWN;
}


/* THIS IS WHERE YOU HANDLE ANY RECEIVED MESSAGES */
void VanguardClient::OnMessageReceived(Object^ sender, NetCoreEventArgs^ e)
{

  Trace::Listeners->Insert(0, gcnew TextWriterTraceListener(Console::Out));
  Trace::AutoFlush = true;

  NetCoreMessage ^ message = e->message;

  //Can't define this unless it's used as SLN is set to treat warnings as errors.
  //NetCoreSimpleMessage ^ simpleMessage = (NetCoreSimpleMessage^)message;

  NetCoreAdvancedMessage ^ advancedMessage = (NetCoreAdvancedMessage^)message;

  switch (CheckCommand(message->Type)) {
  case LOADSTATE: {
    if (Core::GetState() == Core::State::Running)
      LoadState((advancedMessage->objectValue)->ToString());
  }
    
    break;

  case SAVESTATE: {
    if (Core::GetState() == Core::State::Running)
      SaveState((advancedMessage->objectValue)->ToString(), 0);
  }
    break;

  case POKEBYTE: {

    if (Core::GetState() == Core::State::Running) {

      long address = Convert::ToInt64(((array<Object^>^)advancedMessage->objectValue)[0]);
      Byte value = Convert::ToByte(((array<Object^>^)advancedMessage->objectValue)[1]);

      MemoryDomain ^ domain = gcnew MemoryDomain;
      domain = GetDomain(address,1,domain);

      MessageDelegate ^ actionObject = gcnew MessageDelegate(this, &VanguardClient::PokeByte);
      array<Object ^> ^ params = {address, value, domain};
      actionQueue->Enqueue(actionObject);

      parameterQueue->Enqueue(params);

    }
    break;

  }

  case PEEKBYTE:
  {
    if (Core::GetState() == Core::State::Running) {
      Trace::WriteLine("Entering PEEKBYTE");
      

      long address = Convert::ToInt64(advancedMessage->objectValue);
      MemoryDomain ^ domain = gcnew MemoryDomain;

      domain = GetDomain(address, 1, domain);
      e->setReturnValue(PeekByte(address, domain));
       Trace::WriteLine("Exiting PEEKBYTE");
    }
    break;
  }
  case POKEBYTES: {

    if (Core::GetState() == Core::State::Running) {
    //  Trace::WriteLine("Entering POKEBYTES");

      long address = Convert::ToInt64(((array<Object^>^)advancedMessage->objectValue)[0]);
      int range = Convert::ToInt32(((array<Object^>^)advancedMessage->objectValue)[1]);
      array<Byte>^ value = (array<Byte>^)((array<Object^>^)advancedMessage->objectValue)[2];

      MemoryDomain ^ domain = gcnew MemoryDomain;
      domain = GetDomain(address, range, domain);

      
      MessageDelegate ^ actionObject = gcnew MessageDelegate(this, &VanguardClient::PokeBytes);
      array<Object ^> ^ params = {address, value, range, domain};
      actionQueue->Enqueue(actionObject);
      parameterQueue->Enqueue(params);

      //PokeBytes(address, value, range, domain);

    //  Trace::WriteLine("Exiting POKEBYTES");
    }
    break;
  }

  case PEEKBYTES:
  {

    if (Core::GetState() == Core::State::Running) {
      Trace::WriteLine("Entering PEEKBYTES");

      long address = Convert::ToInt64(((array<Object^>^)advancedMessage->objectValue)[0]);
      int range = Convert::ToByte(((array<Object^>^)advancedMessage->objectValue)[1]);

      MemoryDomain ^ domain = gcnew MemoryDomain;
      domain = GetDomain(address, range, domain);

      e->setReturnValue(PeekBytes(address, range, domain));
      Trace::WriteLine("Exiting PEEKBYTES");
    }
    break;
  }

  case POKEADDRESSES: {

    if (Core::GetState() == Core::State::Running) {
      Trace::WriteLine("Entering POKEADDRESSES");

      array<long long> ^ addresses =
          (array<long long> ^)((array<Object ^> ^) advancedMessage->objectValue)[0];
      array<Byte> ^ values = (array<Byte> ^)((array<Object ^> ^) advancedMessage->objectValue)[1];

      MessageDelegate ^ actionObject = gcnew MessageDelegate(this, &VanguardClient::PokeAddresses);
      array<Object ^> ^ params = {addresses, values};
      actionQueue->Enqueue(actionObject);
      parameterQueue->Enqueue(params);

      //PokeAddresses(addresses, values);
      Trace::WriteLine("Exiting POKEADDRESSES");
    }
    break;
  }

  case PEEKADDRESSES:
  {

    if (Core::GetState() == Core::State::Running) {
      Trace::WriteLine("Entering PEEKADDRESSES");

      array<long>^ addresses = (array<long>^)((array<Object^>^)advancedMessage->objectValue)[0];
      
      e->setReturnValue(PeekAddresses(addresses));
      Trace::WriteLine("Exiting PEEKADDRESSES");
    }
    break;
  }

  default:
    break;
  }
}

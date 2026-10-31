#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <string>
#include <vector>
#include <fstream>
namespace fs=std::filesystem;
std::wstring env(const wchar_t* name){DWORD n=GetEnvironmentVariableW(name,nullptr,0);if(!n)return {};std::wstring s(n,L'\0');GetEnvironmentVariableW(name,s.data(),n);s.resize(n-1);return s;}
void set(const wchar_t* n,const fs::path& p){SetEnvironmentVariableW(n,p.c_str());}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){
 try{
  wchar_t self[32768];DWORD n=GetModuleFileNameW(nullptr,self,32768);if(!n||n==32768)throw std::runtime_error("Cannot locate launcher.");
  const fs::path root=fs::path(self).parent_path(),exe=root/L"bin/LuigisMansionRecomp.exe";
  int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);bool flat=fs::path(self).stem()==L"Play Flat Screen",check=false,diagnostics=false,frameSafe=false;
  for(int i=1;i<argc;++i){if(std::wstring(argv[i])==L"--flat")flat=true;if(std::wstring(argv[i])==L"--check")check=true;if(std::wstring(argv[i])==L"--diagnostics")diagnostics=true;if(std::wstring(argv[i])==L"--frame-safe")frameSafe=true;}LocalFree(argv);
  if(!fs::is_regular_file(exe))throw std::runtime_error("Missing bin/LuigisMansionRecomp.exe. Extract the entire ZIP first.");
  std::wstring loader=env(L"LM_OPENXR_LOADER");
  if(!flat&&loader.empty()&&fs::is_regular_file(root/L"bin/openxr_loader.dll"))
   loader=fs::absolute(root/L"bin/openxr_loader.dll").wstring();
  if(!flat&&loader.empty()){
   // Proton may supply its OpenXR bridge as a Windows loader DLL. Prefer that
   // over assuming the Windows PC SteamVR installation path exists.
   HMODULE mod=LoadLibraryW(L"openxr_loader.dll");
   if(mod){wchar_t path[32768];if(GetModuleFileNameW(mod,path,32768))loader=path;FreeLibrary(mod);}
   if(loader.empty()){
    std::vector<fs::path> paths{root/L"bin/openxr_loader.dll",root/L"openxr_loader.dll"};
    wchar_t steam[32768];DWORD size=sizeof(steam);
    if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Valve\\Steam",L"SteamPath",RRF_RT_REG_SZ,nullptr,steam,&size)==ERROR_SUCCESS)
     paths.push_back(fs::path(steam)/L"steamapps/common/SteamVR/bin/win64/openxr_loader.dll");
    paths.push_back(L"C:/Program Files (x86)/Steam/steamapps/common/SteamVR/bin/win64/openxr_loader.dll");
    for(const auto& p:paths)if(fs::is_regular_file(p)){loader=fs::absolute(p).wstring();break;}
   }
   if(loader.empty())throw std::runtime_error("No Windows-compatible OpenXR loader was found. On Windows, install SteamVR. On Steam Frame, Proton must provide a working OpenXR bridge. This standalone path is experimental. Flat-screen play does not require OpenXR (Play Flat Screen.exe or --flat).");
  }
  if(!flat){
   HMODULE module=LoadLibraryW(loader.c_str());
   if(!module)throw std::runtime_error("OpenXR loader DLL could not load (Windows error "+std::to_string(GetLastError())+"). Keep the complete package together.");
   const bool valid=GetProcAddress(module,"xrGetInstanceProcAddr")!=nullptr;FreeLibrary(module);
   if(!valid)throw std::runtime_error("The selected OpenXR DLL is not an application loader: xrGetInstanceProcAddr is missing.");
  }
  // Clear inherited diagnostics while keeping the chosen OpenXR loader locally.
  LPWCH block=GetEnvironmentStringsW();std::vector<std::wstring> names;
  for(auto p=block;p&&*p;p+=wcslen(p)+1){std::wstring line=p;auto eq=line.find(L'=');if(line.rfind(L"LM_",0)==0&&eq!=std::wstring::npos)names.push_back(line.substr(0,eq));}
  if(block)FreeEnvironmentStringsW(block);for(const auto& name:names)SetEnvironmentVariableW(name.c_str(),nullptr);
  if(frameSafe)SetEnvironmentVariableW(L"LM_EFB_RAM",L"0");
  if(diagnostics)SetEnvironmentVariableW(L"LM_TRACE",L"ring");
  const auto profile=root/L"profile";
  for(auto p:{profile/L"appdata/Luigi's Mansion VR",profile/L"cards",profile/L"cache",profile/L"logs"})fs::create_directories(p);
  set(L"APPDATA",profile/L"appdata");set(L"LM_CARD_DIR",profile/L"cards");set(L"LM_CACHE_DIR",profile/L"cache");
  SetEnvironmentVariableW(L"LM_VR",flat?L"0":L"1");SetEnvironmentVariableW(L"LM_VR_FIRST_PERSON",L"0");SetEnvironmentVariableW(L"LM_VR_VIEW_MODE",flat?L"flat":L"diorama");
  if(!flat)SetEnvironmentVariableW(L"LM_OPENXR_LOADER",loader.c_str());
  if(check){std::ofstream report(root/L"launcher-check.txt");report<<"OpenXR loader: "<<(flat?std::string("not required"):fs::path(loader).string())<<"\n";report<<"Native EXE launcher: OK\nMode: "<<(flat?"Flat (no OpenXR required)":"Diorama")<<"\nDisc: selected by native setup; not required by launcher\n";return 0;}
  SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
  auto out=CreateFileW((profile/L"logs/launcher-output.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
  auto err=CreateFileW((profile/L"logs/launcher-errors.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
  STARTUPINFOW si{sizeof(si)};si.dwFlags=STARTF_USESTDHANDLES;si.hStdOutput=out;si.hStdError=err;si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);PROCESS_INFORMATION pi{};
  std::wstring command=L"\""+exe.wstring()+L"\"";
  BOOL ok=CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,0,nullptr,(root/L"bin").c_str(),&si,&pi);
  if(out!=INVALID_HANDLE_VALUE)CloseHandle(out);if(err!=INVALID_HANDLE_VALUE)CloseHandle(err);
  if(!ok)throw std::runtime_error("Could not launch the game executable. Check the Proton version or Windows runtime requirements.");
  CloseHandle(pi.hThread);WaitForSingleObject(pi.hProcess,INFINITE);DWORD code=0;GetExitCodeProcess(pi.hProcess,&code);CloseHandle(pi.hProcess);return int(code);
 }catch(const std::exception& e){MessageBoxA(nullptr,e.what(),"Luigi's Mansion VR launcher",MB_OK|MB_ICONERROR);return 1;}
}

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <iostream>
#include <iterator>

namespace fs = std::filesystem;

static std::wstring Quote(const std::wstring& value)
{
    std::wstring out=L"\"";
    size_t slashes=0;
    for(wchar_t ch:value)
    {
        if(ch==L'\\') { ++slashes; continue; }
        if(ch==L'\"') { out.append(slashes*2+1,L'\\'); out.push_back(L'\"'); slashes=0; continue; }
        out.append(slashes,L'\\'); slashes=0; out.push_back(ch);
    }
    out.append(slashes*2,L'\\'); out.push_back(L'\"');
    return out;
}

static fs::path ExecutableDirectory()
{
    std::vector<wchar_t> buffer(32768,L'\0');
    const DWORD n=GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
    if(n==0 || n>=buffer.size()) return fs::current_path();
    return fs::path(std::wstring(buffer.data(),n)).parent_path();
}

static bool ExistsFile(const fs::path& p)
{
    std::error_code ec; return fs::is_regular_file(p,ec);
}

static fs::path FindScript()
{
    const fs::path exe=ExecutableDirectory();
    const fs::path cwd=fs::current_path();
    const fs::path candidates[]={
        exe/L"editor"/L"local_editor_worker.py",
        exe/L"tools"/L"local_editor"/L"local_editor_worker.py",
        exe/L".."/L".."/L".."/L"tools"/L"local_editor"/L"local_editor_worker.py",
        cwd/L"tools"/L"local_editor"/L"local_editor_worker.py"
    };
    for(const auto& raw:candidates)
    {
        std::error_code ec; const auto p=fs::weakly_canonical(raw,ec);
        if(!ec && ExistsFile(p)) return p;
        if(ExistsFile(raw)) return raw;
    }
    return {};
}

static fs::path SearchExecutable(const wchar_t* name)
{
    wchar_t buffer[32768]{};
    const DWORD n=SearchPathW(nullptr,name,nullptr,static_cast<DWORD>(std::size(buffer)),buffer,nullptr);
    return n>0 && n<std::size(buffer) ? fs::path(buffer) : fs::path{};
}

static std::wstring JsonEscape(const std::wstring& value)
{
    std::wstring out;
    for(wchar_t c:value)
    {
        if(c==L'\\' || c==L'\"') { out.push_back(L'\\'); out.push_back(c); }
        else if(c==L'\r') out+=L"\\r";
        else if(c==L'\n') out+=L"\\n";
        else out.push_back(c);
    }
    return out;
}

static std::wstring Utf8ToWide(const std::string& value)
{
    if(value.empty()) return {};
    const int needed=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);
    if(needed<=0)
    {
        std::wstring fallback; fallback.reserve(value.size());
        for(unsigned char c:value) fallback.push_back(static_cast<wchar_t>(c));
        return fallback;
    }
    std::wstring out(static_cast<size_t>(needed),L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),out.data(),needed);
    return out;
}

static std::wstring ReadLogTail(const fs::path& path,size_t maxBytes=6000)
{
    std::ifstream f(path,std::ios::binary);
    if(!f) return {};
    f.seekg(0,std::ios::end);
    const auto end=f.tellg();
    if(end<=0) return {};
    const auto total=static_cast<size_t>(end);
    const size_t take=(std::min)(total,maxBytes);
    f.seekg(static_cast<std::streamoff>(total-take),std::ios::beg);
    std::string body(take,'\0');
    f.read(body.data(),static_cast<std::streamsize>(body.size()));
    body.resize(static_cast<size_t>(f.gcount()));
    std::wstring text=Utf8ToWide(body);
    while(!text.empty() && (text.back()==L'\r'||text.back()==L'\n'||text.back()==L' '||text.back()==L'\t')) text.pop_back();
    if(text.size()>5000) text=text.substr(text.size()-5000);
    return text;
}

static std::wstring ExitCodeText(DWORD code)
{
    std::wostringstream out;
    out << static_cast<unsigned long>(code) << L" (0x" << std::hex << std::uppercase << static_cast<unsigned long>(code) << L")";
    return out.str();
}

static void WriteFailure(const fs::path& jobPath,const wchar_t* error,const std::wstring& detail)
{
    if(jobPath.empty()) return;
    const fs::path result=jobPath.parent_path()/L"result.json";
    const fs::path temp=result.wstring()+L".tmp";
    std::wofstream f(temp,std::ios::trunc);
    if(!f) return;
    f << L"{\"schema\":2,\"ok\":false,\"error\":\"" << JsonEscape(error)
      << L"\",\"detail\":\"" << JsonEscape(detail) << L"\"}";
    f.flush(); const bool ok=f.good(); f.close();
    if(ok) MoveFileExW(temp.c_str(),result.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    else DeleteFileW(temp.c_str());
}

int wmain(int argc,wchar_t** argv)
{
    fs::path jobPath;
    bool doctor=false;
    for(int i=1;i<argc;++i)
    {
        if(std::wstring(argv[i])==L"--replaybuffer-edit-job" && i+1<argc) jobPath=fs::path(argv[i+1]);
        if(std::wstring(argv[i])==L"--replaybuffer-worker-doctor") doctor=true;
    }

    const fs::path script=FindScript();
    const fs::path pythonExe=SearchExecutable(L"python.exe");
    const fs::path pyLauncher=pythonExe.empty()?SearchExecutable(L"py.exe"):fs::path{};
    const fs::path python=!pythonExe.empty()?pythonExe:pyLauncher;
    if(doctor)
    {
        std::wcout << L"ReplayBuffer LocalEditorWorker bootstrap\n"
                   << L"script: " << (script.empty()?L"MISSING":script.wstring()) << L"\n"
                   << L"python: " << (python.empty()?L"MISSING":python.wstring()) << L"\n";
        if(script.empty() || python.empty()) return 2;
    }
    if(script.empty())
    {
        WriteFailure(jobPath,L"worker_script_missing",L"Could not find editor\\local_editor_worker.py. Package the ReplayBuffer editor runtime beside the worker.");
        return 3;
    }

    const fs::path exeDir=ExecutableDirectory();
    const fs::path localFfmpeg=exeDir/L"ffmpeg.exe";
    const fs::path localFfprobe=exeDir/L"ffprobe.exe";
    if(ExistsFile(localFfmpeg)) SetEnvironmentVariableW(L"REPLAYBUFFER_FFMPEG",localFfmpeg.c_str());
    if(ExistsFile(localFfprobe)) SetEnvironmentVariableW(L"REPLAYBUFFER_FFPROBE",localFfprobe.c_str());
    if(python.empty())
    {
        WriteFailure(jobPath,L"python_runtime_missing",L"Python 3 was not found. Install Python 3.10+ or package a standalone worker.");
        return 4;
    }

    std::wstring cmd=Quote(python.wstring());
    if(!pyLauncher.empty()) cmd+=L" -3";
    cmd+=L" -X faulthandler";
    cmd+=L" "+Quote(script.wstring());
    for(int i=1;i<argc;++i) cmd+=L" "+Quote(argv[i]);
    std::vector<wchar_t> mutableCmd(cmd.begin(),cmd.end()); mutableCmd.push_back(L'\0');
    STARTUPINFOW si{}; si.cb=sizeof(si); PROCESS_INFORMATION pi{};
    BOOL inheritHandles=FALSE;
    HANDLE runtimeLog=INVALID_HANDLE_VALUE;
    HANDLE runtimeInput=INVALID_HANDLE_VALUE;
    fs::path runtimeLogPath;
    if(doctor)
    {
        si.dwFlags=STARTF_USESTDHANDLES;
        si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError=GetStdHandle(STD_ERROR_HANDLE);
        inheritHandles=TRUE;
    }
    else if(!jobPath.empty())
    {
        runtimeLogPath=jobPath.parent_path()/L"worker-runtime.log";
        SECURITY_ATTRIBUTES sa{}; sa.nLength=sizeof(sa); sa.bInheritHandle=TRUE;
        runtimeLog=CreateFileW(runtimeLogPath.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,
                               CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(runtimeLog!=INVALID_HANDLE_VALUE)
        {
            runtimeInput=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            si.dwFlags=STARTF_USESTDHANDLES;
            si.hStdInput=(runtimeInput==INVALID_HANDLE_VALUE?runtimeLog:runtimeInput);
            si.hStdOutput=runtimeLog;
            si.hStdError=runtimeLog;
            inheritHandles=TRUE;
            SetEnvironmentVariableW(L"PYTHONUTF8",L"1");
            SetEnvironmentVariableW(L"PYTHONUNBUFFERED",L"1");
            SetEnvironmentVariableW(L"PYTHONFAULTHANDLER",L"1");
        }
    }
    const std::wstring workDir=script.parent_path().wstring();
    const BOOL ok=CreateProcessW(python.c_str(),mutableCmd.data(),nullptr,nullptr,inheritHandles,
        CREATE_NO_WINDOW|BELOW_NORMAL_PRIORITY_CLASS,nullptr,workDir.c_str(),&si,&pi);
    if(runtimeInput!=INVALID_HANDLE_VALUE) { CloseHandle(runtimeInput); runtimeInput=INVALID_HANDLE_VALUE; }
    if(!ok)
    {
        const DWORD win32=GetLastError();
        if(runtimeLog!=INVALID_HANDLE_VALUE) CloseHandle(runtimeLog);
        WriteFailure(jobPath,L"python_launch_failed",L"The bootstrap found Python but Windows could not launch the local editor script. Win32 error="+std::to_wstring(win32));
        return 5;
    }
    WaitForSingleObject(pi.hProcess,INFINITE);
    DWORD exitCode=1; GetExitCodeProcess(pi.hProcess,&exitCode);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if(runtimeLog!=INVALID_HANDLE_VALUE) { FlushFileBuffers(runtimeLog); CloseHandle(runtimeLog); }
    if(exitCode!=0 && !jobPath.empty() && !ExistsFile(jobPath.parent_path()/L"result.json"))
    {
        std::wstring detail=L"The Python editor exited before producing result.json. Exit code="+ExitCodeText(exitCode)+L".";
        const std::wstring tail=ReadLogTail(runtimeLogPath);
        if(!tail.empty()) detail+=L" Python output: "+tail;
        else detail+=L" Run LocalEditorWorker.exe --replaybuffer-worker-doctor for the concrete runtime check.";
        WriteFailure(jobPath,L"worker_runtime_failed",detail);
    }
    return static_cast<int>(exitCode);
}

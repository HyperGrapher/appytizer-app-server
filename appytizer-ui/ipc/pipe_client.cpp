// FLTK is not thread-safe: pipe I/O happens in a worker, and every result is marshaled with Fl::awake.
#include "appytizer-ui/ipc/pipe_client.hpp"
#include "common/constants.hpp"
#include "common/win_handle.hpp"
#include <FL/Fl.H>
#include <windows.h>
#include <nlohmann/json.hpp>
#include <array>
#include <memory>
#include <thread>

namespace appytizer {
void PipeClient::request(std::string command,std::string params_json,Callback callback){std::thread([this,command=std::move(command),params_json=std::move(params_json),callback=std::move(callback)]()mutable{static std::atomic_uint64_t next{1};nlohmann::json params=nlohmann::json::object();try{params=nlohmann::json::parse(params_json);}catch(...){}const auto message=nlohmann::json{{"id",std::to_string(next++)},{"cmd",command},{"params",params}}.dump()+"\n";WinHandle pipe(CreateFileW(kPipeName,GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr));std::string answer;if(pipe){connected_=true;DWORD written{};WriteFile(pipe.get(),message.data(),static_cast<DWORD>(message.size()),&written,nullptr);std::array<char,65536> buffer{};DWORD read{};if(ReadFile(pipe.get(),buffer.data(),static_cast<DWORD>(buffer.size()),&read,nullptr))answer.assign(buffer.data(),read);}else{connected_=false;answer=R"({"ok":false,"error":"Engine is not connected"})";}auto* task=new std::pair<Callback,std::string>(std::move(callback),std::move(answer));Fl::awake([](void* data){std::unique_ptr<std::pair<Callback,std::string>> owned(static_cast<std::pair<Callback,std::string>*>(data));owned->first(std::move(owned->second));},task);}).detach();}
} // namespace appytizer

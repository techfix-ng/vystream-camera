#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <obs-module.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shlobj.h>
#include <iphlpapi.h>
#include <mmsystem.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <mutex>
#include <vector>
#include <algorithm>
#include "qr_v4.h"

struct obs_module; struct obs_data; struct obs_source; struct obs_scene; struct obs_sceneitem;
static obs_module *g_module = nullptr;
static std::atomic<bool> g_running{false};
static SOCKET g_socket = INVALID_SOCKET;
static std::thread g_worker;
static std::thread g_network_worker;
static std::atomic<bool> g_menu_registered{false};
static std::string g_pairing_payload;
static std::string g_cached_wifi_ip;
static std::mutex g_network_mutex;
static VystrmQrV4 g_pairing_qr;
struct TalkbackTarget { std::string id,name,ip,token; int port=46010; };
static std::mutex g_talkback_mutex;
static std::vector<TalkbackTarget> g_talkback_targets;
static int g_talkback_selected=-1;
static HWAVEIN g_wave_in=nullptr;
static WAVEHDR g_wave_headers[4]{};
static std::vector<std::vector<char>> g_wave_buffers(4,std::vector<char>(640));
static std::atomic<bool> g_ptt{false};
static void write_load_status(const char *message);
static SOCKET g_return_socket=INVALID_SOCKET;
static std::thread g_return_worker;
static HWAVEOUT g_wave_out=nullptr;
static WAVEHDR g_return_headers[24]{};
static char g_return_audio[24][960]{};
static int g_return_slot=0;
#ifdef VYSTRM_QT_DOCK
extern "C" void vystrm_register_dock(void);
#endif

static void send_talkback_pcm(const char *pcm,int length) {
  std::lock_guard<std::mutex> lock(g_talkback_mutex);
  for(size_t i=0;i<g_talkback_targets.size();++i){if(g_talkback_selected>=0&&static_cast<int>(i)!=g_talkback_selected)continue;const auto&t=g_talkback_targets[i];
    std::string header="VYSTB1|"+t.token+"|";std::vector<char> packet(header.begin(),header.end());packet.insert(packet.end(),pcm,pcm+length);
    sockaddr_in to{};to.sin_family=AF_INET;to.sin_port=htons(static_cast<u_short>(t.port));if(InetPtonA(AF_INET,t.ip.c_str(),&to.sin_addr)==1)sendto(g_socket,packet.data(),static_cast<int>(packet.size()),0,reinterpret_cast<sockaddr*>(&to),sizeof(to));
  }
}
static void CALLBACK wave_callback(HWAVEIN in,UINT msg,DWORD_PTR,DWORD_PTR p1,DWORD_PTR){if(msg!=WIM_DATA)return;auto*h=reinterpret_cast<WAVEHDR*>(p1);if(g_ptt&&h->dwBytesRecorded)send_talkback_pcm(h->lpData,static_cast<int>(h->dwBytesRecorded));h->dwBytesRecorded=0;if(g_ptt)waveInAddBuffer(in,h,sizeof(*h));}
static bool start_ptt(){if(g_ptt.exchange(true))return true;WAVEFORMATEX f{};f.wFormatTag=WAVE_FORMAT_PCM;f.nChannels=1;f.nSamplesPerSec=16000;f.wBitsPerSample=16;f.nBlockAlign=2;f.nAvgBytesPerSec=32000;
  if(waveInOpen(&g_wave_in,WAVE_MAPPER,&f,reinterpret_cast<DWORD_PTR>(wave_callback),0,CALLBACK_FUNCTION)!=MMSYSERR_NOERROR){g_ptt=false;return false;}
  for(int i=0;i<4;i++){g_wave_headers[i]={};g_wave_headers[i].lpData=g_wave_buffers[i].data();g_wave_headers[i].dwBufferLength=640;waveInPrepareHeader(g_wave_in,&g_wave_headers[i],sizeof(WAVEHDR));waveInAddBuffer(g_wave_in,&g_wave_headers[i],sizeof(WAVEHDR));}waveInStart(g_wave_in);return true;}
static void stop_ptt(){if(!g_ptt.exchange(false))return;if(!g_wave_in)return;waveInStop(g_wave_in);waveInReset(g_wave_in);for(auto &h:g_wave_headers)waveInUnprepareHeader(g_wave_in,&h,sizeof(h));waveInClose(g_wave_in);g_wave_in=nullptr;}
static bool reply_is_selected(const std::string& token){std::lock_guard<std::mutex>lock(g_talkback_mutex);for(size_t i=0;i<g_talkback_targets.size();++i)if(g_talkback_targets[i].token==token)return g_talkback_selected<0||g_talkback_selected==static_cast<int>(i);return false;}
static void relay_crew_reply(const std::string&sender_token,const char*pcm,int length){std::lock_guard<std::mutex>lock(g_talkback_mutex);auto sender=std::find_if(g_talkback_targets.begin(),g_talkback_targets.end(),[&](const TalkbackTarget&t){return t.token==sender_token;});if(sender==g_talkback_targets.end())return;for(const auto&recipient:g_talkback_targets){if(recipient.token==sender_token)continue;std::string header="VYSTB2|"+recipient.token+"|"+sender->name+"|";std::vector<char>packet(header.begin(),header.end());packet.insert(packet.end(),pcm,pcm+length);sockaddr_in to{};to.sin_family=AF_INET;to.sin_port=htons(static_cast<u_short>(recipient.port));if(InetPtonA(AF_INET,recipient.ip.c_str(),&to.sin_addr)==1)sendto(g_socket,packet.data(),static_cast<int>(packet.size()),0,reinterpret_cast<sockaddr*>(&to),sizeof(to));}}
static void play_reply(const char*data,int length){if(!g_wave_out||length<=0)return;int slot=g_return_slot++%24;WAVEHDR &h=g_return_headers[slot];if(h.dwFlags&WHDR_PREPARED){while(!(h.dwFlags&WHDR_DONE)&&g_running)Sleep(2);waveOutUnprepareHeader(g_wave_out,&h,sizeof(h));}int count=std::min(length,960);std::memcpy(g_return_audio[slot],data,count);h={};h.lpData=g_return_audio[slot];h.dwBufferLength=count;if(waveOutPrepareHeader(g_wave_out,&h,sizeof(h))==MMSYSERR_NOERROR)waveOutWrite(g_wave_out,&h,sizeof(h));}
static void return_audio_loop(){char b[1500]{};while(g_running){sockaddr_in from{};int z=sizeof(from);int n=recvfrom(g_return_socket,b,sizeof(b),0,reinterpret_cast<sockaddr*>(&from),&z);if(n==SOCKET_ERROR){if(!g_running)break;continue;}if(n<10||std::memcmp(b,"VYSUP1|",7)!=0)continue;int end=7;while(end<n&&b[end]!='|')end++;if(end>=n)continue;std::string token(b+7,b+end);relay_crew_reply(token,b+end+1,n-end-1);if(reply_is_selected(token))play_reply(b+end+1,n-end-1);}}
static void start_return_listener(){g_return_socket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(g_return_socket==INVALID_SOCKET){write_load_status("ERROR: reply socket creation failed");return;}BOOL reuse=TRUE;DWORD timeout=500;setsockopt(g_return_socket,SOL_SOCKET,SO_REUSEADDR,reinterpret_cast<const char*>(&reuse),sizeof(reuse));setsockopt(g_return_socket,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_ANY);a.sin_port=htons(46011);if(bind(g_return_socket,reinterpret_cast<sockaddr*>(&a),sizeof(a))==SOCKET_ERROR){write_load_status("ERROR: UDP 46011 reply listener bind failed");closesocket(g_return_socket);g_return_socket=INVALID_SOCKET;return;}WAVEFORMATEX f{};f.wFormatTag=WAVE_FORMAT_PCM;f.nChannels=1;f.nSamplesPerSec=16000;f.wBitsPerSample=16;f.nBlockAlign=2;f.nAvgBytesPerSec=32000;if(waveOutOpen(&g_wave_out,WAVE_MAPPER,&f,0,0,CALLBACK_NULL)!=MMSYSERR_NOERROR){write_load_status("ERROR: Windows reply audio output could not open");closesocket(g_return_socket);g_return_socket=INVALID_SOCKET;return;}write_load_status("UDP 46011 camera reply listener started");g_return_worker=std::thread(return_audio_loop);}
static void stop_return_listener(){if(g_return_socket!=INVALID_SOCKET){closesocket(g_return_socket);g_return_socket=INVALID_SOCKET;}if(g_return_worker.joinable())g_return_worker.join();if(g_wave_out){waveOutReset(g_wave_out);for(auto &h:g_return_headers)if(h.dwFlags&WHDR_PREPARED)waveOutUnprepareHeader(g_wave_out,&h,sizeof(h));waveOutClose(g_wave_out);g_wave_out=nullptr;}}

template <typename T> static T symbol(HMODULE m, const char *n) { return m ? reinterpret_cast<T>(GetProcAddress(m, n)) : nullptr; }
static HMODULE obs_handle() { HMODULE m = GetModuleHandleW(L"obs.dll"); return m ? m : GetModuleHandleW(L"libobs.dll"); }
static std::string clean(std::string s) { for (char &c : s) if (c == '|' || c == '\r' || c == '\n') c = '-'; return s; }
static std::vector<std::string> split(const std::string &s) {
  std::vector<std::string> v; size_t b = 0;
  while (b <= s.size()) { size_t e = s.find('|', b); v.push_back(s.substr(b, e == std::string::npos ? e : e - b)); if (e == std::string::npos) break; b = e + 1; }
  return v;
}
static std::string computer_name() { char n[MAX_COMPUTERNAME_LENGTH + 1]{}; DWORD z = sizeof(n); return GetComputerNameA(n, &z) ? clean(std::string(n, z)) : "OBS-PC"; }
static std::string wifi_ip() {
  ULONG size=15000;std::vector<unsigned char> storage(size);auto *list=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
  ULONG rc=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,list,&size);
  if(rc==ERROR_BUFFER_OVERFLOW){storage.resize(size);list=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());rc=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,list,&size);}
  if(rc!=NO_ERROR)return {};
  for(auto *a=list;a;a=a->Next){if(a->IfType!=IF_TYPE_IEEE80211||a->OperStatus!=IfOperStatusUp)continue;for(auto*u=a->FirstUnicastAddress;u;u=u->Next){auto*sa=reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);if(!sa||sa->sin_family!=AF_INET)continue;char text[INET_ADDRSTRLEN]{};if(InetNtopA(AF_INET,&sa->sin_addr,text,sizeof(text))&&std::strncmp(text,"169.254.",8)!=0)return text;}}
  return {};
}
static void update_cached_wifi_ip(){std::string value=wifi_ip();std::lock_guard<std::mutex>lock(g_network_mutex);g_cached_wifi_ip=value;}
static std::string cached_wifi_ip(){std::lock_guard<std::mutex>lock(g_network_mutex);return g_cached_wifi_ip;}
static void network_monitor_loop(){while(g_running){update_cached_wifi_ip();for(int i=0;i<100&&g_running;i++)Sleep(100);}}
static std::string token() {
  unsigned char b[8]{}; using Random = BOOLEAN (WINAPI *)(PVOID, ULONG);
  if (auto r = symbol<Random>(GetModuleHandleW(L"advapi32.dll"), "SystemFunction036")) r(b, sizeof(b));
  char out[17]{}; for (int i = 0; i < 8; ++i) std::snprintf(out + i * 2, 3, "%02x", b[i]); return out;
}
static std::string ini_path() {
  char app[MAX_PATH]{}; if (FAILED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, app))) return "obs-srt-camera-devices.ini";
  std::string p = std::string(app) + "\\obs-studio"; CreateDirectoryA(p.c_str(), nullptr); p += "\\plugin_config"; CreateDirectoryA(p.c_str(), nullptr);
  p += "\\obs-srt-camera"; CreateDirectoryA(p.c_str(), nullptr); return p + "\\devices.ini";
}
static void write_load_status(const char *message) {
  std::string path=ini_path();size_t slash=path.find_last_of("\\/");if(slash!=std::string::npos)path=path.substr(0,slash+1)+"load-status.txt";
  HANDLE f=CreateFileA(path.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(f==INVALID_HANDLE_VALUE)return;
  SYSTEMTIME t{};GetLocalTime(&t);char line[512]{};int n=std::snprintf(line,sizeof(line),"%04u-%02u-%02u %02u:%02u:%02u  %s\r\n",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,message);DWORD written=0;if(n>0)WriteFile(f,line,static_cast<DWORD>(n),&written,nullptr);CloseHandle(f);
}
static std::string read_value(const std::string &id, const char *key) { char v[256]{}; GetPrivateProfileStringA(id.c_str(), key, "", v, sizeof(v), ini_path().c_str()); return v; }
static std::string desktop_token(){std::string value=read_value("__vystrm_desktop__","token");if(value.empty()){value=token();WritePrivateProfileStringA("__vystrm_desktop__","token",value.c_str(),ini_path().c_str());}return value;}
static bool unique_value(const char *key,const std::string &name, const std::string &id) {
  char sections[8192]{}; GetPrivateProfileSectionNamesA(sections, sizeof(sections), ini_path().c_str());
  for (const char *s = sections; *s; s += std::strlen(s) + 1) { if (id == s) continue; char v[256]{}; GetPrivateProfileStringA(s, key, "", v, sizeof(v), ini_path().c_str()); if (_stricmp(v, name.c_str()) == 0) return false; }
  return true;
}

struct Prompt { std::string id,scene_default,source_default,scene_result,source_result;HWND scene_edit=nullptr,source_edit=nullptr;bool accepted=false; };
static LRESULT CALLBACK prompt_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  auto *p = reinterpret_cast<Prompt *>(GetWindowLongPtrA(w, GWLP_USERDATA));
  if (msg == WM_CREATE) {
    p = reinterpret_cast<Prompt *>(reinterpret_cast<CREATESTRUCTA *>(lp)->lpCreateParams); SetWindowLongPtrA(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(p));
    CreateWindowA("STATIC","A new VYSTRM camera was detected. Choose permanent OBS names:",WS_CHILD|WS_VISIBLE,18,16,444,25,w,nullptr,nullptr,nullptr);
    CreateWindowA("STATIC","Scene name",WS_CHILD|WS_VISIBLE,18,52,110,22,w,nullptr,nullptr,nullptr);
    p->scene_edit=CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT",p->scene_default.c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,132,48,310,28,w,reinterpret_cast<HMENU>(1001),nullptr,nullptr);
    CreateWindowA("STATIC","Source name",WS_CHILD|WS_VISIBLE,18,91,110,22,w,nullptr,nullptr,nullptr);
    p->source_edit=CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT",p->source_default.c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,132,87,310,28,w,reinterpret_cast<HMENU>(1002),nullptr,nullptr);
    CreateWindowA("STATIC","The source will be added only inside the new scene.",WS_CHILD|WS_VISIBLE,18,128,424,22,w,nullptr,nullptr,nullptr);
    CreateWindowA("BUTTON","Create scene and source",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,250,158,192,34,w,reinterpret_cast<HMENU>(IDOK),nullptr,nullptr);
    SetFocus(p->scene_edit);SendMessageA(p->scene_edit,EM_SETSEL,0,-1);return 0;
  }
  if (msg == WM_COMMAND && LOWORD(wp) == IDOK && p) {
    char sv[129]{},cv[129]{};GetWindowTextA(p->scene_edit,sv,sizeof(sv));GetWindowTextA(p->source_edit,cv,sizeof(cv));std::string scene=clean(sv),source=clean(cv);
    if(scene.empty()||source.empty())MessageBoxA(w,"Enter both a scene name and a source name.","Names required",MB_OK|MB_ICONWARNING);
    else if(_stricmp(scene.c_str(),source.c_str())==0)MessageBoxA(w,"Scene and source names must be different.","Names must differ",MB_OK|MB_ICONWARNING);
    else if(!unique_value("scene_name",scene,p->id)||!unique_value("source_name",source,p->id))MessageBoxA(w,"That scene or source name is already assigned to another camera.","Name already used",MB_OK|MB_ICONWARNING);
    else{p->scene_result=scene;p->source_result=source;p->accepted=true;DestroyWindow(w);}return 0;
  }
  if (msg == WM_CLOSE) { DestroyWindow(w); return 0; }
  if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcA(w,msg,wp,lp);
}
static bool ask_names(const std::string&id,const std::string&suggested,std::string&scene,std::string&source) {
  std::string base=suggested.empty()?"VYSTRM Camera":suggested;Prompt p{id,base+" Scene",base+"-Cam"};WNDCLASSA c{};c.lpfnWndProc=prompt_proc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName="VystrmSceneSourcePrompt";c.hCursor=LoadCursor(nullptr,IDC_ARROW);c.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);RegisterClassA(&c);
  HWND w=CreateWindowExA(WS_EX_TOPMOST,c.lpszClassName,"Set up VYSTRM camera in OBS",WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,478,245,nullptr,nullptr,c.hInstance,&p);if(!w)return false;
  RECT r{}; GetWindowRect(w,&r); SetWindowPos(w,HWND_TOPMOST,(GetSystemMetrics(SM_CXSCREEN)-(r.right-r.left))/2,(GetSystemMetrics(SM_CYSCREEN)-(r.bottom-r.top))/2,0,0,SWP_NOSIZE|SWP_SHOWWINDOW);
  MSG m{};while(GetMessageA(&m,nullptr,0,0)>0){if(!IsDialogMessageA(w,&m)){TranslateMessage(&m);DispatchMessageA(&m);}}if(!p.accepted)return false;scene=p.scene_result;source=p.source_result;return true;
}
static int next_port() {
  int high=8999; char sections[8192]{}; GetPrivateProfileSectionNamesA(sections,sizeof(sections),ini_path().c_str());
  for(const char *s=sections;*s;s+=std::strlen(s)+1){int p=GetPrivateProfileIntA(s,"port",8999,ini_path().c_str()); if(p>high)high=p;} return high+1;
}

struct SourceRequest { std::string scene_name,source_name;int port; };
static void create_source(void *param) {
  SourceRequest req=*static_cast<SourceRequest*>(param); delete static_cast<SourceRequest*>(param); HMODULE obs=obs_handle(), front=GetModuleHandleW(L"obs-frontend-api.dll");
  using DC=obs_data*(*)(); using DS=void(*)(obs_data*,const char*,const char*); using DB=void(*)(obs_data*,const char*,bool); using DR=void(*)(obs_data*);
  using SC=obs_source*(*)(const char*,const char*,obs_data*,obs_data*); using SBN=obs_source*(*)(const char*); using SR=void(*)(obs_source*); using SFS=obs_scene*(*)(const obs_source*); using SA=obs_sceneitem*(*)(obs_scene*,obs_source*); using SF=obs_sceneitem*(*)(obs_scene*,const char*); using SNC=obs_scene*(*)(const char*); using SNR=void(*)(obs_scene*);
  auto dc=symbol<DC>(obs,"obs_data_create"); auto ds=symbol<DS>(obs,"obs_data_set_string"); auto db=symbol<DB>(obs,"obs_data_set_bool"); auto dr=symbol<DR>(obs,"obs_data_release"); auto sc=symbol<SC>(obs,"obs_source_create"); auto sbn=symbol<SBN>(obs,"obs_get_source_by_name"); auto sr=symbol<SR>(obs,"obs_source_release"); auto sfs=symbol<SFS>(obs,"obs_scene_from_source"); auto sa=symbol<SA>(obs,"obs_scene_add"); auto sf=symbol<SF>(obs,"obs_scene_find_source"); auto snc=symbol<SNC>(obs,"obs_scene_create"); auto snr=symbol<SNR>(obs,"obs_scene_release");
  if(!dc||!ds||!db||!dr||!sc||!sbn||!sr||!sfs||!sa||!snc||!snr)return;
  const std::string &scene_name=req.scene_name,&source_name=req.source_name;
  obs_source *scene_source=sbn(scene_name.c_str()); obs_scene *scene=scene_source?sfs(scene_source):snc(scene_name.c_str()); if(!scene){if(scene_source)sr(scene_source);return;}
  obs_source *phone=sbn(source_name.c_str()); if(!phone){char input[256]{};std::snprintf(input,sizeof(input),"srt://0.0.0.0:%d?mode=listener&latency=200000",req.port);obs_data *d=dc();db(d,"is_local_file",false);ds(d,"input",input);ds(d,"input_format","mpegts");db(d,"restart_on_activate",true);db(d,"close_when_inactive",false);phone=sc("ffmpeg_source",source_name.c_str(),d,nullptr);dr(d);}
  if(phone&&(!sf||!sf(scene,source_name.c_str())))sa(scene,phone);if(phone)sr(phone);if(scene_source)sr(scene_source);else snr(scene);using Save=void(*)();if(auto save=symbol<Save>(front,"obs_frontend_save"))save();
}
static void queue_source(const std::string&scene,const std::string&source,int port){using Q=void(*)(int,void(*)(void*),void*,bool);if(auto q=symbol<Q>(obs_handle(),"obs_queue_task"))q(0,create_source,new SourceRequest{scene,source,port},false);}
static void restore_saved_sources(){char sections[8192]{};GetPrivateProfileSectionNamesA(sections,sizeof(sections),ini_path().c_str());for(const char*s=sections;*s;s+=std::strlen(s)+1){if(std::strcmp(s,"__vystrm_desktop__")==0)continue;std::string scene=read_value(s,"scene_name"),source=read_value(s,"source_name");int port=GetPrivateProfileIntA(s,"port",0,ini_path().c_str());if(!scene.empty()&&!source.empty()&&port>=9000)queue_source(scene,source,port);}}

extern "C" const char *vystrm_pairing_payload(void){std::string ip=cached_wifi_ip();if(ip.empty()){g_pairing_payload.clear();return "";}g_pairing_payload="vys://p?i="+ip+"&p=9000&t="+desktop_token()+"&n="+clean("VYSTRM-"+computer_name());return g_pairing_payload.c_str();}
extern "C" int vystrm_camera_count(void){std::lock_guard<std::mutex>lock(g_talkback_mutex);return static_cast<int>(g_talkback_targets.size());}
extern "C" const char *vystrm_camera_name(int index){static thread_local std::string value;std::lock_guard<std::mutex>lock(g_talkback_mutex);value=(index>=0&&index<(int)g_talkback_targets.size())?g_talkback_targets[index].name:"";return value.c_str();}
extern "C" void vystrm_select_camera(int index){std::lock_guard<std::mutex>lock(g_talkback_mutex);g_talkback_selected=index;}
extern "C" bool vystrm_talkback_start(void){return start_ptt();}
extern "C" void vystrm_talkback_stop(void){stop_ptt();}

static LRESULT CALLBACK qr_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp){
  if(msg==WM_PAINT){PAINTSTRUCT ps{};HDC dc=BeginPaint(w,&ps);RECT r{};GetClientRect(w,&r);FillRect(dc,&r,reinterpret_cast<HBRUSH>(COLOR_WINDOW+1));int quiet=4,scale=static_cast<int>(std::max<LONG>(5,std::min<LONG>((r.right-40)/(VystrmQrV4::size+quiet*2),(r.bottom-105)/(VystrmQrV4::size+quiet*2))));int side=(VystrmQrV4::size+quiet*2)*scale,ox=(r.right-side)/2,oy=18;HBRUSH black=CreateSolidBrush(RGB(0,0,0));for(int y=0;y<VystrmQrV4::size;y++)for(int x=0;x<VystrmQrV4::size;x++)if(g_pairing_qr.module(x,y)){RECT q{ox+(x+quiet)*scale,oy+(y+quiet)*scale,ox+(x+quiet+1)*scale,oy+(y+quiet+1)*scale};FillRect(dc,&q,black);}DeleteObject(black);SetBkMode(dc,TRANSPARENT);SetTextAlign(dc,TA_CENTER);TextOutA(dc,r.right/2,oy+side+8,"Scan with VYSTRM Camera",23);TextOutA(dc,r.right/2,oy+side+30,"Wi-Fi pairing only",18);EndPaint(w,&ps);return 0;}
  if(msg==WM_CLOSE){DestroyWindow(w);return 0;}if(msg==WM_DESTROY){PostQuitMessage(0);return 0;}return DefWindowProcA(w,msg,wp,lp);
}
static void show_pairing(void*){std::string ip=wifi_ip();if(ip.empty()){MessageBoxA(nullptr,"No active Wi-Fi IPv4 address was found. Connect this PC to Wi-Fi, then open pairing again.","VYSTRM Camera",MB_OK|MB_ICONWARNING);return;}std::string name="VYSTRM-"+computer_name();g_pairing_payload="vys://p?i="+ip+"&p=9000&t="+desktop_token()+"&n="+clean(name);if(!g_pairing_qr.encode(g_pairing_payload)){MessageBoxA(nullptr,"Pairing data is too long.","VYSTRM Camera",MB_OK|MB_ICONERROR);return;}WNDCLASSA c{};c.lpfnWndProc=qr_proc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName="VystrmPairingQr";c.hCursor=LoadCursor(nullptr,IDC_ARROW);RegisterClassA(&c);HWND w=CreateWindowExA(WS_EX_TOPMOST,c.lpszClassName,"VYSTRM Camera Pairing",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,510,590,nullptr,nullptr,c.hInstance,nullptr);ShowWindow(w,SW_SHOW);UpdateWindow(w);MSG m{};while(GetMessageA(&m,nullptr,0,0)>0){TranslateMessage(&m);DispatchMessageA(&m);}}
static HWND g_talkback_combo=nullptr,g_talkback_button=nullptr;static WNDPROC g_old_button_proc=nullptr;
static LRESULT CALLBACK ptt_button_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp){if(msg==WM_LBUTTONDOWN){SetCapture(w);SetWindowTextA(w,"TALKING — RELEASE TO STOP");if(!start_ptt())MessageBoxA(w,"Could not open the selected Windows microphone.","VyStream Talkback",MB_OK|MB_ICONERROR);return 0;}if(msg==WM_LBUTTONUP||msg==WM_CAPTURECHANGED){ReleaseCapture();stop_ptt();SetWindowTextA(w,"HOLD TO TALK");return 0;}return CallWindowProcA(g_old_button_proc,w,msg,wp,lp);}
static LRESULT CALLBACK talkback_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp){
  if(msg==WM_CREATE){CreateWindowA("STATIC","Send director talkback to:",WS_CHILD|WS_VISIBLE,20,18,300,22,w,nullptr,nullptr,nullptr);g_talkback_combo=CreateWindowA("COMBOBOX","",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST,20,44,360,240,w,reinterpret_cast<HMENU>(2001),nullptr,nullptr);SendMessageA(g_talkback_combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>("All paired cameras"));{std::lock_guard<std::mutex>lock(g_talkback_mutex);for(auto&t:g_talkback_targets)SendMessageA(g_talkback_combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(t.name.c_str()));}SendMessageA(g_talkback_combo,CB_SETCURSEL,0,0);g_talkback_button=CreateWindowA("BUTTON","HOLD TO TALK",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,20,92,360,78,w,reinterpret_cast<HMENU>(2002),nullptr,nullptr);g_old_button_proc=reinterpret_cast<WNDPROC>(SetWindowLongPtrA(g_talkback_button,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(ptt_button_proc)));CreateWindowA("STATIC","Camera replies play through the default Windows output. Use headsets to prevent feedback.",WS_CHILD|WS_VISIBLE,20,183,370,42,w,nullptr,nullptr,nullptr);return 0;}
  if(msg==WM_COMMAND&&LOWORD(wp)==2001&&HIWORD(wp)==CBN_SELCHANGE){int choice=static_cast<int>(SendMessageA(g_talkback_combo,CB_GETCURSEL,0,0));g_talkback_selected=choice<=0?-1:choice-1;return 0;}
  if(msg==WM_CLOSE){stop_ptt();DestroyWindow(w);return 0;}if(msg==WM_DESTROY){PostQuitMessage(0);return 0;}return DefWindowProcA(w,msg,wp,lp);
}
static void show_talkback(void*){WNDCLASSA c{};c.lpfnWndProc=talkback_proc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName="VystrmTalkbackPanel";c.hCursor=LoadCursor(nullptr,IDC_ARROW);c.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);RegisterClassA(&c);HWND w=CreateWindowExA(WS_EX_TOPMOST,c.lpszClassName,"VyStream Director Talkback",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,420,275,nullptr,nullptr,c.hInstance,nullptr);ShowWindow(w,SW_SHOW);UpdateWindow(w);MSG m{};while(GetMessageA(&m,nullptr,0,0)>0){TranslateMessage(&m);DispatchMessageA(&m);}}
static void register_tools_menu(void*){
  if(g_menu_registered.exchange(true))return;
#ifdef VYSTRM_QT_DOCK
  vystrm_register_dock();
  write_load_status("VyStream native OBS dock registered");
#else
  HMODULE front=GetModuleHandleW(L"obs-frontend-api.dll");if(!front)front=LoadLibraryW(L"obs-frontend-api.dll");using Add=void(*)(const char*,void(*)(void*),void*);if(auto add=symbol<Add>(front,"obs_frontend_add_tools_menu_item")){add("VyStream Camera Pairing",show_pairing,nullptr);add("VyStream Director Talkback",show_talkback,nullptr);write_load_status("Tools menu registered");}else{g_menu_registered=false;write_load_status("ERROR: obs_frontend_add_tools_menu_item was not found");}
#endif
}
static void queue_menu_registration(){using Q=void(*)(int,void(*)(void*),void*,bool);if(auto q=symbol<Q>(obs_handle(),"obs_queue_task"))q(0,register_tools_menu,nullptr,false);else register_tools_menu(nullptr);}

static void discovery_loop() {
  char b[1024]{}; while(g_running){sockaddr_in sender{};int z=sizeof(sender);int n=recvfrom(g_socket,b,sizeof(b)-1,0,reinterpret_cast<sockaddr*>(&sender),&z);if(n==SOCKET_ERROR){if(!g_running)break;continue;}b[n]=0;auto f=split(std::string(b,n));if(f.size()<3||(f[0]!="OBS_SRT_DISCOVER_V3"&&f[0]!="OBS_SRT_DISCOVER_V4"))continue;
    std::string id=clean(f[1]),suggested=clean(f[2]),scene=read_value(id,"scene_name"),source=read_value(id,"source_name"),pairing=read_value(id,"token");int port=GetPrivateProfileIntA(id.c_str(),"port",0,ini_path().c_str());
    if(scene.empty()||source.empty()){if(!ask_names(id,suggested,scene,source))continue;port=next_port();pairing=token();WritePrivateProfileStringA(id.c_str(),"scene_name",scene.c_str(),ini_path().c_str());WritePrivateProfileStringA(id.c_str(),"source_name",source.c_str(),ini_path().c_str());WritePrivateProfileStringA(id.c_str(),"port",std::to_string(port).c_str(),ini_path().c_str());WritePrivateProfileStringA(id.c_str(),"token",pairing.c_str(),ini_path().c_str());}
    if(port<9000){port=next_port();WritePrivateProfileStringA(id.c_str(),"port",std::to_string(port).c_str(),ini_path().c_str());}if(pairing.empty()){pairing=token();WritePrivateProfileStringA(id.c_str(),"token",pairing.c_str(),ini_path().c_str());}
    std::string ps=std::to_string(port),ip=cached_wifi_ip();if(ip.empty()){update_cached_wifi_ip();ip=cached_wifi_ip();}if(ip.empty())continue;
    if(f[0]=="OBS_SRT_DISCOVER_V4"){char phone_ip[INET_ADDRSTRLEN]{};InetNtopA(AF_INET,&sender.sin_addr,phone_ip,sizeof(phone_ip));int talkback_port=f.size()>3?std::atoi(f[3].c_str()):46010;if(talkback_port<1024||talkback_port>65535)talkback_port=46010;std::lock_guard<std::mutex>lock(g_talkback_mutex);auto it=std::find_if(g_talkback_targets.begin(),g_talkback_targets.end(),[&](const TalkbackTarget&t){return t.id==id;});TalkbackTarget target{id,source,phone_ip,pairing,talkback_port};if(it==g_talkback_targets.end())g_talkback_targets.push_back(target);else *it=target;}
    // 0.0.0.0 remains the correct listener bind address inside OBS. The offer
    // always advertises the routable adapter address selected for this phone.
    std::string offer="OBS_SRT_OFFER_V3|"+computer_name()+"|"+ip+"|"+ps+"|"+pairing+"|"+clean(source);sendto(g_socket,offer.c_str(),static_cast<int>(offer.size()),0,reinterpret_cast<const sockaddr*>(&sender),z);queue_source(scene,source,port);
  }
}

extern "C" __declspec(dllexport) void obs_module_set_pointer(obs_module *m){g_module=m;}
// OBS validates this export before calling obs_module_load(). It must report
// the libobs module ABI used at compile time, not the OBS application version.
extern "C" __declspec(dllexport) uint32_t obs_module_ver(){return LIBOBS_API_VER;}
extern "C" __declspec(dllexport) const char *obs_module_name(){return "VyStream Camera 2.9.0";}
extern "C" __declspec(dllexport) const char *obs_module_description(){return "VyStream Camera discovery, permanent OBS sources, QR pairing, and Director Talkback.";}
extern "C" __declspec(dllexport) const char *obs_module_author(){return "TechFixNG";}
extern "C" __declspec(dllexport) bool obs_module_load(){write_load_status("VyStream 2.9.0 module entered obs_module_load on OBS 32");register_tools_menu(nullptr);queue_menu_registration();restore_saved_sources();WSADATA d{};if(WSAStartup(MAKEWORD(2,2),&d)){write_load_status("ERROR: WSAStartup failed");return true;}g_socket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(g_socket==INVALID_SOCKET){write_load_status("ERROR: discovery socket creation failed");return true;}BOOL reuse=TRUE;DWORD timeout=500;setsockopt(g_socket,SOL_SOCKET,SO_REUSEADDR,reinterpret_cast<const char*>(&reuse),sizeof(reuse));setsockopt(g_socket,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_ANY);a.sin_port=htons(45990);if(bind(g_socket,reinterpret_cast<const sockaddr*>(&a),sizeof(a))==SOCKET_ERROR){char error[128]{};std::snprintf(error,sizeof(error),"ERROR: UDP 45990 bind failed with Winsock code %d",WSAGetLastError());write_load_status(error);closesocket(g_socket);g_socket=INVALID_SOCKET;return true;}write_load_status("UDP 45990 discovery listener started");g_running=true;start_return_listener();g_network_worker=std::thread(network_monitor_loop);g_worker=std::thread(discovery_loop);return true;}
extern "C" __declspec(dllexport) void obs_module_unload(){stop_ptt();g_running=false;if(g_socket!=INVALID_SOCKET){closesocket(g_socket);g_socket=INVALID_SOCKET;}if(g_worker.joinable())g_worker.join();if(g_network_worker.joinable())g_network_worker.join();stop_return_listener();WSACleanup();}
BOOL WINAPI DllMain(HINSTANCE,DWORD,LPVOID){return TRUE;}

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mmsystem.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commctrl.h>
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <iterator>
#include <mutex>
#include <random>
#include <sstream>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "qr_v4.h"
#include "resource.h"

namespace {
constexpr int DISCOVERY_PORT=45990, TALKBACK_PORT=46010, RETURN_PORT=46011;
constexpr UINT WM_CAMERAS=WM_APP+1, WM_STATUS=WM_APP+2, WM_TALLY=WM_APP+3;
constexpr int IDC_CAMERA=1001, IDC_ADD=1002, IDC_PTT=1003, IDC_ATTACH=1004, IDC_REFRESH=1005, IDC_RENAME_EDIT=1006, IDC_RENAME=1007, IDC_REPLY_MUTE=1008, IDC_REPLY_VOLUME=1009, IDC_AUTO_ADD=1010;
constexpr int IDC_TAB_OVERVIEW=1101, IDC_TAB_CAMERAS=1102, IDC_TAB_TALKBACK=1103, IDC_TAB_SETTINGS=1104;
constexpr COLORREF BG=RGB(7,7,9), PANEL=RGB(14,14,17), BLUE=RGB(20,104,224), GREEN=RGB(35,205,123), TEXT=RGB(242,244,248), MUTED=RGB(142,148,160);

struct Camera {
  std::string id,name,ip,token;
  int srtPort=0,talkPort=TALKBACK_PORT,inputNumber=0;
  DWORD lastSeen=0,lastHealth=0;
  int tally=0;
  bool replyMuted=false,autoAdd=true,addPending=false;
  std::string healthState="READY",network="—",thermal="—",resolution="—",codec="—";
  int signal=-1,bitrateKbps=0,latencyMs=-1,packetLoss=-1,fps=0,battery=-1;
  long long droppedFrames=0;
  bool recording=false,microphoneMuted=false;
};
std::atomic<bool> running{true},ptt{false},attached{true};
std::mutex cameraMutex;
std::mutex vmixInputMutex;
std::vector<Camera> cameras;
SOCKET discoverySocket=INVALID_SOCKET,returnSocket=INVALID_SOCKET;
std::thread discoveryThread,replyThread,vmixThread,tallyThread;
HWND mainWindow=nullptr,cameraCombo=nullptr,statusText=nullptr,vmixState=nullptr,qrArea=nullptr,pttButton=nullptr,renameEdit=nullptr,replyMute=nullptr,autoAddCheck=nullptr;
HWND addButton=nullptr,refreshButton=nullptr,attachCheck=nullptr,renameButton=nullptr;
HWND tabButtons[4]{};
WNDPROC oldPttProc=nullptr;
HFONT fontNormal=nullptr,fontBold=nullptr,fontSmall=nullptr;
HBRUSH backgroundBrush=nullptr,panelBrush=nullptr,fieldBrush=nullptr;
VystrmQrV4 pairingQr;
std::string pairingPayload,desktopToken,localIp;
int selectedCamera=-1;
int activeTab=0;
bool launchedWithVmix=false;
HWAVEIN waveIn=nullptr;WAVEHDR waveHeaders[4]{};std::vector<std::vector<char>> waveBuffers(4,std::vector<char>(640));
HWAVEOUT waveOut=nullptr;WAVEHDR outHeaders[24]{};char outAudio[24][960]{};int outSlot=0;

std::wstring widen(const std::string&s){if(s.empty())return{};int n=MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0);std::wstring w(n,0);MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),w.data(),n);return w;}
std::string clean(std::string s){for(char&c:s)if(c=='|'||c=='\r'||c=='\n')c='-';return s;}
std::vector<std::string> split(const std::string&s,char d='|'){std::vector<std::string>v;size_t b=0;while(b<=s.size()){size_t e=s.find(d,b);v.push_back(s.substr(b,e==std::string::npos?e:e-b));if(e==std::string::npos)break;b=e+1;}return v;}
int numberOr(const std::string&s,int fallback=-1){char*end=nullptr;long value=strtol(s.c_str(),&end,10);return end&&*end=='\0'?(int)value:fallback;}
std::string urlEncode(const std::string&s){static const char*h="0123456789ABCDEF";std::string o;for(unsigned char c:s){if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')o+=c;else{o+='%';o+=h[c>>4];o+=h[c&15];}}return o;}
std::string appData(){wchar_t p[MAX_PATH]{};SHGetFolderPathW(nullptr,CSIDL_APPDATA,nullptr,0,p);std::wstring w=p;w+=L"\\VyStream\\vMix Bridge";SHCreateDirectoryExW(nullptr,w.c_str(),nullptr);int n=WideCharToMultiByte(CP_UTF8,0,w.c_str(),-1,nullptr,0,nullptr,nullptr);std::string s(n-1,0);WideCharToMultiByte(CP_UTF8,0,w.c_str(),-1,s.data(),n,nullptr,nullptr);return s;}
std::string iniPath(){return appData()+"\\settings.ini";}
std::string readIni(const std::string&section,const char*key){char b[512]{};GetPrivateProfileStringA(section.c_str(),key,"",b,sizeof(b),iniPath().c_str());return b;}
void writeIni(const std::string&section,const char*key,const std::string&v){WritePrivateProfileStringA(section.c_str(),key,v.c_str(),iniPath().c_str());}
std::string randomToken(){std::random_device rd;char b[33]{};for(int i=0;i<16;i++)std::snprintf(b+i*2,3,"%02x",rd()&255);return b;}
std::string computerName(){char b[MAX_COMPUTERNAME_LENGTH+1]{};DWORD n=sizeof(b);return GetComputerNameA(b,&n)?clean(std::string(b,n)):"vMix-PC";}
bool launchVmix(){
  wchar_t registered[MAX_PATH]{};DWORD registeredSize=sizeof(registered);
  if(RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\vMix64.exe",nullptr,RRF_RT_REG_SZ,nullptr,registered,&registeredSize)==ERROR_SUCCESS&&GetFileAttributesW(registered)!=INVALID_FILE_ATTRIBUTES){ShellExecuteW(nullptr,L"open",registered,nullptr,nullptr,SW_SHOWNORMAL);return true;}
  const int folders[]={CSIDL_PROGRAM_FILESX86,CSIDL_PROGRAM_FILES};
  for(int folder:folders){wchar_t root[MAX_PATH]{};if(SHGetFolderPathW(nullptr,folder,nullptr,SHGFP_TYPE_CURRENT,root)!=S_OK)continue;std::wstring path=root;path+=L"\\vMix\\vMix64.exe";if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES){ShellExecuteW(nullptr,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);return true;}}
  wchar_t located[MAX_PATH]{};if(SearchPathW(nullptr,L"vMix64.exe",nullptr,MAX_PATH,located,nullptr)){ShellExecuteW(nullptr,L"open",located,nullptr,nullptr,SW_SHOWNORMAL);return true;}
  MessageBoxW(nullptr,L"vMix64.exe was not found in the standard vMix installation folders. Start vMix normally, then open VyStream Bridge.",L"vMix not found",MB_ICONINFORMATION);
  return false;
}

bool isVmixProcessWindow(HWND window){
  if(!window||window==mainWindow||!IsWindowVisible(window)||GetWindow(window,GW_OWNER))return false;
  DWORD processId=0;GetWindowThreadProcessId(window,&processId);if(!processId)return false;
  HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,processId);if(!process)return false;
  wchar_t path[MAX_PATH]{};DWORD size=MAX_PATH;bool matched=false;
  if(QueryFullProcessImageNameW(process,0,path,&size)){std::wstring executable=path;std::transform(executable.begin(),executable.end(),executable.begin(),[](wchar_t c){return (wchar_t)towlower(c);});matched=executable.size()>=10&&executable.rfind(L"vmix64.exe")==executable.size()-10;}
  CloseHandle(process);return matched;
}
BOOL CALLBACK findVmixCallback(HWND window,LPARAM result){if(isVmixProcessWindow(window)){*reinterpret_cast<HWND*>(result)=window;return FALSE;}return TRUE;}
HWND findVmixWindow(){HWND result=nullptr;EnumWindows(findVmixCallback,reinterpret_cast<LPARAM>(&result));return result;}

std::string bestIpv4(){ULONG z=15000;std::vector<unsigned char>b(z);auto*l=(IP_ADAPTER_ADDRESSES*)b.data();ULONG r=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,l,&z);if(r==ERROR_BUFFER_OVERFLOW){b.resize(z);l=(IP_ADAPTER_ADDRESSES*)b.data();r=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,l,&z);}if(r!=NO_ERROR)return{};std::string fallback;for(auto*a=l;a;a=a->Next){if(a->OperStatus!=IfOperStatusUp||a->IfType==IF_TYPE_SOFTWARE_LOOPBACK)continue;for(auto*u=a->FirstUnicastAddress;u;u=u->Next){auto*sa=(sockaddr_in*)u->Address.lpSockaddr;if(!sa||sa->sin_family!=AF_INET)continue;char t[INET_ADDRSTRLEN]{};if(!InetNtopA(AF_INET,&sa->sin_addr,t,sizeof(t))||!strcmp(t,"127.0.0.1")||!strncmp(t,"169.254.",8))continue;if(a->IfType==IF_TYPE_IEEE80211)return t;if(fallback.empty())fallback=t;}}return fallback;}
void postStatus(const std::wstring&s){auto*p=new std::wstring(s);PostMessageW(mainWindow,WM_STATUS,0,(LPARAM)p);}

bool tcpRequest(const std::string&host,int port,const std::string&request,std::string&response,int timeoutMs=1500){addrinfo h{},*a=nullptr;h.ai_family=AF_INET;h.ai_socktype=SOCK_STREAM;if(getaddrinfo(host.c_str(),std::to_string(port).c_str(),&h,&a))return false;SOCKET s=socket(a->ai_family,a->ai_socktype,a->ai_protocol);if(s==INVALID_SOCKET){freeaddrinfo(a);return false;}DWORD tv=timeoutMs;setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(char*)&tv,sizeof(tv));setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,(char*)&tv,sizeof(tv));bool ok=connect(s,a->ai_addr,(int)a->ai_addrlen)!=SOCKET_ERROR;freeaddrinfo(a);if(!ok){closesocket(s);return false;}send(s,request.data(),(int)request.size(),0);char b[8192];int n;while((n=recv(s,b,sizeof(b),0))>0)response.append(b,n);closesocket(s);return true;}
bool vmixHttp(const std::string&query,std::string*body=nullptr){std::string path="/api"+(query.empty()?"":"/?"+query);std::string req="GET "+path+" HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",res;if(!tcpRequest("127.0.0.1",8088,req,res))return false;auto p=res.find("\r\n\r\n");if(body&&p!=std::string::npos)*body=res.substr(p+4);return res.find(" 200 ")!=std::string::npos;}
bool inputNumberExists(const std::string&xml,int number){return number>0&&xml.find("number=\""+std::to_string(number)+"\"")!=std::string::npos;}
std::set<int> inputNumbers(const std::string&xml){std::set<int> numbers;size_t p=0;while((p=xml.find("number=\"",p))!=std::string::npos){p+=8;int number=atoi(xml.c_str()+p);if(number>0)numbers.insert(number);}return numbers;}
bool createVmixInput(Camera&c){
  std::lock_guard<std::mutex>createLock(vmixInputMutex);
  std::string before;if(!vmixHttp("",&before))return false;auto oldNumbers=inputNumbers(before);
  std::string uri="srt://0.0.0.0:"+std::to_string(c.srtPort)+"?mode=listener&latency=200000";
  std::string add="Function=AddInput&Value="+urlEncode("Stream|"+uri);if(!vmixHttp(add))return false;
  int number=0;
  for(int attempt=0;attempt<12&&!number;attempt++){Sleep(250);std::string after;if(!vmixHttp("",&after))continue;for(int candidate:inputNumbers(after))if(!oldNumbers.count(candidate)){number=candidate;break;}}
  if(!number)return false;c.inputNumber=number;
  vmixHttp("Function=SetInputName&Input="+std::to_string(number)+"&Value="+urlEncode(c.name));
  writeIni(c.id,"input",std::to_string(number));writeIni(c.id,"name",c.name);return true;
}
void ensureVmixInputs(){std::string xml;if(!vmixHttp("",&xml))return;std::vector<Camera> work;{std::lock_guard<std::mutex>lock(cameraMutex);for(auto&c:cameras){if(!c.autoAdd||GetTickCount()-c.lastSeen>6000||c.addPending)continue;if(inputNumberExists(xml,c.inputNumber))continue;c.inputNumber=0;c.addPending=true;work.push_back(c);}}for(auto c:work){bool ok=createVmixInput(c);{std::lock_guard<std::mutex>lock(cameraMutex);auto it=std::find_if(cameras.begin(),cameras.end(),[&](const Camera&x){return x.id==c.id;});if(it!=cameras.end()){it->addPending=false;if(ok)it->inputNumber=c.inputNumber;}}postStatus(ok?L"A connected camera was restored in vMix.":L"Automatic input restoration is waiting for vMix.");}}

int nextPort(const std::string&excludeId={}){std::set<int>used;char sections[32768]{};GetPrivateProfileSectionNamesA(sections,sizeof(sections),iniPath().c_str());for(const char*s=sections;*s;s+=strlen(s)+1)if(excludeId!=s){int port=(int)GetPrivateProfileIntA(s,"port",0,iniPath().c_str());if(port>=9000&&port<=65535)used.insert(port);}for(const auto&camera:cameras)if(camera.id!=excludeId&&camera.srtPort>=9000)used.insert(camera.srtPort);for(int port=9000;port<=65535;port++)if(!used.count(port))return port;return 0;}
bool portBelongsToAnotherCamera(const std::string&id,int port){if(port<9000||port>65535)return true;for(const auto&camera:cameras)if(camera.id!=id&&camera.srtPort==port)return true;char sections[32768]{};GetPrivateProfileSectionNamesA(sections,sizeof(sections),iniPath().c_str());for(const char*s=sections;*s;s+=strlen(s)+1)if(id!=s&&GetPrivateProfileIntA(s,"port",0,iniPath().c_str())==port)return true;return false;}
bool inputBelongsToAnotherCamera(const std::string&id,int input){if(input<=0)return false;for(const auto&camera:cameras)if(camera.id!=id&&camera.inputNumber==input)return true;char sections[32768]{};GetPrivateProfileSectionNamesA(sections,sizeof(sections),iniPath().c_str());for(const char*s=sections;*s;s+=strlen(s)+1)if(id!=s&&GetPrivateProfileIntA(s,"input",0,iniPath().c_str())==input)return true;return false;}
void refreshCameras(){PostMessageW(mainWindow,WM_CAMERAS,0,0);}
void discoveryLoop(){
  char b[1024];
  while(running){
    sockaddr_in from{};int fl=sizeof(from);int n=recvfrom(discoverySocket,b,sizeof(b)-1,0,(sockaddr*)&from,&fl);
    if(n==SOCKET_ERROR){if(running)Sleep(100);continue;}b[n]=0;
    auto f=split(std::string(b,n));
    if(f.size()>=17&&f[0]=="VYSHEALTH1"){
      std::lock_guard<std::mutex>lock(cameraMutex);
      auto health=std::find_if(cameras.begin(),cameras.end(),[&](const Camera&c){return c.token==f[1];});
      if(health!=cameras.end()){
        health->healthState=clean(f[2]);health->network=clean(f[3]);health->signal=numberOr(f[4]);health->bitrateKbps=numberOr(f[5],0);
        health->latencyMs=numberOr(f[6]);health->packetLoss=numberOr(f[7]);health->fps=numberOr(f[8],0);health->droppedFrames=std::strtoll(f[9].c_str(),nullptr,10);
        health->battery=numberOr(f[10]);health->thermal=clean(f[11]);health->resolution=clean(f[12]);health->codec=clean(f[13]);
        health->recording=f[14]=="1";health->microphoneMuted=f[15]=="1";health->lastHealth=GetTickCount();health->lastSeen=GetTickCount();
      }
      refreshCameras();continue;
    }
    if(f.size()<3||(f[0]!="OBS_SRT_DISCOVER_V4"&&f[0]!="OBS_SRT_DISCOVER_V3"))continue;
    std::string id=clean(f[1]),suggested=clean(f[2]);char ip[INET_ADDRSTRLEN]{};InetNtopA(AF_INET,&from.sin_addr,ip,sizeof(ip));
    int talk=f.size()>3?atoi(f[3].c_str()):TALKBACK_PORT;if(talk<1024||talk>65535)talk=TALKBACK_PORT;
    std::lock_guard<std::mutex>lock(cameraMutex);
    auto it=std::find_if(cameras.begin(),cameras.end(),[&](const Camera&c){return c.id==id;});
    if(it==cameras.end()){
      Camera c;c.id=id;c.name=readIni(id,"name");if(c.name.empty())c.name=(suggested.empty()?"VyStream":suggested)+"-Cam";
      c.token=readIni(id,"token");if(c.token.empty()){c.token=randomToken();writeIni(id,"token",c.token);}
      c.srtPort=GetPrivateProfileIntA(id.c_str(),"port",0,iniPath().c_str());if(portBelongsToAnotherCamera(id,c.srtPort)){c.srtPort=nextPort(id);writeIni(id,"port",std::to_string(c.srtPort));}
      c.inputNumber=GetPrivateProfileIntA(id.c_str(),"input",0,iniPath().c_str());
      if(inputBelongsToAnotherCamera(id,c.inputNumber)){c.inputNumber=0;writeIni(id,"input","0");}
      c.replyMuted=GetPrivateProfileIntA(id.c_str(),"reply_muted",0,iniPath().c_str())!=0;
      c.autoAdd=GetPrivateProfileIntA(id.c_str(),"auto_add",1,iniPath().c_str())!=0;
      c.ip=ip;c.talkPort=talk;c.lastSeen=GetTickCount();cameras.push_back(c);it=std::prev(cameras.end());
    }else{it->ip=ip;it->talkPort=talk;it->lastSeen=GetTickCount();}
    std::string offer="OBS_SRT_OFFER_V3|"+computerName()+"|"+localIp+"|"+std::to_string(it->srtPort)+"|"+it->token+"|"+it->name;
    sendto(discoverySocket,offer.data(),(int)offer.size(),0,(sockaddr*)&from,fl);refreshCameras();
  }
}

void playReply(const char*d,int len){if(!waveOut||len<=0)return;int slot=outSlot++%24;WAVEHDR&h=outHeaders[slot];if(h.dwFlags&WHDR_PREPARED){while(!(h.dwFlags&WHDR_DONE)&&running)Sleep(2);waveOutUnprepareHeader(waveOut,&h,sizeof(h));}int count=std::min(len,960);memcpy(outAudio[slot],d,count);h={};h.lpData=outAudio[slot];h.dwBufferLength=count;if(waveOutPrepareHeader(waveOut,&h,sizeof(h))==MMSYSERR_NOERROR)waveOutWrite(waveOut,&h,sizeof(h));}
void relayCrew(const std::string&token,const char*pcm,int len){std::lock_guard<std::mutex>lock(cameraMutex);auto sender=std::find_if(cameras.begin(),cameras.end(),[&](const Camera&c){return c.token==token;});if(sender==cameras.end())return;for(auto&r:cameras){if(r.token==token)continue;std::string h="VYSTB2|"+r.token+"|"+sender->name+"|";std::vector<char>p(h.begin(),h.end());p.insert(p.end(),pcm,pcm+len);sockaddr_in to{};to.sin_family=AF_INET;to.sin_port=htons((u_short)r.talkPort);InetPtonA(AF_INET,r.ip.c_str(),&to.sin_addr);sendto(discoverySocket,p.data(),(int)p.size(),0,(sockaddr*)&to,sizeof(to));}}
void replyLoop(){char b[1500];while(running){sockaddr_in from{};int z=sizeof(from);int n=recvfrom(returnSocket,b,sizeof(b),0,(sockaddr*)&from,&z);if(n==SOCKET_ERROR){if(running)Sleep(50);continue;}if(n<10||memcmp(b,"VYSUP1|",7))continue;int e=7;while(e<n&&b[e]!='|')e++;if(e>=n)continue;std::string token(b+7,b+e);relayCrew(token,b+e+1,n-e-1);bool selected=false,muted=false;{std::lock_guard<std::mutex>lock(cameraMutex);for(size_t i=0;i<cameras.size();i++)if(cameras[i].token==token){selected=selectedCamera<0||selectedCamera==(int)i;muted=cameras[i].replyMuted;}}if(selected&&!muted){waveOutSetVolume(waveOut,0xFFFFFFFF);playReply(b+e+1,n-e-1);}}}
void sendTalkback(const char*pcm,int len){std::lock_guard<std::mutex>lock(cameraMutex);for(size_t i=0;i<cameras.size();i++){if(selectedCamera>=0&&selectedCamera!=(int)i)continue;auto&c=cameras[i];std::string h="VYSTB1|"+c.token+"|";std::vector<char>p(h.begin(),h.end());p.insert(p.end(),pcm,pcm+len);sockaddr_in to{};to.sin_family=AF_INET;to.sin_port=htons((u_short)c.talkPort);if(InetPtonA(AF_INET,c.ip.c_str(),&to.sin_addr)==1)sendto(discoverySocket,p.data(),(int)p.size(),0,(sockaddr*)&to,sizeof(to));}}
void CALLBACK waveCallback(HWAVEIN in,UINT msg,DWORD_PTR,DWORD_PTR p1,DWORD_PTR){if(msg!=WIM_DATA)return;auto*h=(WAVEHDR*)p1;if(ptt&&h->dwBytesRecorded)sendTalkback(h->lpData,(int)h->dwBytesRecorded);h->dwBytesRecorded=0;if(ptt)waveInAddBuffer(in,h,sizeof(*h));}
bool startPtt(){if(ptt.exchange(true))return true;WAVEFORMATEX f{};f.wFormatTag=WAVE_FORMAT_PCM;f.nChannels=1;f.nSamplesPerSec=16000;f.wBitsPerSample=16;f.nBlockAlign=2;f.nAvgBytesPerSec=32000;if(waveInOpen(&waveIn,WAVE_MAPPER,&f,(DWORD_PTR)waveCallback,0,CALLBACK_FUNCTION)!=MMSYSERR_NOERROR){ptt=false;return false;}for(int i=0;i<4;i++){waveHeaders[i]={};waveHeaders[i].lpData=waveBuffers[i].data();waveHeaders[i].dwBufferLength=640;waveInPrepareHeader(waveIn,&waveHeaders[i],sizeof(WAVEHDR));waveInAddBuffer(waveIn,&waveHeaders[i],sizeof(WAVEHDR));}waveInStart(waveIn);return true;}
void stopPtt(){if(!ptt.exchange(false)||!waveIn)return;waveInStop(waveIn);waveInReset(waveIn);for(auto&h:waveHeaders)waveInUnprepareHeader(waveIn,&h,sizeof(h));waveInClose(waveIn);waveIn=nullptr;}
LRESULT CALLBACK pttProc(HWND w,UINT m,WPARAM wp,LPARAM lp){if(m==WM_LBUTTONDOWN){SetCapture(w);if(!startPtt())MessageBoxW(w,L"Windows could not open the default microphone.",L"Talkback",MB_ICONERROR);InvalidateRect(w,nullptr,TRUE);return 0;}if(m==WM_LBUTTONUP||m==WM_CAPTURECHANGED){ReleaseCapture();stopPtt();InvalidateRect(w,nullptr,TRUE);return 0;}return CallWindowProcW(oldPttProc,w,m,wp,lp);}

void sendTally(Camera&c,int state){std::string m="VYSTALLY1|"+c.token+"|"+std::to_string(state);sockaddr_in to{};to.sin_family=AF_INET;to.sin_port=htons((u_short)c.talkPort);if(InetPtonA(AF_INET,c.ip.c_str(),&to.sin_addr)==1)sendto(discoverySocket,m.data(),(int)m.size(),0,(sockaddr*)&to,sizeof(to));}
void tallyLoop(){while(running){std::string res;if(!tcpRequest("127.0.0.1",8099,"SUBSCRIBE TALLY\r\n",res,2000)){Sleep(1500);continue;}SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(8099);InetPtonA(AF_INET,"127.0.0.1",&a.sin_addr);if(connect(s,(sockaddr*)&a,sizeof(a))==SOCKET_ERROR){closesocket(s);Sleep(1000);continue;}send(s,"SUBSCRIBE TALLY\r\n",17,0);char b[2048];std::string pending;while(running){int n=recv(s,b,sizeof(b),0);if(n<=0)break;pending.append(b,n);size_t e;while((e=pending.find("\r\n"))!=std::string::npos){std::string line=pending.substr(0,e);pending.erase(0,e+2);auto p=line.find("TALLY OK ");if(p==std::string::npos)continue;std::string states=line.substr(p+9);std::lock_guard<std::mutex>lock(cameraMutex);for(auto&c:cameras){if(c.inputNumber>0&&c.inputNumber<=(int)states.size()){int state=states[c.inputNumber-1]-'0';if(state!=c.tally){c.tally=state;sendTally(c,state);PostMessageW(mainWindow,WM_TALLY,0,0);}}}}}closesocket(s);Sleep(700);}}
void vmixMonitor(){bool last=false;int restoreCountdown=0;while(running){bool now=vmixHttp("");if(now!=last){last=now;PostMessageW(mainWindow,WM_STATUS,now?1:2,0);}if(now&&restoreCountdown--<=0){ensureVmixInputs();restoreCountdown=3;}Sleep(1500);}}

void drawQr(HDC dc,RECT r){HBRUSH white=CreateSolidBrush(RGB(255,255,255));FillRect(dc,&r,white);DeleteObject(white);if(pairingPayload.empty())return;int quiet=4;int availableWidth=(int)(r.right-r.left-20);int availableHeight=(int)(r.bottom-r.top-20);int scale=std::max(2,std::min(availableWidth/(VystrmQrV4::size+quiet*2),availableHeight/(VystrmQrV4::size+quiet*2)));int side=(VystrmQrV4::size+quiet*2)*scale,ox=(int)r.left+((int)(r.right-r.left)-side)/2,oy=(int)r.top+((int)(r.bottom-r.top)-side)/2;HBRUSH black=CreateSolidBrush(RGB(0,0,0));for(int y=0;y<VystrmQrV4::size;y++)for(int x=0;x<VystrmQrV4::size;x++)if(pairingQr.module(x,y)){RECT q{ox+(x+quiet)*scale,oy+(y+quiet)*scale,ox+(x+quiet+1)*scale,oy+(y+quiet+1)*scale};FillRect(dc,&q,black);}DeleteObject(black);}
void fillRect(HDC dc,int l,int t,int r,int b,COLORREF color){RECT area{l,t,r,b};HBRUSH brush=CreateSolidBrush(color);FillRect(dc,&area,brush);DeleteObject(brush);}
void label(HDC dc,int x,int y,const wchar_t*value,COLORREF color,HFONT font){SetBkMode(dc,TRANSPARENT);SetTextColor(dc,color);SelectObject(dc,font);TextOutW(dc,x,y,value,(int)wcslen(value));}
void paintWindow(HWND w){
  PAINTSTRUCT ps{};HDC dc=BeginPaint(w,&ps);RECT r{};GetClientRect(w,&r);FillRect(dc,&r,backgroundBrush);
  fillRect(dc,0,0,r.right,78,RGB(12,16,23));
  HICON icon=(HICON)GetClassLongPtrW(w,GCLP_HICONSM);if(icon)DrawIconEx(dc,16,14,icon,42,42,0,nullptr,DI_NORMAL);
  label(dc,68,12,L"VYSTRM",TEXT,fontBold);label(dc,68,42,L"REMOTE PRODUCTION BRIDGE",MUTED,fontSmall);
  fillRect(dc,0,78,r.right,112,RGB(22,28,38));
  label(dc,18,86,L"●",GREEN,fontNormal);label(dc,38,88,L"vMix CONNECTION",TEXT,fontSmall);
  fillRect(dc,12,154,r.right-12,r.bottom-66,PANEL);
  const wchar_t*headings[]={L"PRODUCTION OVERVIEW",L"PAIR A CAMERA",L"TALKBACK & INTERCOM",L"BRIDGE SETTINGS"};
  label(dc,24,168,headings[activeTab],TEXT,fontBold);
  if(activeTab==0){
    int total=0,online=0,warnings=0;{std::lock_guard<std::mutex>lock(cameraMutex);total=(int)cameras.size();for(auto&c:cameras){DWORD age=GetTickCount()-c.lastSeen;if(age<5000)online++;else warnings++;}}
    fillRect(dc,24,210,112,276,RGB(17,23,33));fillRect(dc,120,210,208,276,RGB(17,23,33));fillRect(dc,216,210,304,276,RGB(17,23,33));
    std::wstring a=std::to_wstring(total),b=std::to_wstring(online),c=std::to_wstring(warnings);
    label(dc,62,218,a.c_str(),TEXT,fontBold);label(dc,44,250,L"CAMERAS",MUTED,fontSmall);
    label(dc,158,218,b.c_str(),GREEN,fontBold);label(dc,150,250,L"ONLINE",MUTED,fontSmall);
    label(dc,254,218,c.c_str(),warnings?RGB(241,174,55):MUTED,fontBold);label(dc,238,250,L"WARNINGS",MUTED,fontSmall);
    label(dc,24,300,L"CAMERA ROUTING",MUTED,fontSmall);label(dc,24,394,L"QUICK ACTION",MUTED,fontSmall);
  }else if(activeTab==1){
    label(dc,24,212,L"SELECT CAMERA",MUTED,fontSmall);
    Camera health;bool available=false;{std::lock_guard<std::mutex>lock(cameraMutex);if(selectedCamera>=0&&selectedCamera<(int)cameras.size()){health=cameras[selectedCamera];available=true;}}
    label(dc,24,274,L"CAMERA HEALTH",MUTED,fontSmall);
    fillRect(dc,24,296,324,526,RGB(18,18,22));
    if(!available){label(dc,42,322,L"Select a connected camera to view health.",MUTED,fontSmall);}
    else{
      DWORD age=GetTickCount()-health.lastSeen;bool online=age<6000;bool recent=health.lastHealth&&GetTickCount()-health.lastHealth<7000;
      COLORREF quality=!online?RGB(224,70,70):(health.battery>=0&&health.battery<20)?RGB(241,174,55):GREEN;
      std::wstring state=!online?L"OFFLINE":widen(recent?health.healthState:"CONNECTED");
      label(dc,38,312,L"CONNECTION",MUTED,fontSmall);label(dc,210,312,state.c_str(),quality,fontSmall);
      std::wstring network=widen(health.network);if(health.signal!=-1)network+=L"  "+std::to_wstring(health.signal)+(health.network=="WIFI"?L" dBm":L"");
      label(dc,38,344,L"NETWORK",MUTED,fontSmall);label(dc,170,344,network.c_str(),TEXT,fontSmall);
      std::wstring video=health.bitrateKbps>0?std::to_wstring(health.bitrateKbps)+L" kbps":L"—";if(health.fps>0)video+=L"  •  "+std::to_wstring(health.fps)+L" fps";video+=L"  •  D"+std::to_wstring(health.droppedFrames);
      label(dc,38,376,L"VIDEO",MUTED,fontSmall);label(dc,170,376,video.c_str(),TEXT,fontSmall);
      std::wstring latency=health.latencyMs>=0?std::to_wstring(health.latencyMs)+L" ms":L"—";latency+=L"  •  Loss ";latency+=health.packetLoss>=0?std::to_wstring(health.packetLoss)+L"%":L"—";
      label(dc,38,408,L"SRT LATENCY",MUTED,fontSmall);label(dc,210,408,latency.c_str(),TEXT,fontSmall);
      std::wstring format=widen(health.resolution);if(health.codec!="—"&&!health.codec.empty())format+=L"  •  "+widen(health.codec);
      label(dc,38,440,L"FORMAT",MUTED,fontSmall);label(dc,150,440,format.c_str(),TEXT,fontSmall);
      std::wstring device=health.battery>=0?L"Battery "+std::to_wstring(health.battery)+L"%":L"Battery —";device+=L"  •  "+widen(health.thermal);
      label(dc,38,472,L"DEVICE",MUTED,fontSmall);label(dc,150,472,device.c_str(),health.battery>=0&&health.battery<20?RGB(241,174,55):TEXT,fontSmall);
      std::wstring activity=health.microphoneMuted?L"MIC MUTED":L"MIC ACTIVE";activity+=health.recording?L"  •  RECORDING":L"  •  NOT RECORDING";
      label(dc,38,504,activity.c_str(),health.microphoneMuted?RGB(241,174,55):GREEN,fontSmall);
    }
  }else if(activeTab==2){
    label(dc,24,212,L"DIRECTOR CHANNEL",MUTED,fontSmall);label(dc,24,382,L"CAMERA REPLY",MUTED,fontSmall);
    label(dc,24,452,L"Push and hold to speak. Release to listen.",MUTED,fontSmall);
  }else{
    label(dc,24,204,L"PAIR & TRANSPORT",MUTED,fontSmall);RECT qr{98,222,250,374};drawQr(dc,qr);
    label(dc,80,382,L"SCAN WITH VYSTRM CAMERA",MUTED,fontSmall);
    label(dc,24,410,L"AUTOMATION",MUTED,fontSmall);label(dc,24,462,L"vMIX CONTROL",MUTED,fontSmall);
  }
  fillRect(dc,0,r.bottom-56,r.right,r.bottom,RGB(12,16,23));
  label(dc,18,r.bottom-46,L"UDP 45990",RGB(53,190,126),fontSmall);label(dc,r.right-103,r.bottom-46,L"VYSTRM 1.3.5",MUTED,fontSmall);
  EndPaint(w,&ps);
}
void updateSelectedControls(){std::lock_guard<std::mutex>lock(cameraMutex);bool valid=selectedCamera>=0&&selectedCamera<(int)cameras.size();EnableWindow(renameEdit,valid);EnableWindow(replyMute,valid);EnableWindow(autoAddCheck,valid);if(!valid){SetWindowTextW(renameEdit,L"");CheckDlgButton(mainWindow,IDC_REPLY_MUTE,BST_UNCHECKED);CheckDlgButton(mainWindow,IDC_AUTO_ADD,BST_UNCHECKED);return;}auto&c=cameras[selectedCamera];SetWindowTextW(renameEdit,widen(c.name).c_str());CheckDlgButton(mainWindow,IDC_REPLY_MUTE,c.replyMuted?BST_CHECKED:BST_UNCHECKED);CheckDlgButton(mainWindow,IDC_AUTO_ADD,c.autoAdd?BST_CHECKED:BST_UNCHECKED);}
void updateCombo(){
  SendMessageW(cameraCombo,CB_RESETCONTENT,0,0);SendMessageW(cameraCombo,CB_ADDSTRING,0,(LPARAM)L"All connected cameras");
  {std::lock_guard<std::mutex>lock(cameraMutex);
    if(selectedCamera<0&&!cameras.empty())selectedCamera=0;
    if(selectedCamera>=(int)cameras.size())selectedCamera=cameras.empty()?-1:0;
    for(auto&c:cameras){DWORD age=GetTickCount()-c.lastSeen;std::wstring item=widen(c.name)+(age<5000?L"  • CONNECTED":L"  • OFFLINE");if(age<5000)item+=L"  • "+std::to_wstring(age/1000)+L"s";if(c.tally==1)item+=L"  • LIVE";else if(c.tally==2)item+=L"  • PREVIEW";SendMessageW(cameraCombo,CB_ADDSTRING,0,(LPARAM)item.c_str());}
  }
  SendMessageW(cameraCombo,CB_SETCURSEL,selectedCamera+1,0);updateSelectedControls();
}
void attachToVmix(){
  static RECT previous{};
  static int chosenSide=0;
  if(!attached||SendMessageW(cameraCombo,CB_GETDROPPEDSTATE,0,0))return;
  HWND v=findVmixWindow();
  if(!v||IsIconic(v))return;
  RECT vr{},wr{};GetWindowRect(v,&vr);GetWindowRect(mainWindow,&wr);
  if(EqualRect(&vr,&previous))return;
  previous=vr;
  int width=wr.right-wr.left,height=std::min((int)(vr.bottom-vr.top),720);
  MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(v,MONITOR_DEFAULTTONEAREST),&monitor);
  if(chosenSide==0)chosenSide=vr.right+width<=monitor.rcWork.right?1:-1;
  int x=chosenSide>0?vr.right:vr.left-width;
  x=std::clamp(x,(int)monitor.rcWork.left,(int)monitor.rcWork.right-width);
  int y=std::clamp((int)vr.top,(int)monitor.rcWork.top,(int)monitor.rcWork.bottom-height);
  SetWindowPos(mainWindow,HWND_NOTOPMOST,x,y,width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
}

void showControl(HWND control,bool show){if(control)ShowWindow(control,show?SW_SHOW:SW_HIDE);}
void setActiveTab(int tab){
  activeTab=std::clamp(tab,0,3);
  for(int i=0;i<4;i++)SendMessageW(tabButtons[i],BM_SETSTATE,i==activeTab,0);
  bool overview=activeTab==0,camera=activeTab==1,talk=activeTab==2,settings=activeTab==3;
  showControl(cameraCombo,overview||camera||talk);showControl(addButton,overview);showControl(renameEdit,camera);showControl(renameButton,camera);
  showControl(pttButton,talk);showControl(replyMute,talk);
  showControl(autoAddCheck,settings);showControl(refreshButton,settings);showControl(attachCheck,settings);
  showControl(statusText,overview||settings);InvalidateRect(mainWindow,nullptr,TRUE);
  if(overview){SetWindowPos(cameraCombo,nullptr,24,330,300,200,SWP_NOZORDER);SetWindowPos(addButton,nullptr,24,424,300,46,SWP_NOZORDER);SetWindowPos(statusText,nullptr,24,490,300,42,SWP_NOZORDER);}
  if(camera){SetWindowPos(cameraCombo,nullptr,24,236,300,200,SWP_NOZORDER);SetWindowPos(renameEdit,nullptr,24,550,216,36,SWP_NOZORDER);SetWindowPos(renameButton,nullptr,246,550,78,36,SWP_NOZORDER);}
  if(talk){SetWindowPos(cameraCombo,nullptr,24,242,300,200,SWP_NOZORDER);SetWindowPos(pttButton,nullptr,24,292,300,68,SWP_NOZORDER);SetWindowPos(replyMute,nullptr,24,406,280,34,SWP_NOZORDER);}
  if(settings){SetWindowPos(autoAddCheck,nullptr,24,428,300,30,SWP_NOZORDER);SetWindowPos(refreshButton,nullptr,24,486,300,40,SWP_NOZORDER);SetWindowPos(attachCheck,nullptr,24,532,280,30,SWP_NOZORDER);SetWindowPos(statusText,nullptr,24,570,300,24,SWP_NOZORDER);}
}
void createV11Controls(HWND w){
  if(renameEdit)return;
  const wchar_t*titles[]={L"OVERVIEW",L"CAMERAS",L"TALKBACK",L"SETTINGS"};
  const int ids[]={IDC_TAB_OVERVIEW,IDC_TAB_CAMERAS,IDC_TAB_TALKBACK,IDC_TAB_SETTINGS};
  for(int i=0;i<4;i++)tabButtons[i]=CreateWindowW(WC_BUTTONW,titles[i],WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,8+i*88,116,84,34,w,(HMENU)ids[i],nullptr,nullptr);
  renameEdit=CreateWindowExW(0,WC_EDITW,L"",WS_CHILD|ES_AUTOHSCROLL,24,506,216,36,w,(HMENU)IDC_RENAME_EDIT,nullptr,nullptr);
  renameButton=CreateWindowW(WC_BUTTONW,L"SAVE NAME",WS_CHILD|BS_OWNERDRAW,246,506,78,36,w,(HMENU)IDC_RENAME,nullptr,nullptr);
  replyMute=CreateWindowW(WC_BUTTONW,L"  Mute camera reply",WS_CHILD|BS_AUTOCHECKBOX,24,406,280,34,w,(HMENU)IDC_REPLY_MUTE,nullptr,nullptr);
  autoAddCheck=CreateWindowW(WC_BUTTONW,L"  Restore vMix camera mappings automatically",WS_CHILD|BS_AUTOCHECKBOX,24,246,300,34,w,(HMENU)IDC_AUTO_ADD,nullptr,nullptr);
  for(HWND c=GetWindow(w,GW_CHILD);c;c=GetWindow(c,GW_HWNDNEXT))SendMessageW(c,WM_SETFONT,(WPARAM)fontNormal,TRUE);
  updateSelectedControls();setActiveTab(0);
}

void renameSelectedCamera(){wchar_t b[128]{};GetWindowTextW(renameEdit,b,128);std::wstring value=b;value.erase(0,value.find_first_not_of(L" \t"));if(value.empty()){MessageBoxW(mainWindow,L"Enter a camera name first.",L"VyStream vMix Bridge",MB_ICONINFORMATION);return;}int input=0;std::string id,name;{std::lock_guard<std::mutex>lock(cameraMutex);if(selectedCamera<0||selectedCamera>=(int)cameras.size())return;name=clean(std::string(value.begin(),value.end()));auto&c=cameras[selectedCamera];c.name=name;input=c.inputNumber;id=c.id;writeIni(id,"name",name);}if(input>0)vmixHttp("Function=SetInputName&Input="+std::to_string(input)+"&Value="+urlEncode(name));updateCombo();postStatus(L"Camera name saved permanently.");}

LRESULT CALLBACK wndProc(HWND w,UINT m,WPARAM wp,LPARAM lp){
  switch(m){
    case WM_CREATE:{
      backgroundBrush=CreateSolidBrush(RGB(7,9,13));panelBrush=CreateSolidBrush(RGB(15,19,26));fieldBrush=CreateSolidBrush(RGB(25,31,41));
      fontNormal=CreateFontW(17,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
      fontBold=CreateFontW(23,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI Semibold");
      fontSmall=CreateFontW(14,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
      cameraCombo=CreateWindowW(WC_COMBOBOXW,L"",WS_CHILD|CBS_DROPDOWNLIST,24,326,300,200,w,(HMENU)IDC_CAMERA,nullptr,nullptr);
      pttButton=CreateWindowW(WC_BUTTONW,L"HOLD TO TALK",WS_CHILD|BS_OWNERDRAW,24,288,300,68,w,(HMENU)IDC_PTT,nullptr,nullptr);
      oldPttProc=(WNDPROC)SetWindowLongPtrW(pttButton,GWLP_WNDPROC,(LONG_PTR)pttProc);
      addButton=CreateWindowW(WC_BUTTONW,L"MAP / RECONNECT TO vMIX",WS_CHILD|BS_OWNERDRAW,24,424,300,46,w,(HMENU)IDC_ADD,nullptr,nullptr);
      refreshButton=CreateWindowW(WC_BUTTONW,L"REFRESH vMIX CONNECTION",WS_CHILD|BS_OWNERDRAW,24,356,300,42,w,(HMENU)IDC_REFRESH,nullptr,nullptr);
  attachCheck=CreateWindowW(WC_BUTTONW,L"  Keep bridge beside vMix",WS_CHILD|BS_AUTOCHECKBOX,24,400,280,34,w,(HMENU)IDC_ATTACH,nullptr,nullptr);
      CheckDlgButton(w,IDC_ATTACH,BST_CHECKED);
      vmixState=CreateWindowW(WC_STATICW,L"● Checking vMix...",WS_CHILD|WS_VISIBLE,134,86,210,22,w,nullptr,nullptr,nullptr);
      statusText=CreateWindowW(WC_STATICW,L"Ready for VyStream cameras",WS_CHILD,24,490,300,42,w,nullptr,nullptr,nullptr);
      SetTimer(w,1,800,nullptr);createV11Controls(w);return 0;
    }
    case WM_COMMAND:{
      int id=LOWORD(wp);
      if(id>=IDC_TAB_OVERVIEW&&id<=IDC_TAB_SETTINGS){setActiveTab(id-IDC_TAB_OVERVIEW);return 0;}
      if(id==IDC_CAMERA&&HIWORD(wp)==CBN_SELCHANGE){selectedCamera=(int)SendMessageW(cameraCombo,CB_GETCURSEL,0,0)-1;updateSelectedControls();return 0;}
      if(id==IDC_RENAME){renameSelectedCamera();return 0;}
      if(id==IDC_ATTACH){attached=IsDlgButtonChecked(w,IDC_ATTACH)==BST_CHECKED;return 0;}
      if(id==IDC_REFRESH){vmixHttp("");postStatus(L"vMix connection refreshed.");return 0;}
      if(id==IDC_REPLY_MUTE||id==IDC_AUTO_ADD){std::lock_guard<std::mutex>lock(cameraMutex);if(selectedCamera>=0&&selectedCamera<(int)cameras.size()){auto&c=cameras[selectedCamera];if(id==IDC_REPLY_MUTE){c.replyMuted=IsDlgButtonChecked(w,IDC_REPLY_MUTE)==BST_CHECKED;writeIni(c.id,"reply_muted",c.replyMuted?"1":"0");}else{c.autoAdd=IsDlgButtonChecked(w,IDC_AUTO_ADD)==BST_CHECKED;writeIni(c.id,"auto_add",c.autoAdd?"1":"0");}}return 0;}
      if(id==IDC_ADD){Camera c;{std::lock_guard<std::mutex>lock(cameraMutex);if(selectedCamera<0||selectedCamera>=(int)cameras.size()){MessageBoxW(w,L"Select one camera first.",L"VyStream vMix Bridge",MB_ICONINFORMATION);return 0;}c=cameras[selectedCamera];}postStatus(L"Creating the SRT input in vMix...");std::thread([c]()mutable{bool ok=createVmixInput(c);if(ok){std::lock_guard<std::mutex>lock(cameraMutex);auto it=std::find_if(cameras.begin(),cameras.end(),[&](const Camera&x){return x.id==c.id;});if(it!=cameras.end())it->inputNumber=c.inputNumber;}postStatus(ok?L"Camera input created in vMix.":L"Could not create input. Enable the vMix Web Controller.");}).detach();return 0;}
      break;
    }
    case WM_PAINT:paintWindow(w);return 0;
    case WM_TIMER:attachToVmix();InvalidateRect(w,nullptr,FALSE);return 0;
    case WM_CAMERAS:if(!SendMessageW(cameraCombo,CB_GETDROPPEDSTATE,0,0))updateCombo();InvalidateRect(w,nullptr,FALSE);return 0;
    case WM_TALLY:updateCombo();InvalidateRect(w,nullptr,FALSE);return 0;
    case WM_STATUS:{if(wp==1)SetWindowTextW(vmixState,L"● vMix CONNECTED");else if(wp==2)SetWindowTextW(vmixState,L"● vMix OFFLINE");else if(lp){auto*s=(std::wstring*)lp;SetWindowTextW(statusText,s->c_str());delete s;}InvalidateRect(w,nullptr,FALSE);return 0;}
    case WM_CTLCOLORSTATIC:{HDC dc=(HDC)wp;SetTextColor(dc,TEXT);SetBkColor(dc,PANEL);return (LRESULT)panelBrush;}
    case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX:{HDC dc=(HDC)wp;SetTextColor(dc,TEXT);SetBkColor(dc,RGB(25,31,41));return (LRESULT)fieldBrush;}
    case WM_DRAWITEM:{auto*d=(DRAWITEMSTRUCT*)lp;bool isTab=d->CtlID>=IDC_TAB_OVERVIEW&&d->CtlID<=IDC_TAB_SETTINGS;COLORREF color=RGB(20,25,34);if(d->CtlID==IDC_PTT)color=ptt?RGB(210,48,58):BLUE;else if(isTab&&d->CtlID-IDC_TAB_OVERVIEW==activeTab)color=RGB(18,75,143);else if(d->CtlID==IDC_ADD||d->CtlID==IDC_RENAME||d->CtlID==IDC_REFRESH)color=RGB(22,85,165);HBRUSH br=CreateSolidBrush(color);FillRect(d->hDC,&d->rcItem,br);DeleteObject(br);SetBkMode(d->hDC,TRANSPARENT);SetTextColor(d->hDC,RGB(245,248,252));SelectObject(d->hDC,isTab?fontSmall:fontNormal);wchar_t textValue[96]{};GetWindowTextW(d->hwndItem,textValue,96);if(d->CtlID==IDC_PTT)wcscpy_s(textValue,ptt?L"TALKING — RELEASE":L"HOLD TO TALK");DrawTextW(d->hDC,textValue,-1,&d->rcItem,DT_CENTER|DT_VCENTER|DT_SINGLELINE);return TRUE;}
    case WM_DESTROY:running=false;stopPtt();DeleteObject(backgroundBrush);DeleteObject(panelBrush);DeleteObject(fieldBrush);DeleteObject(fontNormal);DeleteObject(fontBold);DeleteObject(fontSmall);PostQuitMessage(0);return 0;
  }
  return DefWindowProcW(w,m,wp,lp);
}

bool startNetwork(){WSADATA d{};if(WSAStartup(MAKEWORD(2,2),&d))return false;discoverySocket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);BOOL reuse=TRUE,bcast=TRUE;setsockopt(discoverySocket,SOL_SOCKET,SO_REUSEADDR,(char*)&reuse,sizeof(reuse));setsockopt(discoverySocket,SOL_SOCKET,SO_BROADCAST,(char*)&bcast,sizeof(bcast));sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_ANY);a.sin_port=htons(DISCOVERY_PORT);if(bind(discoverySocket,(sockaddr*)&a,sizeof(a))==SOCKET_ERROR)return false;returnSocket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);setsockopt(returnSocket,SOL_SOCKET,SO_REUSEADDR,(char*)&reuse,sizeof(reuse));a.sin_port=htons(RETURN_PORT);if(bind(returnSocket,(sockaddr*)&a,sizeof(a))==SOCKET_ERROR)return false;WAVEFORMATEX f{};f.wFormatTag=WAVE_FORMAT_PCM;f.nChannels=1;f.nSamplesPerSec=16000;f.wBitsPerSample=16;f.nBlockAlign=2;f.nAvgBytesPerSec=32000;waveOutOpen(&waveOut,WAVE_MAPPER,&f,0,0,CALLBACK_NULL);discoveryThread=std::thread(discoveryLoop);replyThread=std::thread(replyLoop);vmixThread=std::thread(vmixMonitor);tallyThread=std::thread(tallyLoop);return true;}
void stopNetwork(){running=false;if(discoverySocket!=INVALID_SOCKET)closesocket(discoverySocket);if(returnSocket!=INVALID_SOCKET)closesocket(returnSocket);if(discoveryThread.joinable())discoveryThread.join();if(replyThread.joinable())replyThread.join();if(vmixThread.joinable())vmixThread.join();if(tallyThread.joinable())tallyThread.join();if(waveOut){waveOutReset(waveOut);waveOutClose(waveOut);}WSACleanup();}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR commandLine,int show){INITCOMMONCONTROLSEX cc{sizeof(cc),ICC_STANDARD_CLASSES|ICC_BAR_CLASSES};InitCommonControlsEx(&cc);launchedWithVmix=commandLine&&wcsstr(commandLine,L"--launch-vmix");if(launchedWithVmix)launchVmix();desktopToken=readIni("__desktop__","token");if(desktopToken.empty()){desktopToken=randomToken();writeIni("__desktop__","token",desktopToken);}localIp=bestIpv4();if(localIp.empty())localIp="127.0.0.1";pairingPayload="vys://p?i="+localIp+"&p=9000&t="+desktopToken+"&n=vMix";if(!pairingQr.encode(pairingPayload)){pairingPayload="vys://p?i="+localIp+"&p=9000&n=vMix";pairingQr.encode(pairingPayload);}WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=wndProc;wc.hInstance=instance;wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(IDI_APP));wc.hIconSm=wc.hIcon;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=CreateSolidBrush(BG);wc.lpszClassName=L"VyStreamVmixBridgeWindow";RegisterClassExW(&wc);mainWindow=CreateWindowExW(WS_EX_APPWINDOW,wc.lpszClassName,L"VyStream Bridge for vMix 1.3.5",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_THICKFRAME,CW_USEDEFAULT,CW_USEDEFAULT,364,720,nullptr,nullptr,instance,nullptr);if(!mainWindow)return 1;ShowWindow(mainWindow,show);UpdateWindow(mainWindow);if(!startNetwork())MessageBoxW(mainWindow,L"VyStream could not open its discovery or Talkback ports. Close OBS or another VyStream bridge, then restart.",L"Network ports unavailable",MB_ICONERROR);MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}stopNetwork();return 0;}

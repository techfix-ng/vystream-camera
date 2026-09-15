#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <string>
#include <vector>

// Small offline QR encoder for byte payloads up to 78 bytes (Version 4-L).
// It intentionally supports one fixed QR profile, which keeps the OBS plugin
// self-contained and avoids sending pairing credentials to an online service.
class VystrmQrV4 {
public:
  static constexpr int size = 33;
  bool module(int x, int y) const { return cells[y][x]; }

  bool encode(const std::string &text) {
    if (text.size() > 78) return false;
    clear(); drawFunctionPatterns();
    std::vector<bool> bits;
    append(bits, 0x4, 4); append(bits, static_cast<unsigned>(text.size()), 8);
    for (unsigned char c : text) append(bits, c, 8);
    int capacity = 80 * 8;
    for (int i=0;i<4 && static_cast<int>(bits.size())<capacity;i++) bits.push_back(false);
    while (bits.size()%8) bits.push_back(false);
    std::vector<uint8_t> data;
    for (size_t i=0;i<bits.size();i+=8) { uint8_t b=0;for(int j=0;j<8;j++)b=(b<<1)|(bits[i+j]?1:0);data.push_back(b); }
    for (bool toggle=false;data.size()<80;toggle=!toggle) data.push_back(toggle?0x11:0xEC);
    auto ecc = remainder(data, 20); data.insert(data.end(), ecc.begin(), ecc.end());
    drawCodewords(data); drawFormat(); return true;
  }

private:
  bool cells[size][size]{};
  bool reserved[size][size]{};
  void clear(){for(auto &r:cells)std::fill(std::begin(r),std::end(r),false);for(auto&r:reserved)std::fill(std::begin(r),std::end(r),false);}
  static void append(std::vector<bool>&b,unsigned v,int n){for(int i=n-1;i>=0;i--)b.push_back(((v>>i)&1)!=0);}
  void set(int x,int y,bool value=true){if(x>=0&&y>=0&&x<size&&y<size){cells[y][x]=value;reserved[y][x]=true;}}
  void finder(int cx,int cy){for(int dy=-4;dy<=4;dy++)for(int dx=-4;dx<=4;dx++){int d=std::max(std::abs(dx),std::abs(dy));set(cx+dx,cy+dy,d!=2&&d!=4);}}
  void alignment(int cx,int cy){for(int dy=-2;dy<=2;dy++)for(int dx=-2;dx<=2;dx++)set(cx+dx,cy+dy,std::max(std::abs(dx),std::abs(dy))!=1);}
  void drawFunctionPatterns(){
    finder(3,3);finder(size-4,3);finder(3,size-4);
    for(int i=8;i<size-8;i++){set(i,6,i%2==0);set(6,i,i%2==0);}alignment(26,26);
    for(int i=0;i<=5;i++)set(8,i);
    set(8,7);set(8,8);set(7,8);
    for(int i=9;i<15;i++)set(14-i,8);
    for(int i=0;i<8;i++)set(size-1-i,8);
    for(int i=8;i<15;i++)set(8,size-15+i);
    set(8,size-8,true);
  }
  static uint8_t mul(uint8_t x,uint8_t y){int z=0;for(int i=7;i>=0;i--){z=(z<<1)^((z>>7)*0x11D);if((y>>i)&1)z^=x;}return static_cast<uint8_t>(z);}
  static std::vector<uint8_t> divisor(int degree){std::vector<uint8_t> r(degree);r[degree-1]=1;uint8_t root=1;for(int i=0;i<degree;i++){for(int j=0;j<degree;j++){r[j]=mul(r[j],root);if(j+1<degree)r[j]^=r[j+1];}root=mul(root,2);}return r;}
  static std::vector<uint8_t> remainder(const std::vector<uint8_t>&data,int degree){auto d=divisor(degree);std::vector<uint8_t> r(degree);for(uint8_t b:data){uint8_t factor=b^r[0];std::rotate(r.begin(),r.begin()+1,r.end());r.back()=0;for(int i=0;i<degree;i++)r[i]^=mul(d[i],factor);}return r;}
  void drawCodewords(const std::vector<uint8_t>&data){int bit=0;bool up=true;for(int right=size-1;right>=1;right-=2){if(right==6)right=5;for(int v=0;v<size;v++){int y=up?size-1-v:v;for(int j=0;j<2;j++){int x=right-j;if(reserved[y][x])continue;bool value=false;if(bit<static_cast<int>(data.size()*8))value=((data[bit>>3]>>(7-(bit&7)))&1)!=0;cells[y][x]=value^((x+y)%2==0);bit++;}}up=!up;}}
  void drawFormat(){int data=8;int rem=data;for(int i=0;i<10;i++)rem=(rem<<1)^(((rem>>9)&1)*0x537);int bits=((data<<10)|rem)^0x5412;auto b=[&](int i){return ((bits>>i)&1)!=0;};for(int i=0;i<=5;i++)set(8,i,b(i));set(8,7,b(6));set(8,8,b(7));set(7,8,b(8));for(int i=9;i<15;i++)set(14-i,8,b(i));for(int i=0;i<8;i++)set(size-1-i,8,b(i));for(int i=8;i<15;i++)set(8,size-15+i,b(i));set(8,size-8,true);}
};

// Isolated chainstate vertical slice - self-contained SHA-256 (no external deps).
// Not part of any production build; compiled only by the isolated slice harness.
#pragma once
#include <cstdint>
#include <cstring>
#include <array>
#include <string>
#include <vector>

namespace cs {

typedef std::array<uint8_t, 32> Hash;

inline uint32_t cs_rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

static const uint32_t CS_K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};

struct SHA256Ctx {
    uint32_t state[8];
    uint64_t bitlen;
    uint32_t datalen;
    uint8_t data[64];
};

inline void sha256_init(SHA256Ctx* c) {
    c->state[0]=0x6a09e667u; c->state[1]=0xbb67ae85u; c->state[2]=0x3c6ef372u; c->state[3]=0xa54ff53au;
    c->state[4]=0x510e527fu; c->state[5]=0x9b05688cu; c->state[6]=0x1f83d9abu; c->state[7]=0x5be0cd19u;
    c->bitlen=0; c->datalen=0;
}

inline void sha256_transform(SHA256Ctx* c, const uint8_t* d) {
    uint32_t m[64];
    for (uint32_t i=0,j=0;i<16;i++,j+=4)
        m[i]=((uint32_t)d[j]<<24)|((uint32_t)d[j+1]<<16)|((uint32_t)d[j+2]<<8)|((uint32_t)d[j+3]);
    for (uint32_t i=16;i<64;i++){
        uint32_t s0=cs_rotr(m[i-15],7)^cs_rotr(m[i-15],18)^(m[i-15]>>3);
        uint32_t s1=cs_rotr(m[i-2],17)^cs_rotr(m[i-2],19)^(m[i-2]>>10);
        m[i]=m[i-16]+s0+m[i-7]+s1;
    }
    uint32_t a=c->state[0],b=c->state[1],cc=c->state[2],dd=c->state[3];
    uint32_t e=c->state[4],f=c->state[5],g=c->state[6],h=c->state[7];
    for (uint32_t i=0;i<64;i++){
        uint32_t S1=cs_rotr(e,6)^cs_rotr(e,11)^cs_rotr(e,25);
        uint32_t ch=(e&f)^(~e&g);
        uint32_t t1=h+S1+ch+CS_K[i]+m[i];
        uint32_t S0=cs_rotr(a,2)^cs_rotr(a,13)^cs_rotr(a,22);
        uint32_t maj=(a&b)^(a&cc)^(b&cc);
        uint32_t t2=S0+maj;
        h=g;g=f;f=e;e=dd+t1;dd=cc;cc=b;b=a;a=t1+t2;
    }
    c->state[0]+=a;c->state[1]+=b;c->state[2]+=cc;c->state[3]+=dd;
    c->state[4]+=e;c->state[5]+=f;c->state[6]+=g;c->state[7]+=h;
}

inline void sha256_update(SHA256Ctx* c, const uint8_t* data, size_t len) {
    for (size_t i=0;i<len;i++){
        c->data[c->datalen++]=data[i];
        if (c->datalen==64){ sha256_transform(c,c->data); c->bitlen+=512; c->datalen=0; }
    }
}

inline void sha256_final(SHA256Ctx* c, uint8_t out[32]) {
    uint32_t i=c->datalen;
    if (c->datalen<56){ c->data[i++]=0x80; while(i<56)c->data[i++]=0x00; }
    else { c->data[i++]=0x80; while(i<64)c->data[i++]=0x00; sha256_transform(c,c->data); memset(c->data,0,56); }
    c->bitlen+=(uint64_t)c->datalen*8;
    for (i=0;i<8;i++){ c->data[63-i]=(uint8_t)(c->bitlen>>(8*i)); }
    sha256_transform(c,c->data);
    for (i=0;i<4;i++){
        out[i]    =(uint8_t)(c->state[0]>>(24-8*i));
        out[i+4]  =(uint8_t)(c->state[1]>>(24-8*i));
        out[i+8]  =(uint8_t)(c->state[2]>>(24-8*i));
        out[i+12] =(uint8_t)(c->state[3]>>(24-8*i));
        out[i+16] =(uint8_t)(c->state[4]>>(24-8*i));
        out[i+20] =(uint8_t)(c->state[5]>>(24-8*i));
        out[i+24] =(uint8_t)(c->state[6]>>(24-8*i));
        out[i+28] =(uint8_t)(c->state[7]>>(24-8*i));
    }
}

inline Hash sha256(const uint8_t* d, size_t n) {
    SHA256Ctx c; sha256_init(&c); sha256_update(&c,d,n);
    uint8_t h[32]; sha256_final(&c,h); Hash r; memcpy(r.data(),h,32); return r;
}
inline Hash sha256(const std::vector<uint8_t>& v){ return sha256(v.data(), v.size()); }

// Chained (streaming) hash for building state digests without materializing a buffer.
class ChainedHash {
    SHA256Ctx c_;
public:
    ChainedHash(){ sha256_init(&c_); }
    void put(const uint8_t* d, size_t n){ sha256_update(&c_, d, n); }
    void put(const std::vector<uint8_t>& v){ if(!v.empty()) sha256_update(&c_, v.data(), v.size()); }
    Hash final() const { SHA256Ctx t=c_; uint8_t h[32]; sha256_final(&t,h); Hash r; memcpy(r.data(),h,32); return r; }
};

inline std::string hex(const Hash& h){
    static const char* x="0123456789abcdef"; std::string s; s.reserve(64);
    for (int i=0;i<32;i++){ s.push_back(x[h[i]>>4]); s.push_back(x[h[i]&15]); }
    return s;
}
inline std::string hex(const uint8_t* d, size_t n){
    static const char* x="0123456789abcdef"; std::string s; s.reserve(n*2);
    for (size_t i=0;i<n;i++){ s.push_back(x[d[i]>>4]); s.push_back(x[d[i]&15]); }
    return s;
}

} // namespace cs

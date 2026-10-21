#include "wire.hpp"
#include <array>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
int main(int argc,char**argv){std::array<std::byte,1472>buf;hp::Event e{7,9,3,123456,987654321,17,2,4};size_t n=hp::encode(buf.data(),buf.size(),e,256);assert(n==328&&hp::valid(buf.data(),n));assert(hp::r64(buf.data()+40)==123456);assert(!hp::valid(buf.data(),71));auto write=[&](const char*name){if(argc<2)return;std::filesystem::create_directories(argv[1]);std::ofstream f(std::string(argv[1])+"/"+name,std::ios::binary);f.write(reinterpret_cast<char*>(buf.data()),n);};write("valid.bin");hp::u16(buf.data()+4,2);assert(!hp::valid(buf.data(),n));write("bad-version.bin");hp::u16(buf.data()+4,1);hp::u32(buf.data()+68,257);assert(!hp::valid(buf.data(),n));write("bad-length.bin");hp::u32(buf.data()+68,256);hp::u16(buf.data()+6,65535);assert(!hp::valid(buf.data(),n));write("bad-header.bin");hp::u16(buf.data()+6,72);hp::u32(buf.data(),0);assert(!hp::valid(buf.data(),n));write("bad-magic.bin");std::cout<<"protocol tests PASS\n";}

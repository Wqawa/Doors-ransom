










#pragma once

#include <string>
#include <vector>

namespace assets {




enum Kind
{
    KIND_ROOT = 0,
    KIND_IMAGE,
    KIND_AUDIO,
    KIND_COUNT
};


struct Blob
{
    std::vector<unsigned char> bytes;

    bool   Ok()   const { return !bytes.empty(); }
    const unsigned char* Data() const { return bytes.empty() ? nullptr : bytes.data(); }
    size_t Size() const { return bytes.size(); }
};



void         UseDiskDir(Kind k, const wchar_t* dir);
bool         DiskMode(Kind k);
std::wstring DiskDir(Kind k);



bool Get(Kind k, const wchar_t* name, Blob& out);



std::wstring FindFirst(Kind k, const wchar_t* pattern);


std::wstring Where(Kind k, const wchar_t* name);


int EmbeddedCount();


std::wstring ExeDir();

}

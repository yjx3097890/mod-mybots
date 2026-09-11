#ifndef MYBOTS_UTIL_H
#define MYBOTS_UTIL_H

#include "Define.h"

#include <atomic>
#include <ctime>
#include <cstdio>
#include <string>

inline void MyBotsJsonEscape(std::string const& in, std::string& out)
{
    out.clear();
    out.reserve(in.size() + 8);
    for (char c : in)
    {
        switch (c)
        {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
}

inline std::string MyBotsJsonEscapeCopy(std::string const& in)
{
    std::string out;
    MyBotsJsonEscape(in, out);
    return out;
}

inline std::string MyBotsNewId(char const* prefix)
{
    static std::atomic<uint32> seq{0};
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%s-%u-%u", prefix ? prefix : "id",
        static_cast<uint32>(time(nullptr)), ++seq);
    return std::string(buf);
}

inline uint32 MyBotsNow()
{
    return static_cast<uint32>(time(nullptr));
}

#endif

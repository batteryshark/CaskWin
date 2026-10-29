// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <cstdio>
#include <string>
namespace game_audio {
class RenderWorkload {
    struct Impl;
    Impl *impl_;
public:
    explicit RenderWorkload(bool render);
    ~RenderWorkload();
    void frame(long long begin,long long finish,long long frequency);
    void report(const std::wstring &stem,FILE *json);
    RenderWorkload(const RenderWorkload&)=delete;
    RenderWorkload& operator=(const RenderWorkload&)=delete;
};
}

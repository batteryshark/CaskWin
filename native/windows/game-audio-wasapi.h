// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <windows.h>
#include <cstdio>
#include <string>
namespace game_audio {
struct WasapiSummary {
    bool complete=false;
    unsigned clients=0,submissions=0,padding_calls=0,empty_padding=0,clock_samples=0,errors=0,dropped=0;
    double submitted_seconds=0,max_submit_gap_ms=0,max_clock_observer_ms=0;
};
// Only the fixture's copied SDL IAT changes. Forwarding COM objects preserve
// actual native audio-client calls and do not modify requested buffer sizes.
class WasapiTap {
    void **slot_=nullptr,*original_=nullptr;
public:
    WasapiTap(HMODULE sdl,bool enabled);
    ~WasapiTap();
    WasapiTap(const WasapiTap&)=delete;
    WasapiTap& operator=(const WasapiTap&)=delete;
};
void wasapi_measurement_window(long long begin,long long finish,long long qpc_frequency);
WasapiSummary wasapi_report(const std::wstring &stem,FILE *json,bool expected);
}

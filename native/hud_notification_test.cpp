#include "heavymp_content.cpp"
#include <cassert>

namespace {
std::string submittedText;
unsigned submittedIcon = 99, ticks = 0;
void fakeSubmit(Ptr, const char** text, unsigned icon, Ptr, unsigned priority, unsigned short flags) {
    assert(priority == 2 && flags == 0xffff);
    submittedText = *text;
    submittedIcon = icon;
}
void fakeTick(Ptr) {
    ++ticks;
}
} // namespace

int main() {
    assert(!enqueueNativeNotice(L"Inactive", true));
    nativeNoticeReady = true;
    nativeHudEngine = reinterpret_cast<Ptr>(&fakeSubmit) - 0x622fa0;
    originalNoticeTick = fakeTick;
    std::array<unsigned char, 96> manager{};
    manager[0x38] = manager[0x3b] = 1;
    const Ptr hud = reinterpret_cast<Ptr>(manager.data());
    assert(enqueueNativeNotice(L"Official <notice>", true));
    assert(!enqueueNativeNotice(L"Official <notice>", true));
    nativeNoticeTick(hud);
    assert(ticks == 1 && submittedText == "Official  notice " && submittedIcon == 2);
    assert(enqueueNativeNotice(L"Multiplayer warning", false));
    nativeNoticeTick(hud);
    assert(ticks == 2 && submittedText == "Multiplayer warning" && submittedIcon == 1 &&
           nativeNotices.empty());
    puts("PASS: multiplayer announcements use the stock HUD and sanitize native markup.");
}

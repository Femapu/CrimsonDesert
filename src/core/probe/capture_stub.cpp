// QuattroStagioni - generic capture detour stub (x64 encoding).
//
// Stack at entry: rsp % 16 == 8 (return address pushed). 7 pushes (56 bytes) make it 0 mod 16,
// "sub rsp, 0x60" keeps it aligned and reserves 0x20 shadow space + 4 x 16 bytes for xmm0-xmm3.
// The caller's return address is then at [rsp + 0x60 + 56] = [rsp + 0x98].
#include "capture_stub.h"

#include <cstring>

namespace qs::probe {
namespace {

void emit(std::vector<uint8_t>& c, std::initializer_list<uint8_t> b) { c.insert(c.end(), b); }
void emit64(std::vector<uint8_t>& c, uint64_t v) {
    for (int i = 0; i < 8; ++i) c.push_back(uint8_t(v >> (8 * i)));
}

}  // namespace

CaptureStub buildCaptureStub(uint64_t ctx, uint64_t callback, uint64_t original) {
    CaptureStub s;
    std::vector<uint8_t>& c = s.code;
    emit(c, {0x51});                    // push rcx
    emit(c, {0x52});                    // push rdx
    emit(c, {0x41, 0x50});              // push r8
    emit(c, {0x41, 0x51});              // push r9
    emit(c, {0x50});                    // push rax
    emit(c, {0x41, 0x52});              // push r10
    emit(c, {0x41, 0x53});              // push r11
    emit(c, {0x48, 0x83, 0xEC, 0x60});  // sub rsp, 0x60
    emit(c, {0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20});  // movdqu [rsp+0x20], xmm0
    emit(c, {0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30});  // movdqu [rsp+0x30], xmm1
    emit(c, {0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40});  // movdqu [rsp+0x40], xmm2
    emit(c, {0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50});  // movdqu [rsp+0x50], xmm3
    emit(c, {0x49, 0x89, 0xD1});        // mov r9, rdx            (4th arg: original rdx)
    emit(c, {0x48, 0x89, 0xCA});        // mov rdx, rcx           (2nd arg: original rcx = this)
    emit(c, {0x4C, 0x8B, 0x84, 0x24, 0x98, 0x00, 0x00, 0x00});  // mov r8, [rsp+0x98] (3rd: return address)
    emit(c, {0x48, 0xB9});              // mov rcx, imm64         (1st arg: ctx)
    emit64(c, ctx);
    emit(c, {0x48, 0xB8});              // mov rax, imm64
    emit64(c, callback);
    emit(c, {0xFF, 0xD0});              // call rax
    emit(c, {0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20});  // movdqu xmm0, [rsp+0x20]
    emit(c, {0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30});  // movdqu xmm1, [rsp+0x30]
    emit(c, {0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40});  // movdqu xmm2, [rsp+0x40]
    emit(c, {0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50});  // movdqu xmm3, [rsp+0x50]
    emit(c, {0x48, 0x83, 0xC4, 0x60});  // add rsp, 0x60
    emit(c, {0x41, 0x5B});              // pop r11
    emit(c, {0x41, 0x5A});              // pop r10
    emit(c, {0x58});                    // pop rax
    emit(c, {0x41, 0x59});              // pop r9
    emit(c, {0x41, 0x58});              // pop r8
    emit(c, {0x5A});                    // pop rdx
    emit(c, {0x59});                    // pop rcx
    emit(c, {0xFF, 0x25, 0, 0, 0, 0});  // jmp [rip+0]
    s.originalSlotOffset = c.size();
    emit64(c, original);
    return s;
}

void setStubOriginal(uint8_t* code, const CaptureStub& layout, uint64_t original) {
    std::memcpy(code + layout.originalSlotOffset, &original, 8);
}

}  // namespace qs::probe

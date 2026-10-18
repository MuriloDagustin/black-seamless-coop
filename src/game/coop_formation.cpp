// Formation destinations (0x1A0050): a controller's record at +0x1EC0 holds
// an anchor character at +0x10 and its formation at +0x30. A native rebuild
// (0x18FC10) can leave the anchor null while the formation still exists. Its
// consumers call this helper without checking, crashing at guest 0x1A0086.
// Windows reports from two release builds identify that exact instruction.
#include <cstring>

#include "core/log.h"
#include "game/layout.h"
#include "kernel/kernel.h"

using namespace kernel;

namespace {

bool Mapped(uint32_t address, uint32_t size) {
    return address && address <= UINT32_MAX - size && mem::IsCommitted(address) &&
           mem::IsCommitted(address + size - 1);
}

bool Character(uint32_t ch) {
    uint32_t world = R32(layout::kWorld);
    if (world && ch == world + layout::kWorldPlayer) return true;
    uint32_t pool = R32(0x2D1980), first = pool + 0xA0;
    return pool && ch >= first && ch - first < 32 * 0x3C0 && (ch - first) % 0x3C0 == 0 &&
           R32(ch + 0x384) == 0 && (R32(ch + 8) || R32(ch + 12));
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_001A0050);
X86_FUNC(sub_001A0050) {
    uint32_t record = R32(ctx.esp + 4), out = R32(ctx.esp + 8);
    // Preserve the native calling convention and output pointer, including in
    // a deferred formation action after its anchor was removed.
    if (!Mapped(record, 0x34) || !Mapped(out, 16)) {
        __imp__sub_001A0050(ctx, base);
        return;
    }
    uint32_t anchor = R32(record + 0x10), group = R32(record + 0x30);
    uint32_t leader = Mapped(group, 0x40) ? R32(group + 0x10) : 0;
    if (Character(anchor) && Character(leader)) {
        __imp__sub_001A0050(ctx, base);
        return;
    }
    static uint32_t reports = 0;
    if (reports++ < 16)
        LOGR("coop: expired formation reference: record=%08X anchor=%08X group=%08X leader=%08X caller=%08X",
             record, anchor, group, leader, R32(ctx.esp));
    if (Character(leader)) {
        // Follow the surviving formation leader. Keep native spacing, heading
        // and destination calculation, without modifying the stored links.
        W32(record + 0x10, leader);
        __imp__sub_001A0050(ctx, base);
        W32(record + 0x10, anchor);
        return;
    }
    // The whole formation is gone: hold the follower's position until its next
    // native AI update selects another action. Never dereference the old group.
    uint32_t ctl = R32(record), own = Mapped(ctl, 0x90) ? R32(ctl + 0x8C) : 0;
    if (Character(own)) std::memcpy(HostPtr(out), HostPtr(own + 0xA0), 16);
    else std::memset(HostPtr(out), 0, 16);
    Return(ctx, out, 2);
}

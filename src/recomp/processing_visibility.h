#pragma once
#include "recomp/ctx.h"
#include <vector>
namespace recomp::processing {
class MachineDrawScope {
    struct Saved {u32 actor,flags,type,vtable;};
    std::vector<Saved> saved_;
public:
    explicit MachineDrawScope(unsigned pass);
    ~MachineDrawScope();
    void verifyCurrentView() const;
    MachineDrawScope(const MachineDrawScope&)=delete;
    MachineDrawScope& operator=(const MachineDrawScope&)=delete;
};
}

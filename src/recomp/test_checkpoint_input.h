#pragma once
namespace recomp::checkpoints {
enum class ClickAction {None,Capture,Reload};
struct ClickState {
    bool left=false,right=false,chord=false;
    ClickAction update(bool l,bool r,bool enabled,bool menu){
        if(l&&r)chord=true;
        ClickAction action=ClickAction::None;
        if(enabled&&!menu&&!chord){
            if(left&&!l&&!r)action=ClickAction::Capture;
            if(right&&!r&&!l)action=ClickAction::Reload;
        }
        left=l;right=r;if(!l&&!r)chord=false;
        return action;
    }
};
}

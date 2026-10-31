#include "recomp/test_checkpoint_input.h"
#include <stdexcept>
#include "recomp/test_checkpoint_element.h"
#include <limits>
using namespace recomp::checkpoints;
void expect(ClickAction a,ClickAction b){if(a!=b)throw std::runtime_error("Checkpoint shortcut regression");}
int main(){
 for(unsigned type=1;type<=4;++type){
  ElementState state{type,37.25f,true};const auto tail=elementTail(state,12345);
  const auto restored=decodeElement(tail,12345);
  if(!restored.available||restored.type!=type||restored.supply!=state.supply)throw std::runtime_error("Element checkpoint round trip");
  if(decodeElement(tail,67890).available)throw std::runtime_error("Element from wrong checkpoint accepted");
 }
 for(auto state:{ElementState{5,1,true},ElementState{2,-1,true},ElementState{3,std::numeric_limits<float>::quiet_NaN(),true}})
  if(decodeElement(elementTail(state,1),1).available)throw std::runtime_error("Invalid element snapshot accepted");
 if(decodeElement({},1).available)throw std::runtime_error("Missing legacy element extension accepted");

 ClickState s;
 expect(s.update(true,false,true,false),ClickAction::None);
 expect(s.update(true,false,true,false),ClickAction::None);
 expect(s.update(false,false,true,false),ClickAction::Capture);
 expect(s.update(false,false,true,false),ClickAction::None);
 expect(s.update(false,true,true,false),ClickAction::None);
 expect(s.update(false,false,true,false),ClickAction::Reload);
 for(bool rightFirst:{false,true}){
  s={};expect(s.update(!rightFirst,rightFirst,true,false),ClickAction::None);
  expect(s.update(true,true,true,true),ClickAction::None);
  expect(s.update(rightFirst,!rightFirst,true,false),ClickAction::None);
  expect(s.update(false,false,true,false),ClickAction::None);
 }
 for(bool left:{false,true}){
  s={};expect(s.update(left,!left,false,false),ClickAction::None);
  expect(s.update(false,false,false,false),ClickAction::None);
  expect(s.update(left,!left,true,true),ClickAction::None);
  expect(s.update(false,false,true,true),ClickAction::None);
 }
}

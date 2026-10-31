#include "recomp/vr_avatar.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <map>
#include <set>
namespace recomp::vr {
bool reflectedActorPass(unsigned flags) {return (flags&0x10000u)!=0;}
bool mirroredTransform(const Matrix& matrix) {
    // Normalize each column so small/nonuniform model scales do not turn a
    // genuine reflection into an almost-zero determinant.
    double basis[3][3]{};
    for(unsigned col=0;col<3;++col) {
        double length2=0;
        for(unsigned row=0;row<3;++row){const double value=matrix[row*4+col];if(!std::isfinite(value))return false;length2+=value*value;}
        if(length2<1e-24)return false;
        const double length=std::sqrt(length2);
        for(unsigned row=0;row<3;++row)basis[row][col]=matrix[row*4+col]/length;
    }
    const double det=basis[0][0]*(basis[1][1]*basis[2][2]-basis[1][2]*basis[2][1])
        -basis[0][1]*(basis[1][0]*basis[2][2]-basis[1][2]*basis[2][0])
        +basis[0][2]*(basis[1][0]*basis[2][1]-basis[1][1]*basis[2][0]);
    return det < -1e-6;
}
Vec3 worldGaze(const Matrix& head,float heading) {
    const float c=std::cos(heading),s=std::sin(heading);
    return {-c*head[2]-s*head[10],-head[6],s*head[2]-c*head[10]};
}

namespace {
Vec3 add(Vec3 a,Vec3 b) {for(int i=0;i<3;++i)a[i]+=b[i];return a;}
Vec3 sub(Vec3 a,Vec3 b) {for(int i=0;i<3;++i)a[i]-=b[i];return a;}
Vec3 mul(Vec3 a,float s) {for(float& v:a)v*=s;return a;}
float dot(Vec3 a,Vec3 b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
float length(Vec3 a) {return std::sqrt(dot(a,a));}
bool finite(Vec3 a) {return std::isfinite(a[0])&&std::isfinite(a[1])&&std::isfinite(a[2]);}
Vec3 cross(Vec3 a,Vec3 b) {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
}
ArmPose solveArm(Vec3 shoulder,Vec3 wrist,Vec3 bendHint,float upperLength,float lowerLength) {
    ArmPose out;
    if(!finite(shoulder)||!finite(wrist)||!finite(bendHint)||!std::isfinite(upperLength)||!std::isfinite(lowerLength)||upperLength<0.01f||lowerLength<0.01f) return out;
    auto direction=sub(wrist,shoulder);const float reach=length(direction);
    direction=reach>0.00001f?mul(direction,1/reach):Vec3{0,0,-1};
    const float d=std::clamp(reach,std::fabs(upperLength-lowerLength)+0.00001f,upperLength+lowerLength-0.00001f);
    auto bend=sub(bendHint,mul(direction,dot(bendHint,direction)));
    if(length(bend)<0.00001f) bend=cross(direction,std::fabs(direction[1])<0.9f?Vec3{0,1,0}:Vec3{1,0,0});
    bend=mul(bend,1/length(bend));
    const float along=(upperLength*upperLength-lowerLength*lowerLength+d*d)/(2*d);
    const float height=std::sqrt(std::max(0.0f,upperLength*upperLength-along*along));
    out.shoulder=shoulder;out.wrist=add(shoulder,mul(direction,d));
    out.elbow=add(add(shoulder,mul(direction,along)),mul(bend,height));out.valid=true;
    return out;
}
Vec3 shoulderTarget(Vec3 root,Vec3 shoulder,Vec3 wrist,float armLength,float maxShift) {
    if(!finite(root)||!finite(shoulder)||!finite(wrist)||!std::isfinite(armLength)||!std::isfinite(maxShift)||armLength<=0||maxShift<=0) return shoulder;
    const auto collar=sub(shoulder,root);const float radius=length(collar);
    if(radius<0.0001f) return shoulder;
    auto reach=sub(wrist,shoulder);const float d=length(reach);
    if(d<0.0001f) return shoulder;
    // No shoulder drift at rest. Reach progressively recruits the clavicle.
    const float effort=std::clamp((d/armLength-0.55f)/0.45f,0.0f,1.0f);
    auto desired=add(collar,mul(reach,maxShift*effort/d));
    desired=mul(desired,radius/length(desired));
    // Remain on the clavicle sphere and cap displacement, including extreme inputs.
    float lo=0,hi=1;
    for(int i=0;i<18;++i) {
        const float t=(lo+hi)*0.5f;
        auto trial=add(collar,mul(sub(desired,collar),t));trial=mul(trial,radius/length(trial));
        if(length(sub(trial,collar))<=maxShift)lo=t;else hi=t;
    }
    auto out=add(collar,mul(sub(desired,collar),lo));out=mul(out,radius/length(out));
    return add(root,out);
}
ToolTarget controllerToolTarget(const Matrix& grip,const Matrix& aim,Vec3 actor,float heading,float eyeHeight,float eyeForward,float units,std::array<float,2> fit,float muzzleOffset) {
    ToolTarget target;
    if(!finite(actor)||!std::isfinite(heading)||!std::isfinite(units)||units<=0||!std::isfinite(eyeHeight)||!std::isfinite(eyeForward)||!std::isfinite(fit[0])||!std::isfinite(fit[1])||!std::isfinite(muzzleOffset))return target;
    for(float v:grip)if(!std::isfinite(v))return target;
    for(float v:aim)if(!std::isfinite(v))return target;
    const auto yaw=poseMatrix({{0,std::sin(heading/2),0,std::cos(heading/2)}, {}});
    auto rotation=multiply(yaw,aim);
    target.direction={-rotation[2],-rotation[6],-rotation[10]};target.up={rotation[1],rotation[5],rotation[9]};
    const float d=length(target.direction),u=length(target.up);
    if(d<0.5f||u<0.5f)return target;
    target.direction=mul(target.direction,1/d);target.up=mul(target.up,1/u);
    const Vec3 local{grip[3],grip[7]+fit[0],grip[11]-fit[1]-eyeForward};
    for(unsigned r=0;r<3;++r){target.origin[r]=actor[r]+(r==1?eyeHeight*units:0)+target.direction[r]*muzzleOffset*units;for(unsigned c=0;c<3;++c)target.origin[r]+=yaw[r*4+c]*local[c]*units;}
    target.tracked=finite(target.origin);return target;
}
Matrix alignTool(Vec3 origin,Vec3 direction,Vec3 up,const ToolTarget& target) {
    const float size=length(direction);if(!finite(origin)||!finite(direction)||size<0.00001f)return poseMatrix({});
    return limbTransform(origin,add(origin,mul(direction,1/size)),up,target.origin,add(target.origin,target.direction),target.up);
}
ArmPose solveReflectedArm(Vec3 collar,Vec3 shoulder,Vec3 elbow,Vec3 wrist,Vec3 target,Vec3 bendHint) {
    const float upper=length(sub(elbow,shoulder)),lower=length(sub(wrist,elbow)),sum=upper+lower;
    if(!std::isfinite(sum)||upper<0.01f||lower<0.01f)return {};
    const float shift=std::min(sum*0.15f,length(sub(shoulder,collar))*0.5f);
    const auto recruited=shoulderTarget(collar,shoulder,target,sum,shift);
    const float stretch=std::clamp(length(sub(target,recruited))/sum,1.0f,1.4f);
    return solveArm(recruited,target,bendHint,upper*stretch,lower*stretch);
}
Matrix rotationAboutPoint(Vec3 pivot,const Matrix& rotation) {
    auto out=rotation;
    for(unsigned r=0;r<3;++r){out[r*4+3]=pivot[r];for(unsigned c=0;c<3;++c)out[r*4+3]-=rotation[r*4+c]*pivot[c];}
    return out;
}
bool startlePoseState(unsigned state,unsigned substate,unsigned reactionKind) {
    return state==2 && substate==3 && reactionKind==5;
}
bool pickupPoseState(unsigned state,unsigned substate) {
    return state==2 && (substate==0x21 || substate==0x22);
}
Matrix avatarBodyAlignment(const Matrix& worldFromModel,Vec3 actorPosition,float actorYaw,float viewYaw) {
    Matrix inverse;
    if(!invertAffine(worldFromModel,inverse)) return poseMatrix({});
    const float yaw=std::remainder(viewYaw-actorYaw,6.283185307f);
    auto turn=poseMatrix({{0,std::sin(yaw*0.5f),0,std::cos(yaw*0.5f)}, {}});
    for(int r=0;r<3;++r) turn[r*4+3]=actorPosition[r]-(turn[r*4]*actorPosition[0]+turn[r*4+1]*actorPosition[1]+turn[r*4+2]*actorPosition[2]);
    return multiply(inverse,multiply(turn,worldFromModel));
}
Matrix calibratedHand(const Matrix& controller,unsigned hand,float outwardRadians) {
    return calibratedHand(controller,hand,outwardRadians,hand?-1.0611601852f:1.0611601852f);
}
Matrix calibratedHand(const Matrix& controller,unsigned hand,float outwardRadians,float rollRadians) {
    const float yaw=hand?-outwardRadians:outwardRadians;
    // User calibration viewed wrist-to-fingers: left counterclockwise,
    // right clockwise. Keep tools in the same calibrated grip frame.
    const auto turn=poseMatrix({{0,std::sin(yaw*0.5f),0,std::cos(yaw*0.5f)}, {}});
    const float halfRoll=rollRadians*0.5f;
    const auto roll=poseMatrix({{0,0,std::sin(halfRoll),std::cos(halfRoll)}, {}});
    return multiply(controller,multiply(turn,roll));
}
Matrix equipmentGripFrame(const Matrix& grip,const Matrix& aim,unsigned hand,float outwardRadians,float palmReachMetres,float palmClearanceMetres) {
    const auto glove=calibratedHand(grip,hand,outwardRadians);auto target=aim;
    for(unsigned r=0;r<3;++r)target[r*4+3]=glove[r*4+3]-glove[r*4+2]*palmReachMetres-glove[r*4+1]*palmClearanceMetres;
    return target;
}
Matrix equipmentContactFrame(const Matrix& grip,const Matrix& aim,unsigned hand,float outwardRadians,Vec3 contactMetres) {
    return equipmentContactFrame(grip,aim,hand,outwardRadians,contactMetres,hand?-1.0611601852f:1.0611601852f);
}
Matrix equipmentContactFrame(const Matrix& grip,const Matrix& aim,unsigned hand,float outwardRadians,Vec3 contactMetres,float rollRadians) {
    const auto glove=calibratedHand(grip,hand,outwardRadians,rollRadians);auto out=glove;
    (void)aim; // The held model and glove share grip space; aim space is not a hand pose.
    for(unsigned r=0;r<3;++r){out[r*4+3]=glove[r*4+3];for(unsigned k=0;k<3;++k)out[r*4+3]+=glove[r*4+k]*contactMetres[k];}
    return out;
}
Matrix toolHoldingHand(const Matrix& target,Vec3 contact,unsigned hand) {
    // A curled finger chain encloses a cylinder along glove X, not along
    // its finger-forward Z. Turn that cylinder onto the barrel axis while
    // keeping its measured centre stationary in the tool's grip frame.
    const float yaw=hand?-1.570796327f:1.570796327f;
    auto turn=poseMatrix({{0,std::sin(yaw/2),0,std::cos(yaw/2)}, {}});
    for(unsigned r=0;r<3;++r){turn[r*4+3]=contact[r];for(unsigned k=0;k<3;++k)turn[r*4+3]-=turn[r*4+k]*contact[k];}
    return multiply(target,turn);
}
Vec3 closedFingerGripCentre(std::array<Matrix,3> chain,Vec3 backNormal) {
    curlFinger(chain,backNormal,0.8f);
    const Vec3 a{chain[0][3],chain[0][7],chain[0][11]},b{chain[1][3],chain[1][7],chain[1][11]},c{chain[2][3],chain[2][7],chain[2][11]};
    const auto u=sub(b,a),v=sub(c,a),n=cross(u,v);const float n2=dot(n,n);
    if(n2<0.000001f)return mul(add(a,c),0.5f);
    return add(a,mul(add(mul(cross(v,n),dot(u,u)),mul(cross(n,u),dot(v,v))),1/(2*n2)));
}
bool equipmentHandleProjection(const Matrix& mesh,Vec3 contact,float& handleY,float& residual) {
    const Vec3 origin{mesh[3],mesh[7],mesh[11]},axis{mesh[1],mesh[5],mesh[9]};
    const float axis2=dot(axis,axis);
    if(!finite(origin)||!finite(axis)||!finite(contact)||!std::isfinite(axis2)||axis2<0.000001f)return false;
    const auto offset=sub(contact,origin);
    handleY=dot(offset,axis)/axis2;
    residual=length(sub(offset,mul(axis,handleY)));
    return std::isfinite(handleY)&&std::isfinite(residual);
}
Matrix reanchorGripInverse(const Matrix& worldToHand,Vec3 handleWorld) {
    auto result=worldToHand;
    for(unsigned r=0;r<3;++r){result[r*4+3]=0;for(unsigned k=0;k<3;++k)result[r*4+3]-=result[r*4+k]*handleWorld[k];}
    return result;
}
Matrix equipmentMeshGripInverse(const Matrix& mesh,float handleY) {
    Vec3 handle{},along{};
    for(unsigned r=0;r<3;++r){handle[r]=mesh[r*4+3]+mesh[r*4+1]*handleY;along[r]=handle[r]+mesh[r*4+1];}
    return handTransform(handle,along,{mesh[2],mesh[6],mesh[10]},poseMatrix({}));
}
void DoorPassage::opened(unsigned room,float signedDistance) {entry_=room;side_=signedDistance<0?-1.f:1.f;clear_=0;}
bool DoorPassage::step(unsigned room,unsigned otherRoom,float signedDistance,std::span<const float> hands,float seconds) {
    if(entry_==255||otherRoom==entry_||room!=otherRoom||!std::isfinite(signedDistance)||!std::isfinite(seconds)||seconds<=0||seconds>0.1f||signedDistance*side_>-200||hands.size()!=2){clear_=0;return false;}
    for(float distance:hands)if(!std::isfinite(distance)||distance*side_>-30){clear_=0;return false;}
    clear_+=seconds;if(clear_<0.5f)return false;entry_=255;return true;
}
bool doorThresholdContactDepth(float y,float radius,float floor,float& depth) {
    if(!std::isfinite(y)||!std::isfinite(radius)||!std::isfinite(floor)||radius<1||radius>100)return false;
    const float delta=floor-(y-radius);if(delta< -0.25f||delta>8)return false;
    depth=std::max(0.f,delta);return true;
}
bool doorThresholdHeight(float a,float b,float placement,float& height) {
    if(!std::isfinite(a)||!std::isfinite(b)||!std::isfinite(placement)||std::fabs(a-b)>2||std::fabs(a-placement)>4||std::fabs(b-placement)>4)return false;
    height=(a+b)*0.5f;return std::isfinite(height);
}
bool doorApertureAllows(const Matrix& base,Vec3 center,Vec3 n,Vec3 v,Vec3 p) {
    if(!finite(center)||!finite(n)||!finite(v)||!finite(p))return false;
    for(float f:base)if(!std::isfinite(f))return false;
    const Vec3 tangent{base[0],0,base[8]},axis{base[2],0,base[10]};
    if(std::fabs(length(tangent)-1)>0.01f||std::fabs(length(axis)-1)>0.01f||std::fabs(dot(tangent,axis))>0.01f||std::fabs(length(n)-1)>0.01f||std::fabs(n[1])>0.05f)return false;
    const auto relative=sub(p,center);
    return std::fabs(dot(n,axis))>=0.99f&&std::fabs(dot(sub(v,center),axis))<=50&&std::fabs(dot(relative,tangent))<=55&&std::fabs(dot(relative,axis))<=180&&relative[1]>=0&&relative[1]<=240;
}
bool retargetHose(std::span<const Vec3> native,const Matrix& packDelta,const Matrix& nozzleDelta,std::vector<Vec3>& result) {
    if(native.size()<3||native.size()>128)return false;
    float rest=0;for(size_t i=0;i<native.size();++i){if(!finite(native[i]))return false;if(i)rest+=length(sub(native[i],native[i-1]));}
    auto point=[](const Matrix& m,Vec3 p){Vec3 out{};for(unsigned r=0;r<3;++r){out[r]=m[r*4+3];for(unsigned k=0;k<3;++k)out[r]+=m[r*4+k]*p[k];}return out;};
    const auto a=point(packDelta,native.front()),b=point(nozzleDelta,native.back());
    auto start=sub(point(packDelta,native[1]),a),end=sub(b,point(nozzleDelta,native[native.size()-2]));
    const float ls=length(start),le=length(end),gap=length(sub(b,a));
    if(!finite(a)||!finite(b)||!std::isfinite(rest)||rest<0.001f||ls<0.00001f||le<0.00001f||gap>rest*4)return false;
    const float control=std::clamp(rest*0.28f,gap*0.18f,std::max(gap*0.18f,rest*0.4f));
    const auto c=add(a,mul(start,control/ls)),d=sub(b,mul(end,control/le));
    std::array<Vec3,97> samples{};std::array<float,97> arc{};
    const float sag=std::min(rest*0.18f,std::max(0.f,rest-gap)*0.35f);
    for(unsigned i=0;i<samples.size();++i){const float t=float(i)/(samples.size()-1),u=1-t;
        samples[i]=add(add(mul(a,u*u*u),mul(c,3*u*u*t)),add(mul(d,3*u*t*t),mul(b,t*t*t)));
        samples[i][1]-=16*sag*t*t*u*u;
        if(!finite(samples[i]))return false;
        if(i)arc[i]=arc[i-1]+length(sub(samples[i],samples[i-1]));
    }
    if(!std::isfinite(arc.back())||arc.back()<0.00001f)return false;
    std::vector<Vec3> output(native.size());output.front()=a;output.back()=b;unsigned j=1;
    for(size_t i=1;i+1<output.size();++i){const float target=arc.back()*float(i)/(output.size()-1);while(j+1<arc.size()&&arc[j]<target)++j;
        const float f=(target-arc[j-1])/std::max(arc[j]-arc[j-1],0.000001f);output[i]=add(samples[j-1],mul(sub(samples[j],samples[j-1]),f));}
    result=std::move(output);return true;
}
Matrix boneTransform(Vec3 oldStart,Vec3 oldEnd,Vec3 newStart,Vec3 newEnd) {
    auto result=poseMatrix({});
    auto a=sub(oldEnd,oldStart),b=sub(newEnd,newStart);
    const float la=length(a),lb=length(b);
    if(!finite(oldStart)||!finite(newStart)||!std::isfinite(la)||!std::isfinite(lb)||la<0.00001f||lb<0.00001f) return result;
    a=mul(a,1/la);b=mul(b,1/lb);const float c=std::clamp(dot(a,b),-1.0f,1.0f);
    if(c<-0.9999f) {
        auto axis=cross(a,std::fabs(a[1])<0.9f?Vec3{0,1,0}:Vec3{1,0,0});axis=mul(axis,1/length(axis));
        for(int r=0;r<3;++r) for(int col=0;col<3;++col) result[r*4+col]=2*axis[r]*axis[col]-(r==col?1.0f:0.0f);
    } else {
        const auto v=cross(a,b);const float k[9]{0,-v[2],v[1],v[2],0,-v[0],-v[1],v[0],0};
        for(int r=0;r<3;++r) for(int col=0;col<3;++col) {
            float square=0;for(int i=0;i<3;++i)square+=k[r*3+i]*k[i*3+col];
            result[r*4+col]=(r==col?1.0f:0.0f)+k[r*3+col]+square/(1+c);
        }
    }
    for(int r=0;r<3;++r) result[r*4+3]=newStart[r]-(result[r*4]*oldStart[0]+result[r*4+1]*oldStart[1]+result[r*4+2]*oldStart[2]);
    return result;
}
Matrix detachedForearmSkin(Vec3 elbow,Vec3 wrist,const Matrix& target,float sleeveLength) {
    const Vec3 end{target[3],target[7],target[11]};Vec3 start=end;
    for(unsigned r=0;r<3;++r)start[r]+=target[r*4+2]*sleeveLength;
    return limbTransform(elbow,wrist,{0,0,1},start,end,{target[1],target[5],target[9]});
}
Matrix limbTransform(Vec3 oldStart,Vec3 oldEnd,Vec3 oldNormal,Vec3 newStart,Vec3 newEnd,Vec3 newNormal) {
    const auto frame=[](Vec3 a,Vec3 b,Vec3 normal) {
        auto x=sub(b,a);const float size=length(x);auto out=poseMatrix({});
        if(size<0.00001f||!std::isfinite(size)) return out;
        x=mul(x,1/size);auto z=sub(normal,mul(x,dot(normal,x)));
        if(length(z)<0.00001f) z=cross(x,std::fabs(x[1])<0.9f?Vec3{0,1,0}:Vec3{1,0,0});
        z=mul(z,1/length(z));const auto y=cross(z,x);
        for(int r=0;r<3;++r) {out[r*4]=x[r];out[r*4+1]=y[r];out[r*4+2]=z[r];out[r*4+3]=a[r];}
        return out;
    };
    Matrix inverse;
    if(!invertAffine(frame(oldStart,oldEnd,oldNormal),inverse)) return boneTransform(oldStart,oldEnd,newStart,newEnd);
    auto desired=frame(newStart,newEnd,newNormal);
    const float oldLength=length(sub(oldEnd,oldStart)),newLength=length(sub(newEnd,newStart));
    if(oldLength>0.00001f) for(int r=0;r<3;++r) desired[r*4]*=newLength/oldLength;
    return multiply(desired,inverse);
}
Matrix handTransform(Vec3 wrist,Vec3 palm,Vec3 up,const Matrix& target) {
    const auto basis=[](Vec3 forward,Vec3 upHint) {
        auto result=poseMatrix({});const float size=length(forward);
        if(!std::isfinite(size)||size<0.00001f) return result;
        forward=mul(forward,1/size);auto right=cross(forward,upHint);
        if(length(right)<0.00001f) right=cross(forward,std::fabs(forward[1])<0.9f?Vec3{0,1,0}:Vec3{1,0,0});
        right=mul(right,1/length(right));const auto upright=cross(right,forward);
        for(int r=0;r<3;++r) {result[r*4]=right[r];result[r*4+1]=upright[r];result[r*4+2]=-forward[r];}
        return result;
    };
    const auto old=basis(sub(palm,wrist),up);
    const auto desired=basis({-target[2],-target[6],-target[10]},{target[1],target[5],target[9]});
    auto inverse=poseMatrix({});for(int r=0;r<3;++r)for(int c=0;c<3;++c)inverse[r*4+c]=old[c*4+r];
    auto result=multiply(desired,inverse);
    for(int r=0;r<3;++r) result[r*4+3]=target[r*4+3]-(result[r*4]*wrist[0]+result[r*4+1]*wrist[1]+result[r*4+2]*wrist[2]);
    return result;
}
std::array<float,5> toolGripCurls(const HandInput& input) {
    const float trigger=std::isfinite(input.trigger)?std::clamp(input.trigger,0.f,1.f):0;
    // A cylindrical grasp, not an empty-hand pinch. The middle finger's 0.8
    // matches the measured contact arc. Trigger motion stays on the index.
    return {0.72f,0.78f+0.10f*trigger,0.8f,0.82f,0.84f};
}
std::array<float,2> counterPullStick(Vec3 start,Vec3 current,float headYaw) {
    if(!finite(start)||!finite(current)||!std::isfinite(headYaw))return {};
    const float dx=current[0]-start[0],dz=current[2]-start[2],reach=std::hypot(dx,dz);
    if(reach<=0.04f)return {};
    const float gain=std::clamp((reach-0.04f)/0.12f,0.f,1.f)/reach,c=std::cos(headYaw),s=std::sin(headYaw);
    return {(c*dx-s*dz)*gain,-(s*dx+c*dz)*gain};
}
float opposedPullDistance(Vec3 delta,float heading,float escapeYaw,float seconds) {
    if(!finite(delta)||!std::isfinite(heading)||!std::isfinite(escapeYaw)||!std::isfinite(seconds)||seconds<=0||seconds>0.1f)return 0;
    const float c=std::cos(heading),s=std::sin(heading);
    const float x=c*delta[0]+s*delta[2],z=-s*delta[0]+c*delta[2];
    const float opposed=x*std::sin(escapeYaw)+z*std::cos(escapeYaw);
    const float travel=std::hypot(x,z);
    return opposed>0.06f*seconds&&opposed>travel*0.5f?opposed:0;
}
void CapturePulls::reset(){targets_={};primary_=0;}
void CapturePulls::observe(unsigned actor,Vec3 player,Vec3 ghost,double now) {
    if(!actor||!finite(player)||!finite(ghost)||!std::isfinite(now))return;
    const float dx=ghost[0]-player[0],dz=ghost[2]-player[2];if(std::hypot(dx,dz)<1)return;
    Target* target=nullptr;
    for(auto& t:targets_)if(t.actor==actor){target=&t;break;}
    if(!target)for(auto& t:targets_)if(!t.actor||now-t.seen>0.25){target=&t;break;}
    if(!target)return;
    const float bearing=std::atan2(-dx,-dz);
    if(target->actor!=actor||now-target->seen>0.25||now<target->seen) {*target={actor,ghost,bearing,bearing,0,now,0};}
    else {
        const float vx=ghost[0]-target->position[0],vz=ghost[2]-target->position[2];
        if(now-target->seen>0.0005&&std::hypot(vx,vz)>0.25f) {
            const float escape=std::atan2(-vx,-vz);
            if(std::cos(escape-target->escape)<0.5f){target->stroke=0;target->damageUntil=0;}
            target->escape=escape;
        }
        target->position=ghost;target->bearing=bearing;target->seen=now;
    }
    bool primaryFresh=false;for(const auto& t:targets_)if(t.actor==primary_&&now>=t.seen&&now-t.seen<=0.25)primaryFresh=true;
    if(!primaryFresh)primary_=actor;
}
CaptureFocus CapturePulls::focus(double now) const {
    for(const auto& t:targets_)if(t.actor==primary_&&now>=t.seen&&now-t.seen<=0.25)return {t.actor,t.bearing,t.escape};
    for(const auto& t:targets_)if(t.actor&&now>=t.seen&&now-t.seen<=0.25)return {t.actor,t.bearing,t.escape};
    return {};
}
void CapturePulls::step(Vec3 delta,float heading,float seconds,double now,bool active) {
    for(auto& t:targets_) {
        if(!active||!t.actor||now<t.seen||now-t.seen>0.25||!finite(delta)||!std::isfinite(seconds)||seconds<=0||seconds>0.1f){t.stroke=0;t.damageUntil=0;continue;}
        const float stroke=opposedPullDistance(delta,heading,t.escape,seconds);
        if(stroke>0)t.stroke+=stroke;
        else if(std::hypot(delta[0],delta[2])>0.0025f){t.stroke=0;t.damageUntil=0;}
        if(t.stroke>=0.015f){t.damageUntil=now+0.16;t.stroke=0;}
    }
}
bool CapturePulls::allows(unsigned actor,double now) const {
    for(const auto& t:targets_)if(t.actor==actor&&now>=t.seen&&now-t.seen<=0.25)return now<t.damageUntil;
    return false;
}
int captureHealthSource(int health,bool damageAllowed) {
    return !damageAllowed&&health>0&&health<32767?health+1:health;
}
float captureFollowStep(float currentYaw,float targetYaw,float seconds) {
    if(!std::isfinite(currentYaw)||!std::isfinite(targetYaw)||!std::isfinite(seconds)||seconds<=0)return 0;
    const float error=std::remainder(targetYaw-currentYaw,6.283185307f);
    const float outside=std::max(0.f,std::fabs(error)-0.08726646f);
    const float dt=std::min(seconds,0.05f);
    return std::copysign(std::min(outside*6.f*dt,2.09439510f*dt),error);
}
std::array<float,5> gloveCurls(const HandInput& input) {
    const auto safe=[](float v){return std::isfinite(v)?std::clamp(v,0.0f,1.0f):0.0f;};
    std::array<float,5> curls{safe(input.grip)*0.65f,safe(input.trigger),safe(input.grip),safe(input.grip),safe(input.grip)};
    for(unsigned i=0;i<5;++i) if(input.fingerValid&(1u<<i)) curls[i]=safe(input.fingerCurl[i]);
    return curls;
}
float jointCurl(std::span<const Vec3> positions,bool thumb) {
    if(positions.size()<4) return -1;
    float angle=0;
    for(unsigned i=1;i+1<positions.size();++i) {
        const auto a=sub(positions[i],positions[i-1]),b=sub(positions[i+1],positions[i]);
        const float na=length(a),nb=length(b);
        if(!finite(a)||!finite(b)||!std::isfinite(na)||!std::isfinite(nb)||na<0.00001f||nb<0.00001f) return -1;
        angle+=std::acos(std::clamp(dot(a,b)/(na*nb),-1.0f,1.0f));
    }
    return std::clamp(angle/(thumb?1.7f:2.6f),0.0f,1.0f);
}
void curlFinger(std::span<Matrix> chain,Vec3 backNormal,float curl,bool thumb) {
    if(chain.size()!=3||!std::isfinite(curl)||!finite(backNormal)) return;
    curl=std::clamp(curl,0.0f,1.0f);
    const auto pos=[](const Matrix& m){return Vec3{m[3],m[7],m[11]};};
    auto axis=cross(sub(pos(chain[1]),pos(chain[0])),mul(backNormal,-1));
    const float n=length(axis);if(!std::isfinite(n)||n<0.00001f) return;axis=mul(axis,1/n);
    for(unsigned i=0;i<3;++i) {
        const float angle=curl*(thumb?(i==0?0.65f:0.9f):(i==0?1.2f:i==1?1.4f:0.95f));
        const float sn=std::sin(angle/2),cs=std::cos(angle/2);
        auto turn=poseMatrix({{axis[0]*sn,axis[1]*sn,axis[2]*sn,cs},{}});const auto pivot=pos(chain[i]);
        for(int r=0;r<3;++r)turn[r*4+3]=pivot[r]-(turn[r*4]*pivot[0]+turn[r*4+1]*pivot[1]+turn[r*4+2]*pivot[2]);
        for(unsigned j=i;j<3;++j)chain[j]=multiply(turn,chain[j]);
    }
}
unsigned capCuffRims(std::vector<std::uint8_t>& data,unsigned stride,const std::array<bool,10>& cuffs,std::span<const Vec3> positions,const std::array<Vec3,10>& axes,std::span<const std::uint8_t> source) {
    if(stride<5||stride>32||positions.empty())return 0;
    const auto input=source.empty()?std::span<const std::uint8_t>(data):source;
    struct Vertex {Vec3 point;std::vector<std::uint8_t> bytes;};
    std::array<std::vector<Vertex>,10> rings;
    size_t at=0;unsigned format=0;
    while(at<input.size()) {
        const auto op=input[at++];if(!op)continue;
        if((op&0xF8)!=0x90||at+2>input.size())return 0;
        format=op&7;const unsigned n=(unsigned(input[at])<<8)|input[at+1];at+=2;
        if(at+size_t(n)*stride>input.size())return 0;
        for(unsigned j=0;j<n;++j) {
            const auto* v=input.data()+at+j*stride;const unsigned slot=v[0]/3,index=(unsigned(v[3])<<8)|v[4];
            if(v[0]%3||slot>=10||!cuffs[slot]||index>=positions.size()||!finite(positions[index]))continue;
            auto& ring=rings[slot];if(std::none_of(ring.begin(),ring.end(),[&](const Vertex& old){return length(sub(old.point,positions[index]))<0.001f;}))ring.push_back({positions[index],{v,v+stride}});
        }
        at+=size_t(n)*stride;
    }
    std::vector<std::uint8_t> triangles;unsigned count=0;
    for(unsigned slot=0;slot<10;++slot)for(float side:{1.f,-1.f}) {
        auto ring=rings[slot];if(ring.size()<3||!finite(axes[slot])||length(axes[slot])<0.001f)continue;
        auto axis=mul(axes[slot],side/length(axes[slot]));
        // Detached native cuffs are a single almost-planar rim. Their bind
        // axis is tilted differently on each side; splitting that rim along
        // the axis leaves a rectangular gap between two partial fans.
        Vec3 plane{};float area=0;
        for(unsigned i=1;i<ring.size();++i)for(unsigned j=i+1;j<ring.size();++j){
            const auto n=cross(sub(ring[i].point,ring[0].point),sub(ring[j].point,ring[0].point));
            if(dot(n,n)>area){plane=n;area=dot(n,n);}
        }
        bool planar=area>0.000001f;
        if(planar){plane=mul(plane,1/std::sqrt(area));for(const auto& v:ring)
            if(std::fabs(dot(sub(v.point,ring[0].point),plane))>0.5f)planar=false;}
        if(planar){if(side<0)continue;axis=plane;}
        float back=1e30f;
        for(const auto& v:ring)back=std::min(back,dot(v.point,axis));
        const auto full=ring;
        if(!planar)std::erase_if(ring,[&](const Vertex& v){return dot(v.point,axis)>back+0.5f;});
        if(ring.size()<3) {
            ring=full;std::sort(ring.begin(),ring.end(),[&](const Vertex& a,const Vertex& b){return dot(a.point,axis)<dot(b.point,axis);});
            ring.resize(std::max<size_t>(3,ring.size()/2));
        }
        if(ring.size()>64)continue;
        Vec3 centre{};for(const auto& v:ring)centre=add(centre,v.point);centre=mul(centre,1.f/ring.size());
        auto right=cross(axis,std::fabs(axis[1])<0.9f?Vec3{0,1,0}:Vec3{1,0,0});right=mul(right,1/length(right));const auto up=cross(axis,right);
        std::sort(ring.begin(),ring.end(),[&](const Vertex& a,const Vertex& b){const auto av=sub(a.point,centre),bv=sub(b.point,centre);return std::atan2(dot(av,up),dot(av,right))<std::atan2(dot(bv,up),dot(bv,right));});
        for(unsigned j=2;j<ring.size();++j)for(unsigned v:{0u,j-1,j,0u,j,j-1})triangles.insert(triangles.end(),ring[v].bytes.begin(),ring[v].bytes.end());
        ++count;
    }
    // Close cut boundaries which are not perpendicular to the cuff axis.
    // The right glove has an oblique rectangular cut; an axial end-ring alone
    // leaves it open. Weld display-list normal/UV seams by position index.
    struct Edge {unsigned count=0;};
    std::map<std::pair<unsigned,unsigned>,Edge> edges;
    std::map<unsigned,std::vector<std::uint8_t>> vertices;
    std::map<std::array<long,3>,unsigned> welded;
    const auto weld=[&](unsigned id) {
        if(id>=positions.size()||!finite(positions[id]))return id;
        const auto& p=positions[id];const std::array<long,3> key{std::lround(p[0]*1000),std::lround(p[1]*1000),std::lround(p[2]*1000)};
        return welded.try_emplace(key,id).first->second;
    };
    at=0;
    while(at<data.size()) {
        const auto op=data[at++];if(!op)continue;
        if((op&0xF8)!=0x90||at+2>data.size())break;
        const unsigned total=(unsigned(data[at])<<8)|data[at+1];at+=2;
        if(total%3||at+size_t(total)*stride>data.size())break;
        for(unsigned j=0;j<total;j+=3) {
            unsigned ids[3];
            for(unsigned k=0;k<3;++k){const auto* v=data.data()+at+(j+k)*stride;ids[k]=weld((unsigned(v[3])<<8)|v[4]);vertices.try_emplace(ids[k],v,v+stride);}
            for(unsigned k=0;k<3;++k)++edges[std::minmax(ids[k],ids[(k+1)%3])].count;
        }
        at+=size_t(total)*stride;
    }
    std::map<unsigned,std::vector<unsigned>> neighbors;
    for(const auto& [edge,info]:edges)if(info.count==1){neighbors[edge.first].push_back(edge.second);neighbors[edge.second].push_back(edge.first);}
    std::set<unsigned> visited;
    for(const auto& [start,links]:neighbors) {
        if(visited.count(start)||links.size()!=2)continue;
        std::vector<unsigned> loop;unsigned current=start,previous=~0u;bool closed=false;
        for(unsigned step=0;step<64;++step){
            if(current>=positions.size()||neighbors[current].size()!=2||visited.count(current))break;
            visited.insert(current);loop.push_back(current);
            const auto& next=neighbors[current];const unsigned chosen=next[0]==previous?next[1]:next[0];previous=current;current=chosen;
            if(current==start){closed=true;break;}
        }
        if(!closed||loop.size()<3)continue;
        unsigned cuff=10;
        for(unsigned id:loop){const unsigned slot=vertices[id][0]/3;if(slot<10&&cuffs[slot]&&length(axes[slot])>.001f){cuff=slot;break;}}
        const bool cuffVertices=cuff!=10;
        // The cut may end on the adjacent wrist envelope rather than on the
        // cuff envelope itself. Accept only loops close to an actual cuff.
        if(cuff==10)for(unsigned slot=0;slot<10&&cuff==10;++slot)if(cuffs[slot]&&length(axes[slot])>.001f)
            for(unsigned id:loop)if(std::any_of(rings[slot].begin(),rings[slot].end(),[&](const Vertex& v){return length(sub(v.point,positions[id]))<8.f;})){cuff=slot;break;}
        if(cuff==10)continue;
        const auto axis=mul(axes[cuff],1/length(axes[cuff]));float lo=1e30f,hi=-1e30f;
        for(unsigned id:loop){const float d=dot(positions[id],axis);lo=std::min(lo,d);hi=std::max(hi,d);}
        if(cuffVertices&&hi-lo<=0.5f)continue; // already covered by the axial caps above
        for(unsigned j=2;j<loop.size();++j)for(unsigned k:{0u,j-1,j,0u,j,j-1}) {
            const auto& bytes=vertices[loop[k]];triangles.insert(triangles.end(),bytes.begin(),bytes.end());
        }
        ++count;
    }
    const auto n=triangles.size()/stride;if(!n||n>65535)return 0;
    data.push_back(0x90|format);data.push_back(std::uint8_t(n>>8));data.push_back(std::uint8_t(n));data.insert(data.end(),triangles.begin(),triangles.end());while(data.size()%32)data.push_back(0);return count;
}
bool filterSleeveTriangles(std::span<const std::uint8_t> data,unsigned stride,const std::array<bool,10>& sleeves,std::vector<std::uint8_t>& out) {
    out.clear();if(stride<3||stride>32||data.size()>262144) return false;
    size_t at=0;
    while(at<data.size()) {
        const auto op=data[at++];if(op==0)continue;
        const unsigned kind=op&0xF8;
        if(kind!=0x80&&kind!=0x90&&kind!=0x98&&kind!=0xA0) {out.clear();return false;}
        if(at+2>data.size()) {out.clear();return false;}
        const unsigned n=(unsigned(data[at])<<8)|data[at+1];at+=2;
        if(at+size_t(n)*stride>data.size()||(kind==0x90&&n%3)||(kind==0x80&&n%4)) {out.clear();return false;}
        const auto start=at;at+=size_t(n)*stride;
        std::vector<std::uint8_t> triangles;
        bool valid=true;
        const auto tri=[&](unsigned a,unsigned b,unsigned c) {
            bool blocked=false;
            for(unsigned v:{a,b,c}) {const unsigned idx=data[start+v*stride];if(idx%3||idx/3>=10) {valid=false;return;}blocked|=sleeves[idx/3];}
            if(blocked)return;
            for(unsigned v:{a,b,c})triangles.insert(triangles.end(),data.begin()+start+v*stride,data.begin()+start+(v+1)*stride);
        };
        if(kind==0x90) for(unsigned i=0;i+2<n;i+=3)tri(i,i+1,i+2);
        if(kind==0x80) for(unsigned i=0;i+3<n;i+=4) {tri(i,i+1,i+2);tri(i,i+2,i+3);}
        if(kind==0x98) for(unsigned i=2;i<n;++i) {if(i%2)tri(i-1,i-2,i);else tri(i-2,i-1,i);}
        if(kind==0xA0) for(unsigned i=2;i<n;++i)tri(0,i-1,i);
        const auto count=triangles.size()/stride;
        if(!valid||count>65535) {out.clear();return false;}
        if(count) {out.push_back(0x90|(op&7));out.push_back(std::uint8_t(count>>8));out.push_back(std::uint8_t(count));out.insert(out.end(),triangles.begin(),triangles.end());}
    }
    while(out.size()%32)out.push_back(0);
    return true;
}
std::array<float,3> equipmentTriggers(const std::array<GrabState,2>& grabs,const std::array<HandInput,2>& hands) {
    const auto trigger=[&](unsigned h){return hands[h].valid&&std::isfinite(hands[h].trigger)?std::clamp(hands[h].trigger,0.0f,1.0f):0.0f;};
    std::array<float,3> out{};
    for(unsigned h=0;h<2;++h)if(grabs[h].target&&hands[h].valid) {
        if(grabs[h].kind==GrabKind::Flashlight)out[2]=trigger(h);
        if(grabs[h].kind==GrabKind::Poltergust){out[1]=trigger(h);if(!grabs[1-h].target)out[0]=trigger(1-h);}
    }
    return out;
}
void GrabController::reset() {state_={};down_=false;armed_=true;}
void GrabController::cancel() {reset();armed_=false;}
GrabState GrabController::update(const HandInput& input,std::span<const GrabTarget> targets) {
    state_.grabbed=state_.released=state_.use=false;
    const auto release=[&] {state_.released=state_.target!=0;state_.target=0;state_.kind=GrabKind::None;state_.twist=0;};
    bool valid=input.valid&&finite(input.pose.position)&&std::isfinite(input.grip)&&std::isfinite(input.trigger);
    for(float q:input.pose.orientation) valid=valid&&std::isfinite(q);
    float norm=0;for(float q:input.pose.orientation)norm+=q*q;valid=valid&&std::isfinite(norm)&&norm>0.00001f;
    if(!valid) {release();down_=false;armed_=false;return state_;}
    if(input.grip<0.35f) {release();down_=false;armed_=true;return state_;}
    if(!armed_) return state_; // tracking recovery cannot re-grab until grip is released
    if(!down_ && input.grip>=0.65f) {
        down_=true;const GrabTarget* best=nullptr;float nearest=1e9f;
        for(const auto& target:targets) {
            if(!target.id||!target.available||target.kind==GrabKind::None||!finite(target.position)||!std::isfinite(target.radius)||target.radius<=0) continue;
            const float distance=length(sub(input.pose.position,target.position));
            if(distance<=target.radius && distance<nearest) {best=&target;nearest=distance;}
        }
        if(best) {state_.target=best->id;state_.kind=best->kind;state_.grabbed=true;start_=input.pose;}
    }
    if(state_.target) {
        const auto it=std::find_if(targets.begin(),targets.end(),[&](const auto& t){return t.id==state_.target&&t.kind==state_.kind&&t.available;});
        if(it==targets.end()) {release();armed_=false;return state_;}
        if(state_.kind==GrabKind::Door && length(sub(input.pose.position,it->position))>it->radius+0.15f) {
            release();armed_=false;return state_;
        }
        state_.use=input.trigger>=0.65f;
        const auto relative=multiply(inversePose(start_),poseMatrix(input.pose));
        // Twist around the wrist's initial forward axis; atan2 wraps at pi.
        state_.twist=std::atan2(relative[4]-relative[1],relative[0]+relative[5]);
    }
    return state_;
}
std::array<GrabState,2> InteractionHands::update(const std::array<HandInput,2>& hands,std::span<const GrabTarget> targets) {
    // Existing owners update first. A new left-hand grab cannot steal a right-hand tool.
    const unsigned first=state_[1].target && !state_[0].target?1:0;
    for(unsigned pass=0;pass<2;++pass) {
        const unsigned h=(first+pass)%2,other=1-h;
        std::vector<GrabTarget> available(targets.begin(),targets.end());
        for(auto& target:available) {
            const bool singleTool=target.kind==GrabKind::Flashlight||target.kind==GrabKind::Poltergust;
            if(state_[other].target && (target.id==state_[other].target || (singleTool&&target.kind==state_[other].kind))) target.available=false;
        }
        state_[h]=controllers_[h].update(hands[h],available);
    }
    return state_;
}
void InteractionHands::cancel() {for(auto& controller:controllers_)controller.cancel();state_={};}
bool settingsPanelPointer(const Matrix& aim,float aspect,std::array<float,2>& uv) {
    if(!std::isfinite(aspect)||aspect<=0||std::fabs(aim[10])<0.00001f)return false;
    const float t=(-1.6f-aim[11])/-aim[10];if(!std::isfinite(t)||t<=0)return false;
    uv={(aim[3]-t*aim[2])/1.4f+0.5f,0.5f-(aim[7]-t*aim[6])*aspect/1.4f};
    return std::isfinite(uv[0])&&std::isfinite(uv[1])&&uv[0]>=0&&uv[0]<=1&&uv[1]>=0&&uv[1]<=1;
}
bool bypassVacuumStartBlend(bool firstPerson,unsigned oldMode,unsigned newMode,unsigned controllerState,unsigned oldMotion,unsigned newMotion) {
    return firstPerson&&oldMode==5&&newMode==1&&controllerState==1&&oldMotion==0&&newMotion==1;
}
bool mergeNativeStick(std::int8_t& x,std::int8_t& y,float sx,float sy) {
    if(!std::isfinite(sx)||!std::isfinite(sy)||std::hypot(sx,sy)<=0.2f)return false;
    const float nx=std::clamp(sx,-1.f,1.f)*100,ny=std::clamp(sy,-1.f,1.f)*100;
    if(std::hypot(float(x),float(y))>=std::hypot(nx,ny))return false;
    x=std::int8_t(std::lround(nx));y=std::int8_t(std::lround(ny));return true;
}
void routeControllerStick(std::int8_t& x,std::int8_t& y,float stickX,float stickY,bool headRelative,const std::array<float,4>& mapping) {
    float ix=x,iy=y;
    if(std::isfinite(stickX)&&std::isfinite(stickY)&&std::hypot(stickX,stickY)>0.2f) {
        ix=std::clamp(stickX,-1.0f,1.0f)*100;iy=std::clamp(stickY,-1.0f,1.0f)*100;
    }
    const float ox=headRelative?mapping[0]*ix+mapping[1]*iy:ix;
    const float oy=headRelative?mapping[2]*ix+mapping[3]*iy:iy;
    x=std::int8_t(std::clamp(std::lround(ox),-100l,100l));y=std::int8_t(std::clamp(std::lround(oy),-100l,100l));
}
void ButtonLatch::sample(std::uint32_t buttons,bool focused) {
    if(!focused) {held_=pending_=0;blocked_=~std::uint32_t(0);return;}
    blocked_&=buttons;
    const auto permitted=buttons&~blocked_;
    pending_|=permitted&~held_;held_=permitted;
}
std::uint32_t ButtonLatch::consume() {const auto out=held_|pending_;pending_=0;return out;}
bool ordinaryDoorEligible(unsigned type,unsigned flags,unsigned roomA,unsigned roomB,unsigned currentRoom) {
    // Verified retail single wooden door only. Unknown flags fail closed; this
    // excludes locks, story seals, fake doors and doors already animating.
    constexpr unsigned idle=8|0x100|3|0x8000;
    return type==1&&(flags&8)&&!(flags&~idle)&&roomA!=roomB&&roomA<255&&roomB<255&&(currentRoom==roomA||currentRoom==roomB);
}
bool ordinaryDoorClosable(unsigned type,unsigned flags,unsigned roomA,unsigned roomB,unsigned currentRoom) {
    constexpr unsigned idle=0x10|0x100|0x200|0x800|3|0x8000;
    return type==1&&(flags&0x10)&&!(flags&~idle)&&roomA!=roomB&&roomA<255&&roomB<255&&(currentRoom==roomA||currentRoom==roomB);
}
unsigned ordinaryDoorOpenFlags(unsigned flags,bool reverse) {
    return (flags&~(8u|0x800u))|0x240u|(reverse?0x800u:0);
}
unsigned ordinaryDoorCloseFlags(unsigned flags){return (flags&~(0x10u|0x40u))|0x280u;}
void WorldGesture::cancel(){id_=0;participants_={};distance_=0;reversals_=0;lastDirection_=0;spent_=false;armed_=false;}
WorldIntent WorldGesture::update(const WorldTarget& target,const std::array<HandInput,2>& hands) {
    WorldIntent out;
    const bool released=hands[0].valid&&hands[1].valid&&(hands[0].occupied||hands[0].grip<0.35f)&&(hands[1].occupied||hands[1].grip<0.35f);
    if(released){const auto previous=id_;cancel();armed_=true;out.id=previous;out.cancelled=previous!=0;return out;}
    if(!target.id||!target.available||!finite(target.contact)||!finite(target.axis)||!std::isfinite(target.reach)||target.reach<=0||target.reach>0.5f||(id_&&(id_!=target.id||action_!=target.action))) {out.id=id_;out.cancelled=id_!=0;cancel();return out;}
    if(!id_) {
        if(!armed_)return out;unsigned count=0;
        for(unsigned h=0;h<2;++h){participants_[h]=hands[h].valid&&!hands[h].occupied&&finite(hands[h].pose.position)&&hands[h].grip>=0.65f&&length(sub(hands[h].pose.position,target.contact))<=target.reach;count+=participants_[h];}
        if(count<(target.twoHands?2u:1u))return out;
        id_=target.id;action_=target.action;for(unsigned h=0;h<2;++h)start_[h]=hands[h].pose;last_=target.contact;out.began=true;
    }
    out.id=id_;Vec3 delta{},point{};unsigned count=0;
    for(unsigned h=0;h<2;++h)if(participants_[h]) {
        if(!hands[h].valid||hands[h].occupied||hands[h].grip<0.35f||!finite(hands[h].pose.position)||length(sub(hands[h].pose.position,start_[h].position))>0.65f||(action_==WorldAction::Door&&length(sub(hands[h].pose.position,target.contact))>target.reach+0.15f)){out.cancelled=true;cancel();return out;}
        for(unsigned k=0;k<3;++k){delta[k]+=hands[h].pose.position[k]-start_[h].position[k];point[k]+=hands[h].pose.position[k];}++count;
    }
    if(!count)return out;for(unsigned k=0;k<3;++k){delta[k]/=count;point[k]/=count;}
    const float axisLength=length(target.axis);if(axisLength<0.99f||axisLength>1.01f){out.cancelled=true;cancel();return out;}
    float motion=0;for(unsigned k=0;k<3;++k)motion+=delta[k]*target.axis[k];
    if(action_==WorldAction::Door) {
        // Signed quaternion twist about the knob axis; translation cannot open it.
        float twist=0;for(unsigned h=0;h<2;++h)if(participants_[h]){const auto& a=start_[h].orientation;const auto& b=hands[h].pose.orientation;
            const Vec3 v{b[0]*a[3]-b[3]*a[0]-b[1]*a[2]+b[2]*a[1],b[1]*a[3]-b[3]*a[1]-b[2]*a[0]+b[0]*a[2],b[2]*a[3]-b[3]*a[2]-b[0]*a[1]+b[1]*a[0]};
            float dot=0;for(unsigned k=0;k<3;++k)dot+=v[k]*target.axis[k];const float w=b[3]*a[3]+b[0]*a[0]+b[1]*a[1]+b[2]*a[2];
            if(!std::isfinite(dot)||!std::isfinite(w)){out.cancelled=true;cancel();return out;}const float angle=2*std::atan2(dot,w);twist+=std::fabs(std::remainder(angle,6.283185307f));}
        out.amount=std::clamp(twist/count/0.65f,0.f,1.f);
    } else if(action_==WorldAction::Shake){
        float step=0;for(unsigned k=0;k<3;++k)step+=(point[k]-last_[k])*target.axis[k];
        if(std::fabs(step)>0.008f){if(lastDirection_*step<0)++reversals_;lastDirection_=step;distance_+=std::fabs(step);}
        last_=point;out.amount=std::min(std::clamp(distance_/0.20f,0.f,1.f),std::clamp(float(reversals_)/2,0.f,1.f));
    } else out.amount=std::clamp(motion/(action_==WorldAction::Chest?0.18f:0.06f),0.f,1.f);
    if(out.amount>=1&&!spent_){out.completed=true;spent_=true;}return out;
}
std::array<GrabTarget,4> equipmentTargets() {
    return {{{1,GrabKind::Flashlight,{0.24f,-0.65f,-0.08f},0.30f,true},
             {2,GrabKind::Key,{0,-0.65f,-0.18f},0.12f,true},
             {3,GrabKind::Poltergust,{0.24f,-0.16f,0.16f},0.34f,true},
             {4,GrabKind::GameBoy,{-0.24f,-0.65f,-0.24f},0.30f,true}}};
}
bool idleKeyGhostLuigi(std::string& script) {
    const std::string stop="<ACTOR>\"demo_luigi\"<STOP>";
    const std::string idle="<ACTOR>\"demo_luigi\"<ACT>\"WAIT0\"";
    if(script.find("ed07cam.cmn")==std::string::npos || script.find("demo_key")==std::string::npos)return false;
    const auto at=script.find(stop);if(at==std::string::npos)return false;
    if(script.find(idle,at)==std::string::npos)script.insert(at+stop.size(),"\n"+idle);
    return true;
}
bool handoffKeyGhostKeys(std::string& script) {
    if(script.find("ed07cam.cmn")==std::string::npos)return false;
    const std::string pickup="<ACTOR>\"demo_key2\"<ALPHA>(255)";
    const std::string scene="<ACTOR>\"demo_key\"<ALPHA>(255)";
    const std::string hidden="<ACTOR>\"demo_key\"<ALPHA>(0)";
    const auto handoff=script.find(pickup);if(handoff==std::string::npos)return false;
    const std::string early="<WAIT>(144)\n"+hidden+"\n<WAIT>(1)";
    if(script.find(early)!=std::string::npos)return true;
    const auto wait=script.rfind("<WAIT>(145)",handoff);
    const auto at=script.find(scene,handoff);
    if(wait==std::string::npos || at==std::string::npos)return false;
    // Keep total native timing and pickup reveal unchanged; hide one tick sooner.
    script.erase(at,scene.size());
    script.replace(wait,std::string("<WAIT>(145)").size(),early);return true;
}
bool hideEarlyRescueProfessor(std::string& script) {
    if(script.find("cam_tdm1.cmn")==std::string::npos)return false;
    const std::string shown="<ACTOR>\"demo_hakase\"<ALPHA>(255)";
    const std::string hidden="<ACTOR>\"demo_hakase\"<ALPHA>(0)";
    const std::string cue="<ACTOR>\"demo_hakase\"<SETCLEANER>\"demo_vhead\"\"demo_vbody\"";
    const auto start=script.find(shown),use=script.find(cue);
    if(use==std::string::npos)return false;
    if(script.find(hidden)!=std::string::npos && script.find(shown)>script.find(hidden) && script.find(shown)<use)return true;
    if(start==std::string::npos || start>use)return false;
    script.insert(use,shown+"\n<ACTOR>\"demo_vhead\"<ALPHA>(255)\n<ACTOR>\"demo_vbody\"<ALPHA>(255)\n");
    script.replace(start,shown.size(),hidden+"\n<ACTOR>\"demo_vhead\"<ALPHA>(0)\n<ACTOR>\"demo_vbody\"<ALPHA>(0)");
    return true;
}

bool visibleSwitchSceneLuigi(std::string& script) {
    if(script.find("rom14cam.cmn")==std::string::npos || script.find("<WAIT>(430)")==std::string::npos)return false;
    const std::string hidden="<ACTOR>\"demo_luigi\"<DISPOFF>";
    const auto at=script.find(hidden);
    if(at==std::string::npos)return script.find("<ACTOR>\"demo_luigi\"<DISPON>")!=std::string::npos;
    script.replace(at,hidden.size(),"<ACTOR>\"demo_luigi\"<DISPON>");return true;
}
bool liveParlourEncounter(std::string& script) {
    // Require its unique camera and final ghost-enable marker. Operate on the
    // user's loaded script, keeping story messages, waits, lights and flags.
    if(script.find("<GCAMERA>\"ima_demo.cmn\"")==std::string::npos||script.find("<FLAGON>(91)")==std::string::npos)return false;
    for(const std::string_view tag:{"<MENUOFF>","<LUIGISTOP2>","<HLIGHTON>",
        "<ACTOR>\"demo_luigi\"<STOP>","<ACTOR>\"demo_luigi\"<ACT>\"WAIT0\"", "<GCAMERA>\"ima_demo.cmn\""}) {
        size_t at=0;while((at=script.find(tag,at))!=std::string::npos){script.replace(at,tag.size(),tag.size(),' ');at+=tag.size();}
    }
    return true;
}
std::array<GrabTarget,4> beltTargets(const Matrix& head,float yaw,bool flashlightLeft,bool poltergustLeft,bool gameBoyLeft) {
    auto targets=equipmentTargets();
    targets[0].position[0]=flashlightLeft?-0.24f:0.24f;targets[2].position[0]=poltergustLeft?-0.24f:0.24f;targets[3].position[0]=gameBoyLeft?-0.24f:0.24f;const float c=std::cos(yaw),s=std::sin(yaw);
    for(auto& t:targets) {const auto p=t.position;t.position={head[3]+c*p[0]+s*p[2],head[7]+p[1],head[11]-s*p[0]+c*p[2]};}
    return targets;
}
std::array<GrabTarget,4> toolbarTargets(const Matrix& head,float yaw,bool poltergustLeft,float drop,float reach) {
    auto targets=equipmentTargets();const float c=std::cos(yaw),s=std::sin(yaw);
    // Three separated belt contacts; the vacuum uses its wider shoulder zone.
    constexpr std::array<float,4> slots{0.22f,-0.22f,0,0};
    for(unsigned i=0;i<targets.size();++i){const bool shoulder=i==2;
        const float x=shoulder?(poltergustLeft?-0.24f:0.24f):slots[i];
        const float y=shoulder?-0.16f:-std::clamp(std::isfinite(drop)?drop:0.50f,0.30f,0.80f),z=shoulder?0.16f:-std::clamp(std::isfinite(reach)?reach:0.23f,0.10f,0.45f);
        targets[i].position={head[3]+c*x+s*z,head[7]+y,head[11]-s*x+c*z};targets[i].radius=shoulder?0.34f:0.105f;
    }
    return targets;
}

std::uint32_t hoverTarget(const HandInput& hand,std::span<const GrabTarget> targets) {
    if(!hand.valid||!finite(hand.pose.position))return 0;
    float nearest=1e9f;std::uint32_t id=0;
    for(const auto& t:targets) {
        if(!t.available||!t.id||!finite(t.position)||!std::isfinite(t.radius)||t.radius<=0)continue;
        const float d=length(sub(hand.pose.position,t.position));
        if(d<=t.radius&&d<nearest){nearest=d;id=t.id;}
    }
    return id;
}
}

#include "vr/openxr_session.h"
#ifdef _WIN32
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdio>
#include "recomp/vr_avatar.h"
#include <stdexcept>

namespace lmvr {
namespace {
void check(XrResult result, const char* operation) {
    if (XR_FAILED(result)) throw std::runtime_error(std::string(operation)+" failed: "+std::to_string(result));
}
}
struct OpenXrSession::Impl {
    HMODULE loader = nullptr;
    PFN_xrGetInstanceProcAddr getProc = nullptr;
#define XR_FUNCTIONS(F) \
    F(DestroyInstance) F(GetSystem) F(GetSystemProperties) F(GetD3D12GraphicsRequirementsKHR) \
    F(EnumerateViewConfigurationViews) F(CreateSession) F(DestroySession) \
    F(CreateReferenceSpace) F(DestroySpace) F(EnumerateSwapchainFormats) \
    F(CreateSwapchain) F(DestroySwapchain) F(EnumerateSwapchainImages) \
    F(PollEvent) F(BeginSession) F(EndSession) F(WaitFrame) F(BeginFrame) \
    F(LocateViews) F(AcquireSwapchainImage) F(WaitSwapchainImage) \
    F(ReleaseSwapchainImage) F(EndFrame) \
    F(StringToPath) F(CreateActionSet) F(DestroyActionSet) F(CreateAction) \
    F(SuggestInteractionProfileBindings) F(AttachSessionActionSets) F(CreateActionSpace) \
    F(SyncActions) F(GetActionStatePose) F(GetActionStateFloat) F(GetActionStateVector2f) F(LocateSpace) F(GetActionStateBoolean) F(ApplyHapticFeedback) F(GetCurrentInteractionProfile) F(PathToString)
#define DECLARE(name) PFN_xr##name name = nullptr;
    XR_FUNCTIONS(DECLARE)
#undef DECLARE
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    XrActionSet actionSet=XR_NULL_HANDLE;
    XrAction gripAction=XR_NULL_HANDLE,aimAction=XR_NULL_HANDLE,squeezeAction=XR_NULL_HANDLE,triggerAction=XR_NULL_HANDLE,stickAction=XR_NULL_HANDLE;
    std::array<XrAction,8> buttons{};
    XrAction haptic=XR_NULL_HANDLE,stickClickAction=XR_NULL_HANDLE;
    std::array<XrPath,2> hands{};
    std::array<XrSpace,2> gripSpaces{},aimSpaces{};
    std::array<ControllerState,2> controllers{};
    bool focused=false,frameProfile=false,handTracking=false,handDataSource=false,handMotionRange=false;
    PFN_xrCreateHandTrackerEXT CreateHandTrackerEXT=nullptr;
    PFN_xrDestroyHandTrackerEXT DestroyHandTrackerEXT=nullptr;
    PFN_xrLocateHandJointsEXT LocateHandJointsEXT=nullptr;
    std::array<XrHandTrackerEXT,2> handTrackers{};
    std::array<unsigned,2> reportedFingers{256,256};
    void createFingerTracking() {
        if(!handTracking)return;
        XrSystemHandTrackingPropertiesEXT tracking{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
        XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};properties.next=&tracking;
        if(XR_FAILED(GetSystemProperties(instance,system,&properties))||!tracking.supportsHandTracking) {std::fprintf(stderr,"[vr fingers] runtime has no hand joint support; analog fallback\n");return;}
        PFN_xrVoidFunction create=nullptr,destroy=nullptr,locate=nullptr;
        getProc(instance,"xrCreateHandTrackerEXT",&create);getProc(instance,"xrDestroyHandTrackerEXT",&destroy);getProc(instance,"xrLocateHandJointsEXT",&locate);
        if(!create||!destroy||!locate)return;
        CreateHandTrackerEXT=reinterpret_cast<PFN_xrCreateHandTrackerEXT>(create);
        DestroyHandTrackerEXT=reinterpret_cast<PFN_xrDestroyHandTrackerEXT>(destroy);
        LocateHandJointsEXT=reinterpret_cast<PFN_xrLocateHandJointsEXT>(locate);
        for(unsigned h=0;h<2;++h) {
            XrHandTrackingDataSourceEXT source=XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT;
            XrHandTrackingDataSourceInfoEXT data{XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT};data.requestedDataSourceCount=1;data.requestedDataSources=&source;
            XrHandTrackerCreateInfoEXT info{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};info.hand=h?XR_HAND_RIGHT_EXT:XR_HAND_LEFT_EXT;info.handJointSet=XR_HAND_JOINT_SET_DEFAULT_EXT;
            if(handDataSource)info.next=&data;
            const auto result=CreateHandTrackerEXT(session,&info,&handTrackers[h]);
            if(XR_FAILED(result)) {handTrackers[h]=XR_NULL_HANDLE;std::fprintf(stderr,"[vr fingers] %s tracker unavailable (%d); analog fallback\n",h?"right":"left",int(result));}
            else std::fprintf(stderr,"[vr fingers] %s joint tracker created%s\n",h?"right":"left",handDataSource?" (controller source)":"");
        }
    }
    void sampleFingers(unsigned h) {
        if(!handTrackers[h]||!LocateHandJointsEXT)return;
        std::array<XrHandJointLocationEXT,XR_HAND_JOINT_COUNT_EXT> joints{};
        XrHandJointLocationsEXT locations{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};locations.jointCount=uint32_t(joints.size());locations.jointLocations=joints.data();
        XrHandJointsMotionRangeInfoEXT range{XR_TYPE_HAND_JOINTS_MOTION_RANGE_INFO_EXT};range.handJointsMotionRange=XR_HAND_JOINTS_MOTION_RANGE_CONFORMING_TO_CONTROLLER_EXT;
        XrHandJointsLocateInfoEXT info{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};info.baseSpace=space;info.time=frame.predictedDisplayTime;if(handMotionRange)info.next=&range;
        if(XR_SUCCEEDED(LocateHandJointsEXT(handTrackers[h],&info,&locations))&&locations.isActive) {
            for(unsigned finger=0;finger<5;++finger) {
                const unsigned first=finger?6+(finger-1)*5:2;
                std::array<recomp::vr::Vec3,5> points{};const unsigned count=finger?5:4;bool valid=true;
                for(unsigned j=0;j<count;++j) {
                    const auto& joint=joints[first+j];valid&=(joint.locationFlags&XR_SPACE_LOCATION_POSITION_VALID_BIT)!=0;
                    points[j]={joint.pose.position.x,joint.pose.position.y,joint.pose.position.z};
                }
                const float curl=valid?recomp::vr::jointCurl(std::span(points.data(),count),finger==0):-1;
                if(curl>=0) {controllers[h].fingerValid|=1u<<finger;controllers[h].fingerCurl[finger]=curl;}
            }
        }
        if(reportedFingers[h]!=controllers[h].fingerValid) {reportedFingers[h]=controllers[h].fingerValid;std::fprintf(stderr,"[vr fingers] %s valid finger mask %02X\n",h?"right":"left",unsigned(controllers[h].fingerValid));}
    }
    std::array<XrPath,2> reportedProfiles{XR_NULL_PATH,XR_NULL_PATH};
    std::array<std::uint64_t,2> reportedTracking{~std::uint64_t(0),~std::uint64_t(0)};
    XrPath path(const char* text) {XrPath p{};check(StringToPath(instance,text,&p),"input path");return p;}
    void createInput() {
        hands={path("/user/hand/left"),path("/user/hand/right")};
        XrActionSetCreateInfo set{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strcpy(set.actionSetName,"luigi_vr");std::strcpy(set.localizedActionSetName,"Luigi VR");
        check(CreateActionSet(instance,&set,&actionSet),"create VR action set");
        const auto action=[&](const char* name,XrActionType type,XrAction& out) {
            XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};info.actionType=type;
            std::strcpy(info.actionName,name);std::strcpy(info.localizedActionName,name);
            info.countSubactionPaths=2;info.subactionPaths=hands.data();
            check(CreateAction(actionSet,&info,&out),"create VR action");
        };
        action("hand_pose",XR_ACTION_TYPE_POSE_INPUT,gripAction);
        action("tool_aim",XR_ACTION_TYPE_POSE_INPUT,aimAction);
        action("grab",XR_ACTION_TYPE_FLOAT_INPUT,squeezeAction);
        action("use_tool",XR_ACTION_TYPE_FLOAT_INPUT,triggerAction);
        action("move_turn",XR_ACTION_TYPE_VECTOR2F_INPUT,stickAction);
        const char* names[]{"confirm","back","pause","map","search","status","hud_previous","hud_next"};
        for(unsigned i=0;i<buttons.size();++i) action(names[i],XR_ACTION_TYPE_BOOLEAN_INPUT,buttons[i]);
        action("feedback",XR_ACTION_TYPE_VIBRATION_OUTPUT,haptic);
        action("stick_click",XR_ACTION_TYPE_BOOLEAN_INPUT,stickClickAction);
        std::vector<const char*> profiles{"/interaction_profiles/oculus/touch_controller","/interaction_profiles/valve/index_controller","/interaction_profiles/microsoft/motion_controller","/interaction_profiles/khr/simple_controller"};
        if(frameProfile) profiles.push_back("/interaction_profiles/valve/frame_controller_valve");
        for(const char* profile:profiles) {
            std::vector<XrActionSuggestedBinding> bindings;
            for(const char* hand:{"/user/hand/left","/user/hand/right"}) {
                const auto add=[&](XrAction a,const char* suffix) {bindings.push_back({a,path((std::string(hand)+suffix).c_str())});};
                add(gripAction,"/input/grip/pose");add(aimAction,"/input/aim/pose");
                add(haptic,"/output/haptic");
                if(std::strstr(profile,"simple_controller")) {
                    add(buttons[0],"/input/select/click");add(buttons[2],"/input/menu/click");continue;
                }
                const bool left=std::strstr(hand,"/left")!=nullptr;
                const bool frame=std::strstr(profile,"frame_controller_valve")!=nullptr;
                if(frame) {
                    if(left) {add(buttons[2],"/input/view/click");add(buttons[3],"/input/dpad_down/click");add(buttons[4],"/input/dpad_up/click");add(buttons[6],"/input/dpad_left/click");add(buttons[7],"/input/dpad_right/click");}
                    else {add(buttons[0],"/input/a/click");add(buttons[1],"/input/b/click");add(buttons[5],"/input/bumper/click");}
                } else if(std::strstr(profile,"microsoft")) {
                    add(buttons[left?1:0],"/input/trackpad/click");
                    add(buttons[left?2:3],"/input/menu/click");
                    if(!left) add(buttons[4],"/input/thumbstick/click");
                } else {
                    const bool touch=std::strstr(profile,"oculus")!=nullptr;
                    add(buttons[left?3:0],left&&touch?"/input/x/click":"/input/a/click");
                    add(buttons[left?4:1],left&&touch?"/input/y/click":"/input/b/click");
                    if(left) add(buttons[2],"/input/thumbstick/click");
                }
                add(squeezeAction,(!frame&&std::strstr(profile,"valve"))?"/input/squeeze/force":std::strstr(profile,"microsoft")?"/input/squeeze/click":"/input/squeeze/value");
                add(triggerAction,"/input/trigger/value");add(stickAction,"/input/thumbstick");add(stickClickAction,"/input/thumbstick/click");
            }
            XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            info.interactionProfile=path(profile);info.countSuggestedBindings=uint32_t(bindings.size());info.suggestedBindings=bindings.data();
            // A runtime may not expose every profile. Its own bindings can supply others.
            const auto result=SuggestInteractionProfileBindings(instance,&info);
            if(result!=XR_ERROR_PATH_UNSUPPORTED) check(result,"suggest VR bindings");
        }
        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets=1;attach.actionSets=&actionSet;check(AttachSessionActionSets(session,&attach),"attach VR input");
        for(unsigned h=0;h<2;++h) {
            XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};info.subactionPath=hands[h];info.poseInActionSpace.orientation.w=1;
            info.action=gripAction;check(CreateActionSpace(session,&info,&gripSpaces[h]),"create grip space");
            info.action=aimAction;check(CreateActionSpace(session,&info,&aimSpaces[h]),"create aim space");
        }
    }
    void sampleInput() {
        controllers={};
        if(!focused) return;
        XrActiveActionSet active{actionSet,XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};sync.countActiveActionSets=1;sync.activeActionSets=&active;
        const auto result=SyncActions(session,&sync);if(result==XR_SESSION_NOT_FOCUSED) return;check(result,"sync VR input");
        for(unsigned h=0;h<2;++h) {
            auto& out=controllers[h];XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};get.subactionPath=hands[h];
            const auto locate=[&](XrAction action,XrSpace source,XrPosef& pose,bool& valid,bool& active,std::uint64_t& flags) {
                get.action=action;XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};check(GetActionStatePose(session,&get,&state),"get hand activity");
                active=state.isActive;
                if(!state.isActive) return;
                XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};check(LocateSpace(source,space,frame.predictedDisplayTime,&location),"locate hand");
                flags=location.locationFlags;
                // VALID poses may be inferred by the runtime between optical updates.
                // TRACKED bits are diagnostics, not a requirement to animate a hand.
                constexpr auto required=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
                valid=(location.locationFlags&required)==required;if(valid) pose=location.pose;
            };
            locate(gripAction,gripSpaces[h],out.grip,out.gripValid,out.gripActive,out.gripFlags);
            locate(aimAction,aimSpaces[h],out.aim,out.aimValid,out.aimActive,out.aimFlags);
            XrInteractionProfileState profile{XR_TYPE_INTERACTION_PROFILE_STATE};
            if(XR_SUCCEEDED(GetCurrentInteractionProfile(session,hands[h],&profile)) && profile.interactionProfile!=reportedProfiles[h]) {
                reportedProfiles[h]=profile.interactionProfile;char name[XR_MAX_PATH_LENGTH]{};uint32_t length=0;
                if(profile.interactionProfile) PathToString(instance,profile.interactionProfile,sizeof(name),&length,name);
                std::fprintf(stderr,"[vr input] %s profile %s\n",h?"right":"left",name[0]?name:"unbound");std::fflush(stderr);
            }
            const auto tracking=out.gripFlags|(out.aimFlags<<8)|(std::uint64_t(out.gripActive)<<32)|(std::uint64_t(out.aimActive)<<33);
            if(tracking!=reportedTracking[h]) {
                reportedTracking[h]=tracking;
                std::fprintf(stderr,"[vr input] %s grip active %d valid %d flags %llu; aim active %d valid %d flags %llu\n",h?"right":"left",out.gripActive,out.gripValid,static_cast<unsigned long long>(out.gripFlags),out.aimActive,out.aimValid,static_cast<unsigned long long>(out.aimFlags));std::fflush(stderr);
            }
            const auto value=[&](XrAction action) {get.action=action;XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};check(GetActionStateFloat(session,&get,&state),"get VR analog");return state.isActive&&std::isfinite(state.currentState)?std::clamp(state.currentState,0.0f,1.0f):0.0f;};
            out.squeeze=value(squeezeAction);out.trigger=value(triggerAction);
            sampleFingers(h);
            out.inputActive=out.gripValid||out.aimValid;
            for(unsigned b=0;b<buttons.size();++b) {
                get.action=buttons[b];XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
                check(GetActionStateBoolean(session,&get,&state),"get VR button");
                out.inputActive=out.inputActive||state.isActive;
                if(state.isActive&&state.currentState) out.buttons|=1u<<b;
            }
            get.action=stickClickAction;XrActionStateBoolean click{XR_TYPE_ACTION_STATE_BOOLEAN};
            check(GetActionStateBoolean(session,&get,&click),"get VR thumbstick click");out.stickClick=click.isActive&&click.currentState;
            get.action=stickAction;XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};check(GetActionStateVector2f(session,&get,&state),"get VR stick");if(state.isActive&&std::isfinite(state.currentState.x)&&std::isfinite(state.currentState.y)) out.stick={std::clamp(state.currentState.x,-1.0f,1.0f),std::clamp(state.currentState.y,-1.0f,1.0f)};
        }
    }
    XrGraphicsRequirementsD3D12KHR graphics{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    std::array<XrViewConfigurationView,2> sizes{{{XR_TYPE_VIEW_CONFIGURATION_VIEW},{XR_TYPE_VIEW_CONFIGURATION_VIEW}}};
    std::array<XrView,2> views{{{XR_TYPE_VIEW},{XR_TYPE_VIEW}}};
    std::array<XrSwapchain,3> swapchains{};
    std::array<std::vector<XrSwapchainImageD3D12KHR>,3> images;
    std::array<bool,3> acquired{}, rendered{}, hasImage{}, reused{};
    std::array<XrView,2> imageViews{{{XR_TYPE_VIEW},{XR_TYPE_VIEW}}};
    XrFrameState frame{XR_TYPE_FRAME_STATE};
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool running=false, frameOpen=false, poseValid=false, alive=true;
    template<class T> T function(const char* name) {
        PFN_xrVoidFunction out = nullptr;
        check(getProc(instance,name,&out), name);
        return reinterpret_cast<T>(out);
    }
    ~Impl() {
        // Resource owners must have stopped GPU work before destroying the session.
        if (session && DestroySession) {
            if(DestroyHandTrackerEXT)for(auto tracker:handTrackers)if(tracker)DestroyHandTrackerEXT(tracker);
            for(auto handSpace:gripSpaces) if(handSpace) DestroySpace(handSpace);
            for(auto handSpace:aimSpaces) if(handSpace) DestroySpace(handSpace);
            for (auto chain : swapchains) if (chain && DestroySwapchain) DestroySwapchain(chain);
            if (space && DestroySpace) DestroySpace(space);
            DestroySession(session);
        }
        if(actionSet && DestroyActionSet) DestroyActionSet(actionSet);
        if (instance && DestroyInstance) DestroyInstance(instance);
        if (loader) FreeLibrary(loader);
    }
};
OpenXrSession::OpenXrSession() : impl(std::make_unique<Impl>()) {}
OpenXrSession::~OpenXrSession() = default;
void OpenXrSession::prepare(const std::wstring& path) {
    auto& s=*impl;
    if (s.loader) throw std::logic_error("OpenXR already prepared");
    s.loader=LoadLibraryW(path.c_str());
    if (!s.loader) throw std::runtime_error("Cannot load OpenXR loader: "+std::to_string(GetLastError()));
    s.getProc=reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(s.loader,"xrGetInstanceProcAddr"));
    if (!s.getProc) throw std::runtime_error("Missing xrGetInstanceProcAddr");
    const auto enumerate=s.function<PFN_xrEnumerateInstanceExtensionProperties>("xrEnumerateInstanceExtensionProperties");
    uint32_t count=0;
    check(enumerate(nullptr,0,&count,nullptr),"enumerate extension count");
    std::vector<XrExtensionProperties> extensions(count,{XR_TYPE_EXTENSION_PROPERTIES});
    check(enumerate(nullptr,count,&count,extensions.data()),"enumerate extensions");
    if (std::none_of(extensions.begin(),extensions.end(),[](const auto& e){return std::strcmp(e.extensionName,XR_KHR_D3D12_ENABLE_EXTENSION_NAME)==0;}))
        throw std::runtime_error("OpenXR runtime does not support D3D12");
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(info.applicationInfo.applicationName,"Luigis Mansion VR");
    std::strcpy(info.applicationInfo.engineName,"Aurora LMVR");
    info.applicationInfo.apiVersion=XR_MAKE_VERSION(1,0,0);
    std::vector<const char*> requested{XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
    s.frameProfile=std::any_of(extensions.begin(),extensions.end(),[](const auto& e){return std::strcmp(e.extensionName,"XR_VALVE_frame_controller_interaction")==0;});
    if(s.frameProfile) requested.push_back("XR_VALVE_frame_controller_interaction");
    const auto optional=[&](const char* name){const bool found=std::any_of(extensions.begin(),extensions.end(),[&](const auto& e){return std::strcmp(e.extensionName,name)==0;});if(found)requested.push_back(name);return found;};
    s.handTracking=optional(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    if(s.handTracking) {s.handDataSource=optional(XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME);s.handMotionRange=optional(XR_EXT_HAND_JOINTS_MOTION_RANGE_EXTENSION_NAME);}
    std::fprintf(stderr,"[vr fingers] hand tracking extension %s\n",s.handTracking?"enabled":"unavailable; analog fallback");
    info.enabledExtensionCount=uint32_t(requested.size());info.enabledExtensionNames=requested.data();
    check(s.function<PFN_xrCreateInstance>("xrCreateInstance")(&info,&s.instance),"create instance");
#define LOAD(name) s.name=s.function<PFN_xr##name>("xr" #name);
    XR_FUNCTIONS(LOAD)
#undef LOAD
#undef XR_FUNCTIONS
    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO}; systemInfo.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    check(s.GetSystem(s.instance,&systemInfo,&s.system),"get HMD system");
    check(s.GetD3D12GraphicsRequirementsKHR(s.instance,s.system,&s.graphics),"get D3D12 graphics requirements");
    check(s.EnumerateViewConfigurationViews(s.instance,s.system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,0,&count,nullptr),"get stereo view count");
    if (count!=2) throw std::runtime_error("Expected two stereo views");
    check(s.EnumerateViewConfigurationViews(s.instance,s.system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,2,&count,s.sizes.data()),"get stereo views");
}
const XrGraphicsRequirementsD3D12KHR& OpenXrSession::requirements() const {return impl->graphics;}
void OpenXrSession::attach(ID3D12Device* device, ID3D12CommandQueue* queue) {
    auto& s=*impl;
    if (!s.system || s.session || !device || !queue) throw std::logic_error("Invalid OpenXR attach state");
    const auto luid=device->GetAdapterLuid();
    if (luid.LowPart!=s.graphics.adapterLuid.LowPart || luid.HighPart!=s.graphics.adapterLuid.HighPart)
        throw std::runtime_error("OpenXR requires a different graphics adapter");
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR}; binding.device=device; binding.queue=queue;
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO}; info.next=&binding; info.systemId=s.system;
    check(s.CreateSession(s.instance,&info,&s.session),"create D3D12 session");
    XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    reference.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL; reference.poseInReferenceSpace.orientation.w=1;
    check(s.CreateReferenceSpace(s.session,&reference,&s.space),"create local space");
    s.createInput();
    s.createFingerTracking();
    uint32_t count=0;
    check(s.EnumerateSwapchainFormats(s.session,0,&count,nullptr),"get format count");
    std::vector<int64_t> formats(count);
    check(s.EnumerateSwapchainFormats(s.session,count,&count,formats.data()),"get formats");
    for (auto candidate : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM})
        if (std::find(formats.begin(),formats.end(),candidate)!=formats.end()) { s.format=candidate; break; }
    if (s.format==DXGI_FORMAT_UNKNOWN) throw std::runtime_error("No supported OpenXR color format");
    for (unsigned eye=0;eye<3;++eye) {
        const auto& size=s.sizes[eye<2?eye:0];
        XrSwapchainCreateInfo chain{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        chain.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        chain.format=s.format; chain.sampleCount=1; chain.width=size.recommendedImageRectWidth;
        chain.height=size.recommendedImageRectHeight; chain.faceCount=chain.arraySize=chain.mipCount=1;
        check(s.CreateSwapchain(s.session,&chain,&s.swapchains[eye]),"create eye swapchain");
        check(s.EnumerateSwapchainImages(s.swapchains[eye],0,&count,nullptr),"get eye image count");
        s.images[eye].assign(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        check(s.EnumerateSwapchainImages(s.swapchains[eye],count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(s.images[eye].data())),"get eye images");
    }
}
bool OpenXrSession::poll() {
    auto& s=*impl;
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    for (;;) {
        const auto result=s.PollEvent(s.instance,&event);
        if (result==XR_EVENT_UNAVAILABLE) break;
        check(result,"poll events");
        if (event.type==XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) s.alive=false;
        if (event.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& change=*reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            if (change.session==s.session) {
                s.focused=change.state==XR_SESSION_STATE_FOCUSED;
                if (change.state==XR_SESSION_STATE_READY && !s.running) {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO}; begin.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    check(s.BeginSession(s.session,&begin),"begin session"); s.running=true;
                } else if (change.state==XR_SESSION_STATE_STOPPING) {
                    check(s.EndSession(s.session),"end session"); s.running=false;
                } else if (change.state==XR_SESSION_STATE_EXITING || change.state==XR_SESSION_STATE_LOSS_PENDING) s.alive=false;
            }
        }
        event={XR_TYPE_EVENT_DATA_BUFFER};
    }
    return s.alive;
}
bool OpenXrSession::beginFrame() {
    auto& s=*impl;
    if (s.frameOpen) throw std::logic_error("OpenXR frame already open");
    if (!s.running || !s.alive) return false;
    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
    check(s.WaitFrame(s.session,&wait,&s.frame),"wait frame");
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
    check(s.BeginFrame(s.session,&begin),"begin frame"); s.frameOpen=true; s.rendered={};s.reused={};
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO}; locate.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime=s.frame.predictedDisplayTime; locate.space=s.space;
    XrViewState state{XR_TYPE_VIEW_STATE}; uint32_t count=0;
    check(s.LocateViews(s.session,&locate,&state,2,&count,s.views.data()),"locate views");
    constexpr XrViewStateFlags flags=XR_VIEW_STATE_ORIENTATION_VALID_BIT|XR_VIEW_STATE_POSITION_VALID_BIT;
    s.poseValid=count==2 && (state.viewStateFlags&flags)==flags;
    s.sampleInput();
    return true;
}
const std::array<ControllerState,2>& OpenXrSession::controllers() const {return impl->controllers;}
void OpenXrSession::pulse(unsigned hand,float amplitude,float seconds) {
    auto& s=*impl;
    if(hand>1 || !s.focused || !s.running || !std::isfinite(amplitude) || !std::isfinite(seconds)) return;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};info.action=s.haptic;info.subactionPath=s.hands[hand];
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude=std::clamp(amplitude,0.0f,1.0f);vibration.frequency=XR_FREQUENCY_UNSPECIFIED;
    vibration.duration=XrDuration(std::clamp(seconds,0.001f,0.1f)*1000000000.0f);
    // Haptics are optional: unsupported/unavailable feedback must not stop rendering.
    s.ApplyHapticFeedback(s.session,&info,reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
}
bool OpenXrSession::shouldRender() const {return impl->frameOpen && impl->frame.shouldRender && impl->poseValid;}
const std::array<XrView,2>& OpenXrSession::views() const {return impl->views;}
const std::array<XrViewConfigurationView,2>& OpenXrSession::viewSizes() const {return impl->sizes;}
DXGI_FORMAT OpenXrSession::format() const {return impl->format;}
ID3D12Resource* OpenXrSession::acquireEye(unsigned eye) {
    auto& s=*impl;
    if (eye>=3 || !shouldRender() || s.acquired[eye]) throw std::logic_error("Invalid eye acquire state");
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO}; uint32_t index=0;
    check(s.AcquireSwapchainImage(s.swapchains[eye],&acquire,&index),"acquire eye image"); s.acquired[eye]=true;
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wait.timeout=XR_INFINITE_DURATION;
    check(s.WaitSwapchainImage(s.swapchains[eye],&wait),"wait eye image");
    return s.images[eye].at(index).texture;
}
bool OpenXrSession::reuseEye(unsigned eye) {
    auto& s=*impl;
    if(eye>=2||!s.frameOpen||!s.hasImage[eye]||s.acquired[eye])return false;
    s.rendered[eye]=true;s.reused[eye]=true;return true;
}
void OpenXrSession::releaseEye(unsigned eye) {
    auto& s=*impl;
    if (eye>=3 || !s.acquired[eye]) throw std::logic_error("Eye image was not acquired");
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    check(s.ReleaseSwapchainImage(s.swapchains[eye],&release),"release eye image"); s.acquired[eye]=false; s.rendered[eye]=true;s.hasImage[eye]=true;
    if(eye<2)s.imageViews[eye]=s.views[eye];
}
void OpenXrSession::endFrame(bool submitEyes, const XrPosef* panelPose, float panelAspect, bool stereoPanel, const XrPosef* overlayPose) {
    auto& s=*impl;
    if (!s.frameOpen || s.acquired[0] || s.acquired[1] || s.acquired[2]) throw std::logic_error("Invalid OpenXR end frame state");
    if (submitEyes && (!shouldRender() || !s.rendered[0] || ((!panelPose || stereoPanel) && !s.rendered[1])))
        throw std::logic_error("Required view images were not rendered before submission");
    std::array<XrCompositionLayerProjectionView,2> views{};
    for (unsigned eye=0;eye<2;++eye) {
        auto& view=views[eye]; view.type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        const auto& imageView=s.reused[eye]?s.imageViews[eye]:s.views[eye];
        view.pose=imageView.pose; view.fov=imageView.fov; view.subImage.swapchain=s.swapchains[eye];
        view.subImage.imageRect.extent={int32_t(s.sizes[eye].recommendedImageRectWidth),int32_t(s.sizes[eye].recommendedImageRectHeight)};
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION}; layer.space=s.space; layer.viewCount=2; layer.views=views.data();
    const auto* header=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
    std::array<XrCompositionLayerQuad,2> panels{};
    const XrCompositionLayerBaseHeader* layers[3]{header,nullptr};
    if(panelPose) {
        for(unsigned eye=0;eye<(stereoPanel?2u:1u);++eye) {
            auto& panel=panels[eye];panel.type=XR_TYPE_COMPOSITION_LAYER_QUAD;
            panel.space=s.space;panel.eyeVisibility=stereoPanel?(eye?XR_EYE_VISIBILITY_RIGHT:XR_EYE_VISIBILITY_LEFT):XR_EYE_VISIBILITY_BOTH;
            panel.pose=*panelPose;panel.size={1.4f,1.4f/panelAspect};panel.subImage=views[eye].subImage;
            layers[eye]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&panel);
        }
    }
    unsigned layerCount=panelPose&&stereoPanel?2:1;
    XrCompositionLayerQuad overlay{XR_TYPE_COMPOSITION_LAYER_QUAD};
    if(overlayPose&&submitEyes) {
        if(!s.rendered[2])throw std::logic_error("Settings overlay image missing");
        overlay.space=s.space;overlay.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
        overlay.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        overlay.pose=*overlayPose;overlay.size={1.4f,1.4f/panelAspect};
        overlay.subImage.swapchain=s.swapchains[2];overlay.subImage.imageRect.extent=views[0].subImage.imageRect.extent;
        layers[layerCount++]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&overlay);
    }
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO}; end.displayTime=s.frame.predictedDisplayTime; end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount=submitEyes?layerCount:0; end.layers=submitEyes?layers:nullptr;
    const auto result=s.EndFrame(s.session,&end); s.frameOpen=false; check(result,"end frame");
}
}
#endif

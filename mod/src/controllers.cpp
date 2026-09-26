#include "controllers.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "log.h"

namespace rfgvr::controllers {
namespace {

XrInstance g_instance = XR_NULL_HANDLE;
XrActionSet g_set = XR_NULL_HANDLE;
XrPath g_handPath[2] = {};
XrAction g_gripPose = XR_NULL_HANDLE, g_aimPose = XR_NULL_HANDLE;
XrAction g_trigger = XR_NULL_HANDLE, g_squeeze = XR_NULL_HANDLE;
XrAction g_a = XR_NULL_HANDLE, g_b = XR_NULL_HANDLE, g_stick = XR_NULL_HANDLE, g_stickClick = XR_NULL_HANDLE;
XrSpace g_gripSpace[2] = {}, g_aimSpace[2] = {};

std::mutex g_mutex;
State g_state;
bool g_valid = false;

bool ok(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    LOG("controllers: %s -> %d", what, static_cast<int>(r));
    return false;
}

XrPath path(const char* s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(g_instance, s, &p);
    return p;
}

XrAction makeAction(const char* name, const char* localized, XrActionType type) {
    XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
    strcpy_s(aci.actionName, name);
    strcpy_s(aci.localizedActionName, localized);
    aci.actionType = type;
    aci.countSubactionPaths = 2;
    aci.subactionPaths = g_handPath;
    XrAction a = XR_NULL_HANDLE;
    ok(xrCreateAction(g_set, &aci, &a), name);
    return a;
}

struct Binding {
    XrAction* action;
    const char* suffix;  // after /user/hand/<side>
};

// Bindings per profile; the same input name is used for both hands unless noted.
void suggest(const char* profile, const Binding* both, size_t nBoth, const Binding* left, size_t nLeft,
             const Binding* right, size_t nRight) {
    std::vector<XrActionSuggestedBinding> list;
    char buf[256];
    for (int h = 0; h < 2; ++h) {
        const char* side = h == kLeft ? "left" : "right";
        auto add = [&](const Binding* b, size_t n) {
            for (size_t i = 0; i < n; ++i) {
                snprintf(buf, sizeof buf, "/user/hand/%s%s", side, b[i].suffix);
                list.push_back({*b[i].action, path(buf)});
            }
        };
        add(both, nBoth);
        add(h == kLeft ? left : right, h == kLeft ? nLeft : nRight);
    }
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = path(profile);
    sb.suggestedBindings = list.data();
    sb.countSuggestedBindings = static_cast<uint32_t>(list.size());
    ok(xrSuggestInteractionProfileBindings(g_instance, &sb), profile);
}

template <size_t N>
constexpr size_t count(const Binding (&)[N]) {
    return N;
}

float getFloat(XrSession s, XrAction a, int hand) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = g_handPath[hand];
    XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(xrGetActionStateFloat(s, &gi, &st)) && st.isActive ? st.currentState : 0.f;
}

bool getBool(XrSession s, XrAction a, int hand) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = g_handPath[hand];
    XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(s, &gi, &st)) && st.isActive && st.currentState;
}

XrVector2f getVec2(XrSession s, XrAction a, int hand) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = g_handPath[hand];
    XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
    return XR_SUCCEEDED(xrGetActionStateVector2f(s, &gi, &st)) && st.isActive ? st.currentState : XrVector2f{};
}

}  // namespace

bool create(XrInstance instance, XrSession session) {
    g_instance = instance;
    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(asci.actionSetName, "gameplay");
    strcpy_s(asci.localizedActionSetName, "Gameplay");
    if (!ok(xrCreateActionSet(instance, &asci, &g_set), "xrCreateActionSet")) return false;
    g_handPath[kLeft] = path("/user/hand/left");
    g_handPath[kRight] = path("/user/hand/right");

    g_gripPose = makeAction("grip_pose", "Hand pose", XR_ACTION_TYPE_POSE_INPUT);
    g_aimPose = makeAction("aim_pose", "Aim pose", XR_ACTION_TYPE_POSE_INPUT);
    g_trigger = makeAction("trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT);
    g_squeeze = makeAction("squeeze", "Grip", XR_ACTION_TYPE_FLOAT_INPUT);
    g_a = makeAction("button_a", "A", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_b = makeAction("button_b", "B", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_stick = makeAction("thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT);
    g_stickClick = makeAction("thumbstick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT);

    const Binding index[] = {{&g_gripPose, "/input/grip/pose"},     {&g_aimPose, "/input/aim/pose"},
                             {&g_trigger, "/input/trigger/value"},  {&g_squeeze, "/input/squeeze/value"},
                             {&g_a, "/input/a/click"},              {&g_b, "/input/b/click"},
                             {&g_stick, "/input/thumbstick"},       {&g_stickClick, "/input/thumbstick/click"}};
    suggest("/interaction_profiles/valve/index_controller", index, count(index), nullptr, 0, nullptr, 0);

    const Binding touch[] = {{&g_gripPose, "/input/grip/pose"},    {&g_aimPose, "/input/aim/pose"},
                             {&g_trigger, "/input/trigger/value"}, {&g_squeeze, "/input/squeeze/value"},
                             {&g_stick, "/input/thumbstick"},      {&g_stickClick, "/input/thumbstick/click"}};
    const Binding touchLeft[] = {{&g_a, "/input/x/click"}, {&g_b, "/input/y/click"}};
    const Binding touchRight[] = {{&g_a, "/input/a/click"}, {&g_b, "/input/b/click"}};
    suggest("/interaction_profiles/oculus/touch_controller", touch, count(touch), touchLeft, count(touchLeft),
            touchRight, count(touchRight));

    const Binding simple[] = {{&g_gripPose, "/input/grip/pose"},
                              {&g_aimPose, "/input/aim/pose"},
                              {&g_trigger, "/input/select/click"},
                              {&g_b, "/input/menu/click"}};
    suggest("/interaction_profiles/khr/simple_controller", simple, count(simple), nullptr, 0, nullptr, 0);

    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo sci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sci.subactionPath = g_handPath[h];
        sci.poseInActionSpace.orientation.w = 1.f;
        sci.action = g_gripPose;
        ok(xrCreateActionSpace(session, &sci, &g_gripSpace[h]), "grip space");
        sci.action = g_aimPose;
        ok(xrCreateActionSpace(session, &sci, &g_aimSpace[h]), "aim space");
    }

    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &g_set;
    if (!ok(xrAttachSessionActionSets(session, &ai), "xrAttachSessionActionSets")) return false;
    LOG("controllers: actions created");
    return true;
}

void sync(XrSession session, XrSpace base, XrTime time) {
    if (g_set == XR_NULL_HANDLE) return;
    XrActiveActionSet active{g_set, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(session, &si))) return;

    State s;
    for (int h = 0; h < 2; ++h) {
        HandState& hs = s.hand[h];
        XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION, &vel};
        constexpr XrSpaceLocationFlags kTracked =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if (XR_SUCCEEDED(xrLocateSpace(g_gripSpace[h], base, time, &loc)) &&
            (loc.locationFlags & kTracked) == kTracked) {
            hs.active = true;
            hs.grip = loc.pose;
            if (vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) hs.velocity = vel.linearVelocity;
        }
        XrSpaceLocation aim{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(g_aimSpace[h], base, time, &aim)) && (aim.locationFlags & kTracked) == kTracked)
            hs.aim = aim.pose;
        else
            hs.aim = hs.grip;
        hs.trigger = getFloat(session, g_trigger, h);
        hs.squeeze = getFloat(session, g_squeeze, h);
        hs.a = getBool(session, g_a, h);
        hs.b = getBool(session, g_b, h);
        hs.stickClick = getBool(session, g_stickClick, h);
        hs.stick = getVec2(session, g_stick, h);
    }

    static DWORD lastLog = 0;
    if (DWORD now = GetTickCount(); now - lastLog > 2000) {
        lastLog = now;
        for (int h = 0; h < 2; ++h) {
            const HandState& hs = s.hand[h];
            LOG("controller %s: tracked %d pos %.2f %.2f %.2f trig %.2f grip %.2f a %d b %d stick %.2f %.2f click %d",
                h ? "R" : "L", hs.active, hs.grip.position.x, hs.grip.position.y, hs.grip.position.z, hs.trigger,
                hs.squeeze, hs.a, hs.b, hs.stick.x, hs.stick.y, hs.stickClick);
        }
    }

    std::scoped_lock lock(g_mutex);
    g_state = s;
    g_valid = true;
}

void destroy() {
    {
        std::scoped_lock lock(g_mutex);
        g_valid = false;
    }
    for (int h = 0; h < 2; ++h) {
        if (g_gripSpace[h] != XR_NULL_HANDLE) xrDestroySpace(g_gripSpace[h]);
        if (g_aimSpace[h] != XR_NULL_HANDLE) xrDestroySpace(g_aimSpace[h]);
        g_gripSpace[h] = g_aimSpace[h] = XR_NULL_HANDLE;
    }
    if (g_set != XR_NULL_HANDLE) xrDestroyActionSet(g_set);  // destroys its actions
    g_set = XR_NULL_HANDLE;
}

bool state(State& out) {
    std::scoped_lock lock(g_mutex);
    if (!g_valid) return false;
    out = g_state;
    return true;
}

}  // namespace rfgvr::controllers

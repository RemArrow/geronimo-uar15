// UAR15Dev - automated in-game test of the UAR-15 plugin. Not shipped; installed only for a run.
// Loads the team room and, at the gun station, through the spawner's own menu event:
//   1. spawns a stock M4A1 Block II and logs its layout (components, attach points, movables bones), and renders it;
//   2. spawns the UAR-15, holds it still in the air and logs it (meshes on, the M4's hidden, moving parts) and
//      renders it with a scene capture of its own to [Test] ShotDir;
//   3. mounts an optic from the station's menu (the UAR-15's iron sights hide);
//   4. fires it through the game's own FireRoundLocal (one 5.56 round);
//   5. pulls its charging handle (half, full, home), the trigger, the mag release, and steps the selector;
//   6. saves it as a loadout gun and rebuilds the loadout, as an operation does.
#include <GML/GML.hpp>
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <format>

using namespace gml;

GML_PLUGIN("com.remarrow.uar15dev", "UAR15Dev", "0.1.0", {{"com.remarrow.uar15", "0.1.0", GML_DEPENDENCY_SOFT}});

struct Vec { double x, y, z; };
struct Rot { double pitch, yaw, roll; };

static const char* kItemPress =
    "BndEvt__WB_CustomizationItem_Button_46_K2Node_ComponentBoundEvent_1_OnButtonPressedEvent__DelegateSignature";
static const char* kConfirm =
    "BndEvt__WB_GunCustomization_WB_ConfirmWindow_K2Node_ComponentBoundEvent_0_OnConfirm__DelegateSignature";
static const char* kDonor = "/Game/Weapons/Data/DA_M4A1_BlockII.DA_M4A1_BlockII";

static std::string s_shotDir;
static Object s_station, s_widget, s_uar, s_donor, s_gun, s_rt, s_cap, s_gear;
static int s_step = -1, s_wait = 0, s_fallbacks = 0;
static GUObject* s_before = nullptr;
static Object s_target;  // the entry being spawned
static Vec s_holdAt{};
static bool s_holding = false;
static int s_rounds = 0;  // rounds that began play since the last reset
static uint8_t s_capSource = 7;  // ESceneCaptureSource: 7 base colour, 6 world normal, 2 final colour (lit)
static Object s_light;

static std::string TextOf(Object o, const char* prop) {
    char b[128] = "";
    if (void* t = o.PropPtr(prop)) API->TextToString(t, b, sizeof b);
    return b;
}
static std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }
static std::string FStr8(Object o, const char* prop) {
    char b[256] = "";
    if (auto* s = (const GML_FString*)o.PropPtr(prop)) API->FStringToUtf8(s, b, sizeof b);
    return b;
}
static std::string NameStr(uint64_t n) {
    char b[96] = "";
    API->NameToString(n, b, sizeof b);
    return b;
}
static std::string ClassName(Object o) { return o ? Object((GUObject*)o.Class()).Name() : "-"; }
static Object Pawn() {
    Params p(Lib("GameplayStatics"), "GetPlayerPawn");
    p.SetObj("WorldContextObject", Object(API->WorldContext())).Set("PlayerIndex", (int32_t)0).Call();
    return p.Return<GUObject*>();
}
static Vec ActorLocation(Object a) {
    Params p(a, "K2_GetActorLocation");
    p.Call();
    return p.Return<Vec>();
}
static std::vector<Object> Components(Object actor, const char* cls) {
    std::vector<Object> out;
    Params gc(actor, "K2_GetComponentsByClass");
    gc.Set("ComponentClass", FindClass(cls)).Call();
    auto* arr = (GML_TArray*)gc.Ptr("ReturnValue");
    for (int i = 0; arr && i < arr->Num; i++) out.push_back(((GUObject**)arr->Data)[i]);
    return out;
}
static bool Visible(Object c) {
    Params v(c, "IsVisible");
    v.Call();
    return v.Return<bool>();
}
static Vec ToActor(Object actor, Vec world) {
    Params gt(actor, "GetTransform");
    gt.Call();
    Params inv(Lib("KismetMathLibrary"), "InverseTransformLocation");
    GML_PropInfo ri;
    API->GetPropertyInfo(API->FindProperty((GUStruct*)API->FindFunction(actor.Struct(), "GetTransform"), "ReturnValue"), &ri);
    std::memcpy(inv.Ptr("T"), gt.Ptr("ReturnValue"), ri.size);
    inv.Set("Location", world).Call();
    return inv.Return<Vec>();
}
static Vec CompLocation(Object c) {
    Params loc(c, "K2_GetComponentLocation");
    loc.Call();
    return loc.Return<Vec>();
}
static Rot CompRotationInActor(Object actor, Object c) {  // world rotation of c, the actor held unrotated
    Params r(c, "K2_GetComponentRotation");
    r.Call();
    return r.Return<Rot>();
}

// Hold the gun still in the air in front of our capture camera (the station eases it back to the table).
static void Hold() {
    if (!s_holding || !s_gun) return;
    Params mv(s_gun, "K2_SetActorLocationAndRotation");
    mv.Set("NewLocation", s_holdAt).Set("NewRotation", Rot{0, 0, 0}).Set("bSweep", false).Set("bTeleport", true).Call();
}
static void Freeze(Object gun) {
    if (Object root = gun.GetObj("StaticMeshComponent")) {
        Params p(root, "SetSimulatePhysics");
        p.Set("bSimulate", false).Call();
    }
}

static void EnsureCapture() {
    if (s_rt) return;
    Params rt(Lib("KismetRenderingLibrary"), "CreateRenderTarget2D");
    rt.SetObj("WorldContextObject", Object(API->WorldContext())).Set("Width", (int32_t)1800).Set("Height", (int32_t)800)
        .Set("Format", (uint8_t)2).Call();  // RTF_RGBA8
    s_rt = rt.Return<GUObject*>();
    Params mk(Lib("KismetMathLibrary"), "MakeTransform");
    mk.Set("Scale", Vec{1, 1, 1}).Call();
    GML_PropInfo xi;
    API->GetPropertyInfo(API->FindProperty((GUStruct*)API->FindFunction(Lib("KismetMathLibrary").Struct(), "MakeTransform"), "ReturnValue"), &xi);
    Params b(Lib("GameplayStatics"), "BeginDeferredActorSpawnFromClass");
    b.SetObj("WorldContextObject", Object(API->WorldContext())).Set("ActorClass", FindClass("SceneCapture2D")).Set("CollisionHandlingOverride", (uint8_t)1);
    std::memcpy(b.Ptr("SpawnTransform"), mk.Ptr("ReturnValue"), xi.size);
    b.Call();
    Object actor = b.Return<GUObject*>();
    Params f(Lib("GameplayStatics"), "FinishSpawningActor");
    f.SetObj("Actor", actor);
    std::memcpy(f.Ptr("SpawnTransform"), mk.Ptr("ReturnValue"), xi.size);
    f.Call();
    s_cap = actor.GetObj("CaptureComponent2D");
    s_cap.Set<GUObject*>("TextureTarget", s_rt.ptr);
    s_cap.SetBool("bCaptureEveryFrame", false);
    s_cap.Set("FOVAngle", 40.0f);
}

// From `from` toward `at` (both actor space; the gun is held unrotated at s_holdAt), base colour, the gun and its
// magazine only.
static void CaptureFrom(const std::string& file, Vec from, Vec at) {
    EnsureCapture();
    s_cap.Set<uint8_t>("PrimitiveRenderMode", 2);  // PRM_UseShowOnlyList
    if (auto* only = (GML_TArray*)s_cap.PropPtr("ShowOnlyActors")) {
        only->Num = 0;
        Params ga(s_gun, "GetAttachedActors");
        ga.Set("bResetArray", true).Set("bRecursivelyIncludeAttachedActors", true).Call();
        std::vector<Object> actors{s_gun, s_gun.GetObj("CurrentMagazine")};
        if (auto* arr = (GML_TArray*)ga.Ptr("OutActors"))
            for (int i = 0; i < arr->Num; i++) actors.push_back(((GUObject**)arr->Data)[i]);
        for (Object a : actors)
            if (a) *(GUObject**)API->ArrayAdd(only, sizeof(GUObject*), 1) = a.ptr;
    }
    Vec f{s_holdAt.x + from.x, s_holdAt.y + from.y, s_holdAt.z + from.z}, t{s_holdAt.x + at.x, s_holdAt.y + at.y, s_holdAt.z + at.z};
    Params look(Lib("KismetMathLibrary"), "FindLookAtRotation");
    look.Set("Start", f).Set("Target", t).Call();
    Params mv(Object(API->GetOuter(s_cap.ptr)), "K2_SetActorLocationAndRotation");
    mv.Set("NewLocation", f).Set("NewRotation", look.Return<Rot>()).Set("bSweep", false).Set("bTeleport", true).Call();
    s_cap.Set("CaptureSource", s_capSource);  // SCS_BaseColor unless a step asks for normals or the lit scene
    Params cs(s_cap, "CaptureScene");
    cs.Call();
    Params ex(Lib("KismetRenderingLibrary"), "ExportRenderTarget");
    ex.SetObj("WorldContextObject", Object(API->WorldContext())).SetObj("TextureRenderTarget", s_rt)
        .SetString("FilePath", W(s_shotDir)).SetString("Filename", W(file + ".png")).Call();
}
// The whole gun from its right, left and above, centred on X `cx`.
static void CaptureGun(const std::string& file, double cx) {
    CaptureFrom(file + "_right", {cx, 230, 10}, {cx, 0, 0});
    CaptureFrom(file + "_left", {cx, -230, 10}, {cx, 0, 0});
    CaptureFrom(file + "_top", {cx, 20, 230}, {cx, 0, 0});
    Log("SHOT {}", file);
}

// Everything on the receiver: components (class, parent, socket, where in actor space), movables bones,
// magazine, charging handle, the round.
static void DumpGun(const char* what) {
    Object g = s_gun;
    Log("DUMP [{}] {} class {} BulletData {} rpm {:.0f}/{:.0f} mode {} category? ", what, g.Name(), ClassName(g),
        Object((GUObject*)g.Get<GUClass*>("BulletData")).Name(), g.Get<float>("FireRateRPMMin"), g.Get<float>("FireRateRPMMax"),
        g.Get<uint8_t>("FireMode"));
    Vec rps = g.Get<Vec>("RotationPerShot"), lps = g.Get<Vec>("LocationPerShot"), mr = g.Get<Vec>("MaxRotation"), ml = g.Get<Vec>("MaxLocation");
    Log("DUMP   recoil per shot rot ({:.2f},{:.2f},{:.2f}) loc ({:.2f},{:.2f},{:.2f}) max rot ({:.2f},{:.2f},{:.2f}) loc ({:.2f},{:.2f},{:.2f}); sound {}",
        rps.x, rps.y, rps.z, lps.x, lps.y, lps.z, mr.x, mr.y, mr.z, ml.x, ml.y, ml.z, g.GetObj("AudioData").Name());
    for (auto& c : Components(g, "SceneComponent")) {
        Params sn(c, "GetAttachSocketName");
        sn.Call();
        Object parent = c.GetObj("AttachParent");
        Vec l = ToActor(g, CompLocation(c)), rel = c.Get<Vec>("RelativeLocation");
        Rot rr = c.Get<Rot>("RelativeRotation");
        std::string extra;
        if (c.IsA("StaticMeshComponent")) extra = " mesh " + c.GetObj("StaticMesh").Name();
        if (c.IsA("SkinnedMeshComponent")) extra = " mesh " + c.GetObj("SkeletalMesh").Name() + c.GetObj("SkinnedAsset").Name();
        if (c.IsA("BoxComponent")) { Vec e = c.Get<Vec>("BoxExtent"); extra += std::format(" box extent ({:.2f},{:.2f},{:.2f})", e.x, e.y, e.z); }
        Log("DUMP   {:<34} [{}] parent {} socket '{}' at ({:.2f},{:.2f},{:.2f}) rel ({:.2f},{:.2f},{:.2f}) rot ({:.1f},{:.1f},{:.1f}) visible {}{}",
            c.Name(), ClassName(c), parent.Name(), NameStr(sn.Return<uint64_t>()), l.x, l.y, l.z, rel.x, rel.y, rel.z, rr.pitch, rr.yaw, rr.roll,
            Visible(c), extra);
    }
    if (Object mov = g.GetObj("Movables")) {
        Params nb(mov, "GetNumBones");
        nb.Call();
        int n = nb.Return<int32_t>();
        std::string bones;
        for (int i = 0; i < n; i++) {
            Params bn(mov, "GetBoneName");
            bn.Set("BoneIndex", (int32_t)i).Call();
            uint64_t name = bn.Return<uint64_t>();
            Params bl(mov, "GetSocketLocation");
            *(uint64_t*)bl.Ptr("InSocketName") = name;
            bl.Call();
            Vec l = ToActor(g, bl.Return<Vec>());
            bones += std::format(" {}({:.2f},{:.2f},{:.2f})", NameStr(name), l.x, l.y, l.z);
        }
        Log("DUMP   movables {} bones:{}", n, bones);
    }
    if (GFProperty* hs = API->FindProperty(g.Struct(), "ChargingHandleSettings")) {
        GML_PropInfo si{};
        API->GetPropertyInfo(hs, &si);
        std::string f;
        for (GFProperty* p = si.structType ? API->NextProperty(si.structType, nullptr) : nullptr; p; p = API->NextProperty(si.structType, p)) {
            GML_PropInfo i{};
            char name[64] = "";
            API->GetPropertyInfo(p, &i);
            API->GetPropertyName(p, name, sizeof name);
            if (!strcmp(i.type, "FloatProperty")) f += std::format(" {}={:.3f}", name, *(float*)((uint8_t*)g.ptr + si.offset + i.offset));
        }
        Log("DUMP   ChargingHandleSettings:{}", f);
    }
    Object mag = g.GetObj("CurrentMagazine");
    if (mag) {
        Vec ml2 = ToActor(g, ActorLocation(mag));
        Log("DUMP   magazine {} [{}] capacity {} ammo {} BulletData {} at ({:.2f},{:.2f},{:.2f})", mag.Name(), ClassName(mag), mag.Get<int32_t>("AmmoCapacity"),
            mag.Get<int32_t>("CurrentAmmo"), Object((GUObject*)mag.Get<GUClass*>("BulletData")).Name(), ml2.x, ml2.y, ml2.z);
        for (auto& c : Components(mag, "SceneComponent")) {
            Vec l = ToActor(g, CompLocation(c));
            Log("DUMP     {:<30} [{}] at ({:.2f},{:.2f},{:.2f}) visible {}", c.Name(), ClassName(c), l.x, l.y, l.z, Visible(c));
        }
    }
    auto* acc = (GML_TArray*)g.PropPtr("AcceptedMagazines");
    for (int i = 0; acc && i < acc->Num; i++) Log("DUMP   accepts {}", Object(((GUObject**)acc->Data)[i]).Name());
    Params ga(g, "GetAttachedActors");
    ga.Set("bResetArray", true).Set("bRecursivelyIncludeAttachedActors", true).Call();
    if (auto* arr = (GML_TArray*)ga.Ptr("OutActors"))
        for (int i = 0; i < arr->Num; i++) {
            Object a(((GUObject**)arr->Data)[i]);
            Vec l = ToActor(g, ActorLocation(a));
            Log("DUMP   attached actor {} [{}] at ({:.2f},{:.2f},{:.2f}) tag '{}'", a.Name(), ClassName(a), l.x, l.y, l.z,
                a.IsA("GunAttachment") ? NameStr(a.Get<uint64_t>("AttachPointTag")) : std::string());
        }
    if (GUClass* round = g.Get<GUClass*>("BulletData")) {
        Object d = DefaultObject(round);
        Log("DUMP   round defaults {}: Mass {:.4f} Diameter {:.3f} velocity {:.0f}-{:.0f} FormFactor {:.3f} penetration {:.2f}-{:.2f} pooling {} DamageCurve {}",
            Object((GUObject*)round).Name(), d.Get<float>("Mass"), d.Get<float>("Diameter"), d.Get<float>("MuzzleVelocityMin"),
            d.Get<float>("MuzzleVelocityMax"), d.Get<float>("FormFactor"), d.Get<float>("MinPenetration"), d.Get<float>("MaxPenetration"),
            d.GetBool("EnablePooling"), d.GetObj("DamageCurve").Name());
    }
}

// Opens a category's page as the VR pointer does: its button's bound event (which calls the station's
// ShowWeapons with the category). 0 = Assault rifles, the M4's.
static void OpenCategory(uint8_t category) {
    static const char* buttons[] = {
        "BndEvt__WB_GunCustomization_CategoryBtn_Body_K2Node_ComponentBoundEvent_24_OnPressed__DelegateSignature",
        "BndEvt__WB_GunCustomization_CategoryBtn_Submachines_K2Node_ComponentBoundEvent_25_OnPressed__DelegateSignature",
        "BndEvt__WB_GunCustomization_CategoryBtn_Snipers_K2Node_ComponentBoundEvent_28_OnPressed__DelegateSignature",
        "BndEvt__WB_GunCustomization_CategoryBtn_Shotguns_K2Node_ComponentBoundEvent_27_OnPressed__DelegateSignature",
        "BndEvt__WB_GunCustomization_CategoryBtn_Pistols_K2Node_ComponentBoundEvent_26_OnPressed__DelegateSignature",
    };
    if (category >= std::size(buttons)) { Error("no button for category {}", category); return; }
    Params b(s_widget, buttons[category]);
    b.Call();
}

static bool Select(Object asset) {
    s_before = s_station.GetObj("CurrentReceiver").ptr;
    s_target = asset;
    s_fallbacks = 0;
    Object item;
    std::string seen;
    for (auto& it : FindAllOf("WB_CustomizationItemGun_C")) {
        seen += TextOf(it.GetObj("WeaponItem"), "DisplayName") + ", ";
        if (it.GetObj("WeaponItem").ptr == asset.ptr) item = it;
    }
    Log("menu items: {}", seen);
    if (!item) { Error("no menu item for '{}'", TextOf(asset, "DisplayName")); return false; }
    Params press(item, kItemPress);
    press.Call();
    Log("pressed menu item {} for '{}'", item.Name(), TextOf(asset, "DisplayName"));
    return true;
}

// Waits for the station's new receiver (confirming, then the station's own SpawnWeapon if the request was gated:
// no headset at the station). Returns it once there.
static Object Spawned() {
    Object cur = s_station.GetObj("CurrentReceiver");
    if (cur && cur.ptr != s_before) return cur;
    if (s_fallbacks == 0) {
        Log("no new receiver yet - confirming");
        Params c(s_widget, kConfirm);
        c.Call();
    } else if (s_fallbacks < 3) {
        Log("FALLBACK: calling the station's SpawnWeapon");
        Params sp(s_station, "SpawnWeapon");
        sp.SetObj("GunAsset", s_target).Call();
    } else {
        Error("spawn did not happen");
        s_step = 99;
    }
    s_fallbacks++;
    s_wait = 90;
    return {};
}

static void SetHandle(float progress) {
    Object h = s_gun.GetObj("ChargingHandle");
    if (!h) return;
    if (h.GetBool("bIsLocked")) {
        Params u(h, "UnlockHandle");
        u.Call();
    }
    Params p(h, "SetProgress");
    p.Set("Progress", progress).Call();
    s_gun.Set("CurrentBoltProgress", progress);
}
static void LogParts(const char* when) {
    static const char* names[] = {"body", "muzzle", "sights", "carrier", "charging handle", "mag release", "trigger", "selector"};
    auto parts = Components(s_gun, "DynamicMeshComponent");
    Object h = s_gun.GetObj("ChargingHandle");
    Vec box = h ? ToActor(s_gun, CompLocation(h)) : Vec{};
    Log("ACTION [{}] FireMode {} CurrentTrigger {:.2f} release {} bolt progress {:.2f}; handle progress {:.2f} box at ({:.2f},{:.2f},{:.2f})", when,
        s_gun.Get<uint8_t>("FireMode"), s_gun.Get<float>("CurrentTrigger"), s_gun.GetBool("bIsMagReleasePressed"), s_gun.Get<float>("CurrentBoltProgress"),
        h ? h.Get<float>("CurrentProgress") : -1.f, box.x, box.y, box.z);
    for (size_t i = 3; i < parts.size() && i < std::size(names); i++) {
        Vec l = parts[i].Get<Vec>("RelativeLocation");
        Rot r = parts[i].Get<Rot>("RelativeRotation");
        Log("ACTION   {:<12} location ({:7.3f},{:7.3f},{:7.3f}) rotation pitch {:7.2f} yaw {:6.2f} roll {:7.2f}", names[i], l.x, l.y, l.z, r.pitch,
            r.yaw, r.roll);
    }
}
static void LogRoundDefaults(const char* when) {
    GUClass* cls = s_gun ? s_gun.Get<GUClass*>("BulletData") : nullptr;
    if (!cls) return;
    Object d = DefaultObject(cls);
    Log("ROUND DEFAULTS [{}] {} Mass {:.4f} Diameter {:.3f} velocity {:.0f}-{:.0f} penetration {:.2f}-{:.2f} pooling {} DamageCurve {}", when,
        Object((GUObject*)cls).Name(), d.Get<float>("Mass"), d.Get<float>("Diameter"), d.Get<float>("MuzzleVelocityMin"), d.Get<float>("MuzzleVelocityMax"),
        d.Get<float>("MinPenetration"), d.Get<float>("MaxPenetration"), d.GetBool("EnablePooling"), d.GetObj("DamageCurve").Name());
}

#include "steps.inc"

// UAR-15 - GML plugin (com.remarrow.uar15). Personal use: the model and textures come from the BONELAB
// Cold Weapon Pack (Bonewar), exported by the user, and must not be distributed.
//
// Adds a "UAR-15" to the spawner's assault rifle list, built on the game's M4A1 Block II (the same AR-15 action,
// 5.56 round and STANAG magazine) WITHOUT replacing any game asset: every other M4A1 stays an M4A1.
//
// 1. Spawner entry: after BP_GameInstance::GetItems builds AvailableGuns, a GunAsset_C constructed at runtime
//    (the M4's receiver, an icon rendered from the UAR-15 model) is appended to the M4's category.
// 2. Visuals: an M4 receiver spawned from that entry (or rebuilt from a saved loadout slot holding it) gets the
//    UAR-15 as DynamicMeshComponents built from the OBJs embedded in this DLL, textured with M_DefaultShader
//    instances; the M4 mesh and moving parts are hidden. The UAR-15 is fitted on the M4 by its receivers
//    (tools\prep: 2.9 mm rms), so the grips, magazine well, chamber and charging handle stay the M4's. Its
//    muzzle point moves out to the UAR-15's 20" barrel and the stock butt onto its stock.
// 3. Its own moving parts (bolt carrier, charging handle, trigger, selector, magazine release) follow the M4's
//    matching bones every frame, turned about the UAR-15's own pivots.
//
// Magazines and rounds are the M4's own (30-round PMAG, 5.56).
//
// Settings: GML\config\com.remarrow.uar15.cfg. Build: build.bat [install].
#include <GML/GML.hpp>
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include "payload.h"
#include "payload_key.h"  // generated per build by tools\pack (build\gen), never committed
#include "version.h"

using namespace gml;

static const char* kDonor = "/Game/Weapons/Data/DA_M4A1_BlockII.DA_M4A1_BlockII";
static const char* kDisplayName = "UAR-15";

static GUObject* s_gun = nullptr;  // our runtime GunAsset

// ------------------------------------------------------------------ assets

static bool Unpack(const char* name, std::vector<uint8_t>& out, bool optional = false) {
    static Blob blob = Resource("DATA");
    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = kPayloadKeyA[i] ^ kPayloadKeyB[i];
    bool ok = blob && payload::Extract(blob.data, blob.size, key, name, out);
    SecureZeroMemory(key, sizeof key);
    if (!ok && !optional) Error("embedded asset '{}' is missing or damaged", name);
    return ok;
}

static GUObject* Icon() {
    static GUObject* icon = nullptr;
    if (icon && API->IsValid(icon)) return icon;
    std::vector<uint8_t> buf;
    if (Unpack("Icon", buf, true)) {
        icon = API->ImportTextureFromMemory(buf.data(), buf.size(), "UAR15Icon");
        payload::Wipe(buf);
        if (icon) API->KeepAlive(icon);
    }
    return icon;
}

// ------------------------------------------------------------------ spawner entry

static GML_TArray* GunList(Object gi, uint8_t category) {
    GFProperty* mapProp = API->FindProperty(gi.Struct(), "AvailableGuns");
    void* map = gi.PropPtr("AvailableGuns");
    if (!mapProp || !map) return nullptr;
    struct Ctx { GML_TArray* found; GUStruct* valueStruct; uint8_t category; } ctx{nullptr, nullptr, category};
    GML_PropInfo mi, vi;
    API->GetPropertyInfo(mapProp, &mi);
    API->GetPropertyInfo(mi.value, &vi);
    ctx.valueStruct = vi.structType;  // FGunAssetList
    API->MapForEach(map, mapProp, [](void* u, void* key, void* value) -> int {
        auto* c = (Ctx*)u;
        if (*(uint8_t*)key != c->category) return 0;
        GFProperty* arr = API->FindProperty(c->valueStruct, "GunAssets");  // GUID-mangled in the BP struct
        GML_PropInfo ai;
        if (!arr || !API->GetPropertyInfo(arr, &ai)) return 1;
        c->found = (GML_TArray*)((uint8_t*)value + ai.offset);
        return 1;
    }, &ctx);
    return ctx.found;
}

// The UAR-15 goes in the M4's own category (the game's assault rifle list).
static void AddGun(Object gi) {
    Object donor = FindObject(kDonor);
    GUClass* gunClass = FindClass("GunAsset_C");
    if (!donor || !gunClass) { Error("donor {} or GunAsset_C missing", kDonor); return; }
    uint8_t category = donor.Get<uint8_t>("Category");
    GML_TArray* raw = GunList(gi, category);
    if (!raw) { Warn("AvailableGuns[{}] not found yet", category); return; }
    TArrayView<GUObject*> list{raw};
    if (s_gun && Object(s_gun) && list.Contains(s_gun)) return;  // GetItems ran again; already there

    Object gun = NewObject(gunClass, gi);
    if (!gun) { Error("could not construct GunAsset_C"); return; }
    gun.Set("ReceiverClass", donor.Get<GUClass*>("ReceiverClass"));
    GUObject* icon = Icon();
    gun.Set("Icon", icon ? icon : donor.Get<GUObject*>("Icon"));
    gun.SetBool("bEnabled", donor.GetBool("bEnabled"));
    gun.Set("Category", category);
    uint8_t text[16];
    if (API->MakeText(kDisplayName, text)) std::memcpy(gun.PropPtr("DisplayName"), text, 16);

    int before = list.Num();
    if (list.Add(gun.ptr)) {
        s_gun = gun.ptr;
        Log("*** ADDED '{}' to the spawner (category {}, {} -> {}) as {}{} ***", kDisplayName, category, before, list.Num(), gun.FullName(),
            icon ? "" : " (M4 icon: the UAR-15's could not be built)");
    } else {
        Error("append failed");
    }
}

// ------------------------------------------------------------------ visuals

static const char* kShader = "/Game/Art/Common/M_DefaultShader.M_DefaultShader";
// Texture sets / material slots = OBJ `usemtl` groups (tools\prep `parts`, same order).
static const char* kParts[] = {"Barrel", "Gas", "Bolt", "Charging", "Handguard", "Grip", "Tube",
                               "Lower", "Upper", "Stock", "Muzzle", "MbusRear", "SightRear", "SightFront"};
static constexpr int kNumParts = (int)std::size(kParts);

// The UAR-15's own moving parts: each follows one of the M4's movables bones.
enum class Motion { Slide, Turn };  // the bone's translation, or its rotation about the UAR-15's pivot
struct Vec3 { double x, y, z; };
// flip: the part's rest pose is turned 180 degrees about the gun's Y axis (through its pivot) before it follows the bone.
struct MovingPart { const char* asset; const char* bone; Motion motion; Vec3 pivot; GUObject* mesh; bool flip = false; };
// pivots: actor space, cm (tools\prep, fitted: the TP and SP nodes)
static MovingPart s_movingParts[] = {
    {"uar_bolt", "RootBoltCarrierGroup", Motion::Slide, {}, nullptr},
    {"uar_charging", "RootChargingHandle", Motion::Slide, {}, nullptr},
    {"uar_magrelease", "RootMagRelease", Motion::Slide, {}, nullptr},
    {"uar_trigger", "RootTrigger", Motion::Turn, {-1.347, 0.005, -1.892}, nullptr},
    {"uar_selector", "RootSelector", Motion::Turn, {-4.191, -1.292, -1.340}, nullptr, true},  // the M4's lever rests pointing back, the UAR-15's forward
};
static constexpr int kNumMoving = (int)std::size(s_movingParts);

static bool s_ownMovingParts = true, s_fitAttachments = true;
// The receiver fit (toolsprep) matches the UAR-15's upper to the M4 mesh, but the M4's interaction points (magazine
// well, charging handle, pistol grip hand socket, selector pivot) all sit 1.0-1.1 cm behind the UAR-15's matching
// parts: its magwell walls, charging handle latch and selector pivot measure +1.0, +1.1 and +1.09 cm. So the whole
// model moves back along the bore by this much; the hands and the magazine then land on the UAR-15's own parts.
static constexpr float kFitShiftX = -1.0f;
// The whole model is drawn 0.1% larger than the export (scaled about the gun's origin, before the shift): the
// player's call, after holding it next to the game's own AR-15s.
static constexpr float kFitScale = 1.001f;
static float s_offset[3] = {0, 0, 0};  // cm: kFitShiftX plus the player's nudge (config), on top of the OBJs' fit
static GUObject* s_mesh = nullptr;        // runtime UDynamicMesh: receiver, barrel, handguard, stock, grip
static GUObject* s_muzzleMesh = nullptr;  // the A2 flash hider (hidden while a muzzle device is mounted)
static GUObject* s_sightsMesh = nullptr;  // the MBUS sights (hidden while an optic is on the top rail)
static GUObject* s_mids[kNumParts] = {};  // one M_DefaultShader instance per texture set
static bool s_assetsTried = false;
static GUClass* s_receiverClass = nullptr;

struct Pending { GUObject* actor; ULONGLONG since; };
static std::vector<Pending> s_pending;

static bool BuildVisualAssets() {
    if (s_assetsTried) return s_mesh != nullptr;
    s_assetsTried = true;
    if (API->size < offsetof(GML_API, ImportDynamicMeshFromMemory) + sizeof(void*)) {
        Error("this plugin needs GML 2.2 or newer (assets embedded in the DLL)");
        return false;
    }
    Object shader = LoadObject(kShader);
    if (!shader) { Error("weapon shader {} not found", kShader); return false; }

    // Textures made at runtime (ImportBufferAsTexture2D) are always sRGB. A normal map read through sRGB decoding
    // tilts every normal ~50 degrees, so the model (which has flat normals anyway) takes the engine's own flat
    // normal map instead; tools\prep stores the ORM channels sRGB-encoded so they decode to the intended values.
    Object flatNormal = LoadObject("/Engine/EngineMaterials/FlatNormal.FlatNormal");
    if (!flatNormal) flatNormal = LoadObject("/Engine/EngineMaterials/DefaultNormal.DefaultNormal");
    if (!flatNormal) Warn("no engine flat normal map - normals keep the shader default");
    std::vector<uint8_t> buf;
    for (int i = 0; i < kNumParts; i++) {
        s_mids[i] = API->CreateMaterialInstance(shader.ptr);
        if (!s_mids[i]) { Error("could not create a material instance for {}", kParts[i]); return false; }
        if (flatNormal) API->SetMaterialTexture(s_mids[i], "Normal", flatNormal.ptr);
        for (const char* param : {"Diffuse", "ORM"}) {
            std::string res = std::string(kParts[i]) + "_" + param;
            GUObject* tex = Unpack(res.c_str(), buf) ? API->ImportTextureFromMemory(buf.data(), buf.size(), res.c_str()) : nullptr;
            payload::Wipe(buf);
            if (!tex || !API->SetMaterialTexture(s_mids[i], param, tex))
                Warn("{}: texture {} missing - that channel keeps the shader default", kParts[i], param);
        }
    }

    GML_MeshImport o{};
    o.size = sizeof o;
    o.axis = GML_AXIS_UNREAL;  // tools\prep writes the game's space, cm, fitted
    o.scale = kFitScale;
    std::memcpy(o.offset, s_offset, sizeof s_offset);
    o.flipWinding = -1;
    o.materialCount = kNumParts;
    o.materialNames = kParts;
    o.materials = s_mids;
    auto load = [&](const char* name, bool optional = false) -> GUObject* {
        if (!Unpack(name, buf, optional)) return nullptr;
        GUObject* m = API->ImportDynamicMeshFromMemory(buf.data(), buf.size(), name, &o);
        payload::Wipe(buf);
        if (!m) Error("could not build the UAR-15 mesh '{}'", name);
        return m;
    };
    s_mesh = load("uar");
    s_muzzleMesh = load("uar_muzzle");
    s_sightsMesh = load("uar_sights");
    for (auto& p : s_movingParts) p.mesh = load(p.asset, true);
    if (s_mesh) Log("UAR-15 visuals ready: {}", Object(s_mesh).FullName());
    return s_mesh != nullptr;
}

// ------------------------------------------------------------------ transforms (raw FTransform bytes)

using Xf = std::vector<uint8_t>;
static size_t XfSize() {
    static size_t size = 0;
    if (!size) {
        GML_PropInfo ri;
        API->GetPropertyInfo(API->FindProperty((GUStruct*)API->FindFunction(Lib("KismetMathLibrary").Struct(), "MakeTransform"), "ReturnValue"), &ri);
        size = ri.size;
    }
    return size;
}
static Xf TakeXf(Params& p, const char* name = "ReturnValue") {
    auto* s = (uint8_t*)p.Ptr(name);
    return Xf(s, s + XfSize());
}
static void PutXf(Params& p, const char* name, const Xf& x) { std::memcpy(p.Ptr(name), x.data(), x.size()); }
static Xf XfCall(const char* fn, std::initializer_list<std::pair<const char*, const Xf*>> args) {
    Params p(Lib("KismetMathLibrary"), fn);
    for (auto& [n, x] : args) PutXf(p, n, *x);
    p.Call();
    return TakeXf(p);
}
static Xf Compose(const Xf& a, const Xf& b) { return XfCall("ComposeTransforms", {{"A", &a}, {"B", &b}}); }  // a then b

// A bone's reference-pose transform in component space: its local ref transforms composed up the parent chain.
static Xf RefPoseComponent(Object skel, const char* bone) {
    Xf acc;
    uint64_t name = API->MakeName(bone);
    for (int guard = 0; guard < 64 && name; guard++) {
        Params bi(skel, "GetBoneIndex");
        *(uint64_t*)bi.Ptr("BoneName") = name;
        bi.Call();
        int idx = bi.Return<int32_t>();
        if (idx < 0) break;
        Params rp(skel, "GetRefPoseTransform");
        rp.Set("BoneIndex", (int32_t)idx).Call();
        Xf local = TakeXf(rp);
        acc = acc.empty() ? local : Compose(acc, local);  // child first, then its parent
        Params pb(skel, "GetParentBone");
        *(uint64_t*)pb.Ptr("BoneName") = name;
        pb.Call();
        name = pb.Return<uint64_t>();
    }
    return acc;
}

static void HideComponent(Object c) {
    Params v(c, "SetVisibility");
    v.Set("bNewVisibility", false).Set("bPropagateToChildren", false).Call();  // attached children stay visible
}

// The M4 receiver's points moved onto the UAR-15's (actor space, cm; the M4's from its blueprint, the UAR-15's
// from tools\prep's fit). The receivers coincide, so only the muzzle (a 20" barrel against the M4's 14.5") and
// the stock butt move.
struct AttachPoint { const char* name; double x, y, z; Vec3 m4; };
static const AttachPoint kAttachPoints[] = {
    {"Fire Location", 50.46, 0, 2.041, {34.364, 0, 2.086}},                    // shot origin, muzzle-device mount: the flash hider's rear
    {"DefaultStockButtPosition", -30.83, 0, -7.07, {-36.231, 0, -7.070}},      // behind the bottom of the butt pad, as the M4's
};

// A point of the model (the OBJs' space) where it is drawn: scaled about the origin, then shifted.
static Vec3 Fitted(double x, double y, double z) { return {x * kFitScale + s_offset[0], y * kFitScale + s_offset[1], z * kFitScale + s_offset[2]}; }
static Vec3 AttachTarget(const AttachPoint& p) { return Fitted(p.x, p.y, p.z); }

struct ActorFrame {
    Params gt;
    size_t size;
    explicit ActorFrame(Object actor) : gt(actor, "GetTransform") {
        gt.Call();
        GML_PropInfo xf;
        API->GetPropertyInfo(API->FindProperty((GUStruct*)API->FindFunction(actor.Struct(), "GetTransform"), "ReturnValue"), &xf);
        size = xf.size;
    }
    Vec3 Call(const char* fn, const char* arg, Vec3 v) {
        Params p(Lib("KismetMathLibrary"), fn);
        std::memcpy(p.Ptr("T"), gt.Ptr("ReturnValue"), size);
        p.Set(arg, v).Call();
        return p.Return<Vec3>();
    }
    Vec3 ToWorld(Vec3 local) { return Call("TransformLocation", "Location", local); }
    Vec3 ToLocal(Vec3 world) { return Call("InverseTransformLocation", "Location", world); }
    Vec3 DirToWorld(Vec3 local) { return Call("TransformDirection", "Direction", local); }
};

static const AttachPoint* FindAttachPoint(const std::string& componentName) {
    for (auto& p : kAttachPoints)
        if (componentName == p.name) return &p;
    return nullptr;
}

// Attachments already on the gun (a loadout restores them) move with the point they hang on.
static int SnapMountedAttachments(Object gun, ActorFrame& frame) {
    Params ga(gun, "GetAttachedActors");
    ga.Set("bResetArray", true).Set("bRecursivelyIncludeAttachedActors", false).Call();
    auto* arr = (GML_TArray*)ga.Ptr("OutActors");
    int snapped = 0;
    for (int i = 0; arr && i < arr->Num; i++) {
        Object att(((GUObject**)arr->Data)[i]);
        if (!att.IsA("GunAttachment")) continue;
        Object point = att.GetObj("AttachablePoint"), parent = att.GetObj("RootComponent").GetObj("AttachParent");
        const AttachPoint* p = point ? FindAttachPoint(point.Name()) : nullptr;
        if (!p && parent) p = FindAttachPoint(parent.Name());
        if (!p || (parent && FindAttachPoint(parent.Name()) == p)) continue;
        Vec3 to = AttachTarget(*p);
        Vec3 d = frame.DirToWorld({to.x - p->m4.x, to.y - p->m4.y, to.z - p->m4.z});
        Params mv(att, "K2_AddActorWorldOffset");
        mv.Set("DeltaLocation", d).Set("bSweep", false).Set("bTeleport", true).Call();
        snapped++;
    }
    return snapped;
}

static void FitAttachPoints(Object actor) {
    ActorFrame frame(actor);
    Params gc(actor, "K2_GetComponentsByClass");
    gc.Set("ComponentClass", FindClass("SceneComponent")).Call();
    auto* arr = (GML_TArray*)gc.Ptr("ReturnValue");
    int moved = 0;
    for (int i = 0; arr && i < arr->Num; i++) {
        Object c(((GUObject**)arr->Data)[i]);
        if (const AttachPoint* p = FindAttachPoint(c.Name())) {
            Params sw(c, "K2_SetWorldLocation");
            sw.Set("NewLocation", frame.ToWorld(AttachTarget(*p))).Set("bSweep", false).Set("bTeleport", true).Call();
            moved++;
        }
    }
    if (moved != (int)std::size(kAttachPoints))
        Warn("{}: moved {} of {} attach points - receiver layout differs from the M4's", actor.Name(), moved, std::size(kAttachPoints));
    if (int n = SnapMountedAttachments(actor, frame)) Log("{}: {} mounted attachment(s) moved onto the UAR-15", actor.Name(), n);
}

// An optic on the top rail: the MBUS sights would stand in its view, so they are hidden. An attachment counts if
// it hangs on a top-rail spline, or sits on top of the rail (above Z 5.5 in the gun's space).
static bool OpticMounted(Object gun) {
    Params ga(gun, "GetAttachedActors");
    ga.Set("bResetArray", true).Set("bRecursivelyIncludeAttachedActors", false).Call();
    auto* arr = (GML_TArray*)ga.Ptr("OutActors");
    ActorFrame frame(gun);
    for (int i = 0; arr && i < arr->Num; i++) {
        Object att(((GUObject**)arr->Data)[i]);
        if (!att.IsA("GunAttachment") || !att.GetBool("bAttached")) continue;
        for (Object c : {att.GetObj("AttachablePoint"), att.GetObj("RootComponent").GetObj("AttachParent")}) {
            std::string n = c ? c.Name() : "";
            if (n == "ReceiverAttachmentSpline" || n == "ReceiverAttachmentSplineHG12") return true;
        }
        Params al(att, "K2_GetActorLocation");
        al.Call();
        Vec3 at = frame.ToLocal(al.Return<Vec3>());
        if (at.z > 5.5 + s_offset[2] && at.x > -12 && at.x < 45) return true;
    }
    return false;
}

// ------------------------------------------------------------------ the UAR-15's own action

struct Q { double x, y, z, w; };
static Vec3 Rotate(const Q& q, Vec3 v) {  // FQuat::RotateVector
    Vec3 t{2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x)};
    return {v.x + q.w * t.x + (q.y * t.z - q.z * t.y), v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
            v.z + q.w * t.z + (q.x * t.y - q.y * t.x)};
}
static Q Mul(const Q& a, const Q& b) {  // a * b: b, then a
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
static Q Conj(const Q& q) { return {-q.x, -q.y, -q.z, q.w}; }

// An FTransform as the engine lays it out (doubles): rotation quaternion, translation, scale.
struct XfD { double q[4]; double t[3]; double pad0; double s[3]; double pad1; };
static_assert(sizeof(XfD) == 0x60);

static bool XfLayoutOk() {
    static int ok = -1;
    if (ok < 0) {
        Params mk(Lib("KismetMathLibrary"), "MakeTransform");
        struct { double pitch, yaw, roll; } yaw90{0, 90, 0};
        mk.Set("Location", Vec3{1, 2, 3}).Set("Rotation", yaw90).Set("Scale", Vec3{1, 1, 1}).Call();
        XfD x{};
        ok = XfSize() == sizeof x;
        if (ok) {
            std::memcpy(&x, mk.Ptr("ReturnValue"), sizeof x);
            ok = x.t[0] == 1 && x.t[1] == 2 && x.t[2] == 3 && x.s[0] == 1 && std::fabs(std::fabs(x.q[2]) - 0.7071068) < 1e-5 &&
                 std::fabs(x.q[3]) > 0.7;
        }
        if (!ok) Warn("FTransform layout is not the expected one - the UAR-15's moving parts stay still");
    }
    return ok == 1;
}

// x -> q (x - pivot) + pivot + move, as the part's relative transform on the gun's root (actor space).
static void PosePart(GUObject* part, Q q, Vec3 pivot, Vec3 move) {
    Vec3 r = Rotate(q, pivot);
    XfD x{{q.x, q.y, q.z, q.w}, {pivot.x - r.x + move.x, pivot.y - r.y + move.y, pivot.z - r.z + move.z}, 0, {1, 1, 1}, 0};
    Params p(Object(part), "K2_SetRelativeTransform");
    std::memcpy(p.Ptr("NewTransform"), &x, sizeof x);
    p.Set("bSweep", false).Set("bTeleport", true).Call();
}

static Vec3 Nudged(Vec3 p) { return Fitted(p.x, p.y, p.z); }

struct Rig {
    GUObject* gun;
    GUObject* mov;
    GUObject* part[kNumMoving];
    uint64_t bone[kNumMoving];
    XfD rest[kNumMoving];
    XfD applied[kNumMoving];
};
static std::vector<Rig> s_rigs;

static void PoseRig(Rig& r) {
    for (int i = 0; i < kNumMoving; i++) {
        if (!r.part[i] || !r.bone[i]) continue;
        Params bt(Object(r.mov), "GetBoneTransformByName");
        *(uint64_t*)bt.Ptr("BoneName") = r.bone[i];
        bt.Set("BoneSpace", (uint8_t)1).Call();  // component space
        XfD now{};
        std::memcpy(&now, bt.Ptr("ReturnValue"), sizeof now);
        if (!std::memcmp(&now, &r.applied[i], offsetof(XfD, pad0))) continue;  // unchanged since last frame
        r.applied[i] = now;
        const XfD& rest = r.rest[i];
        const MovingPart& mp = s_movingParts[i];
        if (mp.motion == Motion::Slide) {
            PosePart(r.part[i], {0, 0, 0, 1}, {0, 0, 0}, {now.t[0] - rest.t[0], now.t[1] - rest.t[1], now.t[2] - rest.t[2]});
        } else {
            Q d = Mul(Q{now.q[0], now.q[1], now.q[2], now.q[3]}, Conj(Q{rest.q[0], rest.q[1], rest.q[2], rest.q[3]}));
            if (mp.flip) d = Mul(d, Q{0, 1, 0, 0});  // 180 degrees about Y first
            PosePart(r.part[i], d, Nudged(mp.pivot), {0, 0, 0});
        }
    }
}

static void PoseRigs() {  // every frame
    for (size_t i = 0; i < s_rigs.size();) {
        Rig& r = s_rigs[i];
        if (!API->IsValid(r.gun) || !API->IsValid(r.mov)) { s_rigs.erase(s_rigs.begin() + i); continue; }
        PoseRig(r);
        i++;
    }
}

static void HangMovingParts(Object actor) {
    Object mov = actor.GetObj("Movables");  // M4 moving parts: a skeletal mesh the gun code poses by bone name
    if (!mov) { Warn("{} has no Movables component - keeping the M4's moving parts", actor.Name()); return; }
    if (!XfLayoutOk()) return;
    Rig r{actor.ptr, mov.ptr, {}, {}, {}, {}};
    int hung = 0;
    for (int i = 0; i < kNumMoving; i++) {
        Xf rest = RefPoseComponent(mov, s_movingParts[i].bone);
        if (rest.empty()) { Warn("{}: no bone {} on the M4's movables - the UAR-15's {} stays put", actor.Name(), s_movingParts[i].bone, s_movingParts[i].asset); }
        else {
            std::memcpy(&r.rest[i], rest.data(), sizeof(XfD));
            r.bone[i] = API->MakeName(s_movingParts[i].bone);
        }
        r.part[i] = s_movingParts[i].mesh ? API->AddDynamicMeshComponent(actor.ptr, s_movingParts[i].mesh, s_mids, kNumParts) : nullptr;
        r.applied[i].t[0] = 1e9;  // posed on the first frame
        hung += r.part[i] != nullptr;
    }
    s_rigs.push_back(r);
    PoseRig(s_rigs.back());
    // Stop drawing the M4's parts but keep the bones posed: they are read every frame.
    mov.Set("VisibilityBasedAnimTickOption", (uint8_t)0);  // AlwaysTickPoseAndRefreshBones
    HideComponent(mov);
    Log("{}: {} UAR-15 moving parts, following the M4's bones", actor.Name(), hung);
}

// ------------------------------------------------------------------ dressing

// Only this plugin gives an M4 receiver DynamicMeshComponents (the UAR-15's meshes).
static bool IsDressed(Object actor) {
    Params gc(actor, "K2_GetComponentsByClass");
    gc.Set("ComponentClass", FindClass("DynamicMeshComponent")).Call();
    auto* arr = (GML_TArray*)gc.Ptr("ReturnValue");
    return arr && arr->Num > 0;
}

struct DressedGun { GUObject* gun; GUObject* muzzle; GUObject* sights; };
static std::vector<DressedGun> s_dressedGuns;

static void Mark(Object receiver);

// ------------------------------------------------------------------ hand poses
//
// The game's grip and handguard hand poses are authored for the M4's A2 pistol grip and quad rail. The UAR-15's BCM
// grip stands 11 degrees more upright, and its keymod handguard is 2 cm narrower and 1 cm deeper, so the fingers
// floated. Measured on the meshes (the M4's grip registered onto the UAR-15's: 80th-percentile gap 1.29 -> 0.39 cm):
//  - grip: the hand socket turns 10.96 degrees nose-up about itself and moves 0.20 cm forward, 0.05 cm down;
//  - handguard: the hand (its placement relative to the socket, which the game mirrors for the other hand) moves
//    1.03 cm in, onto the narrower side, and 0.35 cm up for the taller top; the fingers curl 12 degrees more at their
//    middle and tip joints to reach the far side.
// Each is a setting, to tune in a headset.
static float s_gripPitch = 10.96f, s_gripX = 0.20f, s_gripZ = -0.05f;
static float s_guardIn = 1.03f, s_guardUp = 0.35f, s_guardCurl = 12.0f;

struct PoseRaw { uint64_t bone, pad; double q[4]; uint8_t rest[0x10]; };  // FBPVRHandPoseBonePair
static_assert(sizeof(PoseRaw) == 0x40);

static Object FindSocket(Object actor, const char* prefix) {
    Params gc(actor, "K2_GetComponentsByClass");
    gc.Set("ComponentClass", FindClass("SceneComponent")).Call();
    auto* arr = (GML_TArray*)gc.Ptr("ReturnValue");
    for (int i = 0; arr && i < arr->Num; i++) {
        Object c(((GUObject**)arr->Data)[i]);
        if (c.Name().rfind(prefix, 0) == 0) return c;
    }
    return {};
}

static void FitHandPoses(Object actor) {
    int done = 0;
    if (Object grip = FindSocket(actor, "PrimaryHandSocket")) {
        Vec3 l = grip.Get<Vec3>("RelativeLocation");
        struct { double pitch, yaw, roll; } r = grip.Get<decltype(r)>("RelativeRotation");
        Params s(grip, "K2_SetRelativeLocationAndRotation");
        s.Set("NewLocation", Vec3{l.x + s_gripX, l.y, l.z + s_gripZ}).Set("NewRotation", decltype(r){r.pitch + s_gripPitch, r.yaw, r.roll})
            .Set("bSweep", false).Set("bTeleport", true).Call();
        done++;
    }
    if (Object guard = FindSocket(actor, "SecondaryHandSocket")) {
        if (auto* hrp = (double*)guard.PropPtr("HandRelativePlacement")) {
            hrp[5] += s_guardIn;  // translation y: the hand sits on the -Y (left) side, so + is in toward the handguard
            hrp[6] += s_guardUp;
        }
        GFProperty* dp = API->FindProperty(guard.Struct(), "CustomPoseDeltas");
        GML_PropInfo ai{}, ei{};
        auto* d = (GML_TArray*)guard.PropPtr("CustomPoseDeltas");
        if (d && dp && API->GetPropertyInfo(dp, &ai) && ai.inner && API->GetPropertyInfo(ai.inner, &ei) && ei.size == (int)sizeof(PoseRaw)) {
            double h = -s_guardCurl * 3.14159265358979 / 360;  // curl = about the bone's -Z
            Q c{0, 0, std::sin(h), std::cos(h)};
            for (int i = 0; i < d->Num; i++) {
                auto& e = ((PoseRaw*)d->Data)[i];
                char b[64] = "";
                API->NameToString(e.bone, b, sizeof b);
                std::string n = b;
                if (n.rfind("thumb", 0) == 0 || (n.find("_02_") == std::string::npos && n.find("_03_") == std::string::npos)) continue;
                Q q = Mul(Q{e.q[0], e.q[1], e.q[2], e.q[3]}, c);
                e.q[0] = q.x; e.q[1] = q.y; e.q[2] = q.z; e.q[3] = q.w;
            }
        }
        done++;
    }
    if (done < 2) Warn("{}: found {} of 2 hand sockets - the hand poses stay the M4's", actor.Name(), done);
}

static void DressAsUar(Object actor) {
    if (IsDressed(actor)) return;
    if (!BuildVisualAssets()) return;
    Object root = actor.GetObj("StaticMeshComponent");  // the M4 mesh; also collision + physics
    if (!root) { Error("{} has no StaticMeshComponent", actor.FullName()); return; }
    Object comp = API->AddDynamicMeshComponent(actor.ptr, s_mesh, s_mids, kNumParts);
    if (!comp) { Error("could not add the UAR-15 mesh to {}", actor.FullName()); return; }
    Object muzzle = s_muzzleMesh ? Object(API->AddDynamicMeshComponent(actor.ptr, s_muzzleMesh, s_mids, kNumParts)) : Object();
    Object sights = s_sightsMesh ? Object(API->AddDynamicMeshComponent(actor.ptr, s_sightsMesh, s_mids, kNumParts)) : Object();
    HideComponent(root);
    if (s_ownMovingParts) HangMovingParts(actor);
    if (s_fitAttachments) FitAttachPoints(actor);
    FitHandPoses(actor);
    s_dressedGuns.push_back({actor.ptr, muzzle.ptr, sights.ptr});
    Log("*** {} dressed as a UAR-15 ***", actor.Name());
}

static GUObject* AttachRoot(Object a) {
    for (int guard = 0; guard < 16 && a; guard++) {
        Params p(a, "GetAttachParentActor");
        p.Call();
        Object up = p.Return<GUObject*>();
        if (!up) break;
        a = up;
    }
    return a.ptr;
}

// ------------------------------------------------------------------ multiplayer
//
// Each machine dresses guns itself, and only the one whose player picked the UAR-15 (or loaded it from a save
// slot) knows the M4 it got is one. It tells the others through the receiver's own ColorComponent: its
// ColorOption is an FName the game replicates to every player and keeps in a saved loadout. A machine without the
// plugin ignores the unknown colour and shows an M4.

static constexpr char kMarker[] = "UAR15";
static const char* kSelectHook =
    "WB_GunCustomization_C:BndEvt__WB_GunCustomization_WB_Database_K2Node_ComponentBoundEvent_34_OnItemSelected__DelegateSignature";
static std::map<GUObject*, GUObject*> s_picks;  // gun station -> gun last picked at it on this machine

static bool HasAuthority(Object actor) {
    Params p(actor, "HasAuthority");
    p.Call();
    return p.Return<bool>();
}

static std::string ColorOf(Object receiver) {
    Object cc = receiver ? receiver.GetObj("ColorComponent") : Object();
    char b[128] = "";
    if (cc) API->NameToString(cc.Get<uint64_t>("ColorOption"), b, sizeof b);
    return b;
}

static bool IsMarked(Object receiver) {
    return receiver.Class() == s_receiverClass && _stricmp(ColorOf(receiver).c_str(), kMarker) == 0;
}

static bool LocallyOwned(Object actor) {
    Object o = actor;
    for (int guard = 0; guard < 4 && o; guard++) {
        if (o.IsA("Pawn")) {
            Params lc(o, "IsLocallyControlled");
            lc.Call();
            return lc.Return<bool>();
        }
        if (o.IsA("PlayerController")) {
            Params lc(o, "IsLocalController");
            lc.Call();
            return lc.Return<bool>();
        }
        Params go(o, "GetOwner");
        go.Call();
        o = go.Return<GUObject*>();
    }
    return false;
}

static void Mark(Object receiver) {
    static std::map<GUObject*, ULONGLONG> asked;
    Object cc = receiver.GetObj("ColorComponent");
    if (!cc || IsMarked(receiver)) return;
    bool authority = HasAuthority(receiver);
    if (!authority && !LocallyOwned(receiver)) return;
    ULONGLONG now = GetTickCount64();
    if (auto it = asked.find(receiver.ptr); it != asked.end() && now - it->second < 1000) return;
    asked[receiver.ptr] = now;
    Params ac(cc, "ApplyColor_Server");
    ac.Set("ColorName", API->MakeName(kMarker)).Call();
    Log("{}: named a UAR-15 for every player{}", receiver.Name(), authority ? "" : " (asked the server)");
}

// Every quarter second: each UAR-15's flash hider (hidden while a muzzle device replaces it) and its sights
// (hidden under an optic).
static void WatchDressedGuns() {
    static ULONGLONG next = 0;
    if (s_dressedGuns.empty() || GetTickCount64() < next) return;
    next = GetTickCount64() + 250;
    for (size_t i = 0; i < s_dressedGuns.size();) {
        Object gun(s_dressedGuns[i].gun);
        if (!gun || !API->IsValid(gun.ptr) || !IsDressed(gun)) { s_dressedGuns.erase(s_dressedGuns.begin() + i); continue; }
        Mark(gun);
        if (Object muzzle = s_dressedGuns[i].muzzle; muzzle && API->IsValid(muzzle.ptr)) {
            bool device = false;
            Params ga(gun, "GetAttachedActors");
            ga.Set("bResetArray", true).Set("bRecursivelyIncludeAttachedActors", false).Call();
            auto* arr = (GML_TArray*)ga.Ptr("OutActors");
            for (int k = 0; arr && k < arr->Num && !device; k++) {
                Object att(((GUObject**)arr->Data)[k]);
                char tag[64] = "";
                if (att.IsA("GunAttachment")) API->NameToString(att.Get<uint64_t>("AttachPointTag"), tag, sizeof tag);
                device = !strcmp(tag, "MuzzleAttachLocation") && att.GetBool("bAttached");
            }
            if (muzzle.GetBool("bVisible") == device) {
                Params v(muzzle, "SetVisibility");
                v.Set("bNewVisibility", !device).Set("bPropagateToChildren", false).Call();
            }
        }
        if (Object sights = s_dressedGuns[i].sights; sights && API->IsValid(sights.ptr)) {
            bool show = !OpticMounted(gun);
            if (sights.GetBool("bVisible") != show) {
                Params v(sights, "SetVisibility");
                v.Set("bNewVisibility", show).Set("bPropagateToChildren", false).Call();
                Log("{}: iron sights {}", gun.Name(), show ? "up (no optic)" : "hidden under the optic");
            }
        }
        i++;
    }
}
// ------------------------------------------------------------------ loadouts (team room, operations)
//
// The game saves each carried gun as a GunSerializable in SaveGames\SavedGun_<slot>.sav, with its GunAsset
// stored as an object path, and rebuilds it from there at team-room start and in every operation. The UAR-15's
// GunAsset is created at runtime, so its path only resolves in the session that wrote it. So once the game has
// saved the UAR-15, the plugin re-saves that slot (with the game's own SaveGameToSlot) with the M4 donor as
// GunAsset - same receiver and attachments. A loadout gun is the UAR-15 only if its slot was saved with the
// UAR-15's colour name (a stock CQBR is saved under our runtime asset too, so that proves nothing). Without the
// plugin the slot simply loads as an M4.

static std::string FStr8(Object o, const char* prop) {
    char b[256] = "";
    if (auto* s = (const GML_FString*)o.PropPtr(prop)) API->FStringToUtf8(s, b, sizeof b);
    return b;
}

static std::wstring Widen8(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

static std::string ReadFileBytes(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string s(GetFileSize(f, nullptr), '\0');
    DWORD n = 0;
    if (!ReadFile(f, s.data(), (DWORD)s.size(), &n, nullptr)) n = 0;
    CloseHandle(f);
    s.resize(n);
    return s;
}

static std::string HashHex(const std::string& bytes) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : bytes) h = (h ^ c) * 0x100000001b3ull;
    return std::format("{:016x}-{}", h, bytes.size());
}

static std::wstring SaveGamesDir() {
    static std::wstring dir;
    if (dir.empty()) {
        Params p(Lib("KismetSystemLibrary"), "GetProjectSavedDirectory");
        p.Call();
        char b[1024] = "";
        if (auto* s = (const GML_FString*)p.Ptr("ReturnValue")) API->FStringToUtf8(s, b, sizeof b);
        if (*b) dir = Widen8(b) + L"SaveGames\\";
    }
    return dir;
}

// [Loadouts] <slot> = <hash of the file while it holds the UAR-15>, in the plugin's .cfg.
static ConfigEntry<std::string> SlotRecord(const std::string& slot) {
    return Config.Bind<std::string>("Loadouts", slot.c_str(), "", "Written by the plugin: this save slot holds the UAR-15 (hash of its file).");
}
static std::string RecordedHash(const std::string& slot) { return SlotRecord(slot).Value(); }
static void RecordHash(const std::string& slot, const std::string& hash) { SlotRecord(slot).Set(hash); }

// A field of FGunSetupStruct inside a loaded GunSerializable.
static void* SetupField(Object save, const char* name) {
    GFProperty* setupProp = save ? API->FindProperty(save.Struct(), "GunSetup") : nullptr;
    GML_PropInfo setup{}, field{};
    if (!setupProp || !API->GetPropertyInfo(setupProp, &setup) || !setup.structType) return nullptr;
    GFProperty* fieldProp = API->FindProperty(setup.structType, name);
    if (!fieldProp || !API->GetPropertyInfo(fieldProp, &field)) return nullptr;
    return (uint8_t*)save.ptr + setup.offset + field.offset;
}
static GUObject** GunAssetField(Object save) { return (GUObject**)SetupField(save, "GunAsset"); }

// The receiver colour a setup was saved with: SaveGunSetup keeps the receiver's ColorOption, which the plugin
// sets to kMarker on every UAR-15 (see "multiplayer").
static bool SavedWithMarker(Object save) {
    auto* name = (uint64_t*)SetupField(save, "SelectedColor");
    char b[128] = "";
    if (name) API->NameToString(*name, b, sizeof b);
    return _stricmp(b, kMarker) == 0;
}

struct SavedGun { GUObject* asset; bool marked; };

// The gun a gear manager saved in slot `slotProp`, read with the game's own GetGunSetup.
static SavedGun SavedGunSetup(Object gear, const char* slotProp) {
    std::string slot = FStr8(gear, slotProp);
    if (slot.empty()) return {};
    Params gg(gear, "GetGunSetup");
    gg.SetString("SlotName", Widen8(slot.c_str())).Call();
    Object save = gg.Get<GUObject*>("GunSetup");
    GUObject** f = GunAssetField(save);
    return {f ? *f : nullptr, SavedWithMarker(save)};
}

// SaveGunSetup names a gun by a GunAsset in AvailableGuns with the receiver's class, and the M4 CQBR and the
// UAR-15 share one: a stock CQBR is saved as our runtime asset too (seen in game). So the GunAsset says nothing;
// only the UAR-15's colour name, which every UAR-15 carries and SaveGunSetup keeps, marks a UAR-15 slot.
static bool IsUarSlot(const std::string&, SavedGun saved) {
    if (!saved.asset || !saved.marked) return false;
    Object donor = FindObject(kDonor);
    return saved.asset == s_gun || saved.asset == donor.ptr;
}

// Re-save any gun slot the game wrote with the UAR-15 (or, from an earlier session, with a runtime GunAsset path
// that no longer resolves) so it names the donor; remember the new file's hash.
static void WatchLoadouts() {
    static ULONGLONG next = 0;
    static std::map<std::wstring, uint64_t> seen;  // file -> last write time
    if (!s_gun || GetTickCount64() < next) return;
    next = GetTickCount64() + 1000;
    std::wstring dir = SaveGamesDir();
    if (dir.empty()) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"SavedGun_*.sav").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring file = fd.cFileName;
        uint64_t stamp = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        if (seen[file] == stamp) continue;
        seen[file] = stamp;
        std::wstring stemW = file.substr(0, file.size() - 4);
        char slotBuf[260] = "";
        WideCharToMultiByte(CP_UTF8, 0, stemW.c_str(), -1, slotBuf, sizeof slotBuf, nullptr, nullptr);
        std::string slot = slotBuf;
        std::string raw = ReadFileBytes(dir + file);
        Params lg(Lib("GameplayStatics"), "LoadGameFromSlot");
        lg.SetString("SlotName", stemW).Set("UserIndex", (int32_t)0).Call();
        Object save = lg.Return<GUObject*>();
        GUObject** asset = GunAssetField(save);
        if (!asset) continue;
        // A runtime GunAsset path in the file (saved while this plugin wasn't watching) is the UAR-15 only if the
        // slot held the UAR-15 before: other plugins (the Noveske, the Saiga) make runtime GunAssets too.
        size_t t = raw.find("/Engine/Transient.");
        bool staleRuntime = !*asset && t != std::string::npos && raw.find(".GunAsset_C_", t) != std::string::npos;
        bool staleUar = staleRuntime && !RecordedHash(slot).empty();
        bool marked = SavedWithMarker(save);
        // Our runtime asset in the file: a UAR-15 if it carries the colour name, else a stock CQBR saved under the
        // shared receiver. Either way re-save it with the donor so the slot still loads next session.
        if (*asset == s_gun || staleUar) {
            Object donor = FindObject(kDonor);
            if (!donor) continue;
            *asset = donor.ptr;
            Params sv(Lib("GameplayStatics"), "SaveGameToSlot");
            sv.SetObj("SaveGameObject", save).SetString("SlotName", stemW).Set("UserIndex", (int32_t)0).Call();
            if (!sv.Return<bool>()) { Error("could not re-save loadout slot {}", slot); continue; }
            RecordHash(slot, marked ? HashHex(ReadFileBytes(dir + file)) : std::string());
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (GetFileAttributesExW((dir + file).c_str(), GetFileExInfoStandard, &a))
                seen[file] = ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
            Log("loadout slot {} holds {}: re-saved with a stable reference{}", slot, marked ? "the UAR-15" : "a stock M4 CQBR",
                staleUar ? " (repaired a save from an earlier session)" : "");
        } else if (!RecordedHash(slot).empty() && !IsUarSlot(slot, {*asset, marked})) {
            RecordHash(slot, "");  // the game saved a different gun there since
            Log("loadout slot {} no longer holds the UAR-15", slot);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// A receiver the gear manager rebuilt from your saved loadout (team room start, operations): it is ours if it
// became a player's PrimaryGun/SecondaryGun and that slot holds the UAR-15.
static bool ResolveLoadout(Object actor) {
    for (auto& gear : FindAllOf("BP_GearManagerComponent_C")) {
        Object pawn(API->GetOuter(gear.ptr));
        if (pawn.IsA("Pawn")) {
            Params lc(pawn, "IsLocallyControlled");
            lc.Call();
            if (!lc.Return<bool>()) continue;
        }
        const char* slotProp = gear.GetObj("PrimaryGun").ptr == actor.ptr     ? "PrimaryGunSetupSlotName"
                               : gear.GetObj("SecondaryGun").ptr == actor.ptr ? "SecondaryGunSetupSlotName"
                                                                               : nullptr;
        if (!slotProp) continue;
        SavedGun saved = SavedGunSetup(gear, slotProp);
        std::string slot = FStr8(gear, "GunSetupSlotName") + "_" + FStr8(gear, slotProp);  // SavedGun_MainPrimary
        bool uar = IsUarSlot(slot, saved);
        Log("{} loaded from {} ({}): saved gun '{}'{} -> {}", actor.Name(), slot, Object(API->GetOuter(gear.ptr)).Name(),
            saved.asset ? Object(saved.asset).Name() : std::string("?"), saved.marked ? " named a UAR-15" : "", uar ? "UAR-15" : "not the UAR-15");
        if (uar) {
            DressAsUar(actor);
            Mark(actor);
        }
        return true;
    }
    return false;
}

// The spawner's menu items are WB_CustomizationItemGun widgets; the gun they stand for is their WeaponItem.
static GUObject* GunOf(GUObject* itemOrGun) {
    Object o(itemOrGun);
    if (o && o.IsA("WB_CustomizationItemGun_C")) return o.GetObj("WeaponItem").ptr;
    return itemOrGun;
}

static GUObject* s_lastPressedGun = nullptr;  // any spawner menu item pressed, most recent
static ULONGLONG s_lastPressedAt = 0;

// A gun station whose player is someone else's: this machine's menu presses say nothing about it.
static bool ForeignStation(Object station) {
    Object player = station.GetObj("Player");
    if (!player) return false;
    Params lc(player, "IsLocallyControlled");
    lc.Call();
    return !lc.Return<bool>();
}

// An M4 receiver began play. It is a UAR-15 if another machine named it one, if the gun station now holding it
// as CurrentReceiver was last asked for the UAR-15 entry on this machine, or if it is a loadout gun from a UAR-15
// slot. The station sets CurrentReceiver right after the spawn returns, so this is checked on the following
// frames.
static void ResolvePending() {
    if (s_pending.empty()) return;
    auto stations = FindAllOf("CustomizationStation_Gun_C");
    for (size_t i = 0; i < s_pending.size();) {
        Pending& p = s_pending[i];
        Object actor(p.actor);
        bool done = !actor || !API->IsValid(actor.ptr);
        if (!done && IsMarked(actor)) {
            Log("{} is named a UAR-15 by the game it came from", actor.Name());
            DressAsUar(actor);
            done = true;
        }
        for (auto& st : stations) {
            if (done || st.GetObj("CurrentReceiver").ptr != p.actor) continue;
            done = true;
            if (ForeignStation(st)) break;
            GUObject* picked = nullptr;
            const char* how = "?";
            if (auto pk = s_picks.find(st.ptr); pk != s_picks.end()) { picked = pk->second; how = "selection"; }
            if (!picked && GetTickCount64() - s_lastPressedAt < 30000) { picked = s_lastPressedGun; how = "last menu press"; }
            Log("{} spawned at {}: picked '{}' (via {}) -> {}", actor.Name(), st.Name(), picked ? Object(picked).Name() : std::string("?"),
                how, picked == s_gun ? "UAR-15" : "stock M4");
            if (picked == s_gun) {
                DressAsUar(actor);
                Mark(actor);
            }
        }
        if (!done) done = ResolveLoadout(actor);
        if (!done && GetTickCount64() - p.since > 5000) done = true;  // a world or enemy M4 stays one
        if (done) s_pending.erase(s_pending.begin() + i);
        else i++;
    }
}

// A level started: build the assets while it loads, and forget what was dressed (a new level's actors can reuse
// old addresses).
static void NewLevel() {
    BuildVisualAssets();
    std::erase_if(s_dressedGuns, [](const DressedGun& d) { return !API->IsValid(d.gun) || !IsDressed(Object(d.gun)); });
    s_picks.clear();
}

// The level changed if the game state did (GML's WORLD_BEGIN_PLAY comes from the game mode, which only the host
// has; the game state is on every machine).
static void WatchLevel() {
    static ULONGLONG next = 0;
    static std::string level;
    if (GetTickCount64() < next) return;
    next = GetTickCount64() + 500;
    Params gs(Lib("GameplayStatics"), "GetGameState");
    gs.SetObj("WorldContextObject", Object(API->WorldContext())).Call();
    Object state = gs.Return<GUObject*>();
    if (!state) return;
    std::string now = state.FullName();
    if (now == level) return;
    level = now;
    NewLevel();
}

static void QueueReceiver(GUObject* actor) {
    for (auto& pe : s_pending)
        if (pe.actor == actor) return;
    s_pending.push_back({actor, GetTickCount64()});
}

static void InstallVisuals() {
    // The spawner's selection. On a client the station asks the server for the gun with the GunAsset it is
    // handed, and the server can't resolve this machine's runtime UAR-15 asset: for that call the menu item stands
    // for the M4 donor (same receiver), and this machine remembers the UAR-15 was picked.
    static Object swapped;
    Hook(kSelectHook,
         [](Object widget, GUFunction* fn, void* params) {
             Object item(Param<GUObject*>(fn, params, "Item"));
             if (!item || !item.IsA("WB_CustomizationItemGun_C")) return false;
             GUObject* gun = item.GetObj("WeaponItem").ptr;
             Object station = widget.GetObj("OwningStation");
             if (station) s_picks[station.ptr] = gun;
             if (gun != s_gun || !station || HasAuthority(station)) return false;
             Object donor = FindObject(kDonor);
             if (!donor) return false;
             item.Set<GUObject*>("WeaponItem", donor.ptr);
             swapped = item;
             Log("{}: asking the server for the M4 donor; this machine dresses it as the UAR-15", station.Name());
             return false;
         },
         [](Object widget, GUFunction* fn, void* params) {
             if (swapped) {
                 swapped.Set<GUObject*>("WeaponItem", s_gun);
                 swapped = {};
             }
             Log("spawner selection in {}: {}", widget.Name(), Object(GunOf(Param<GUObject*>(fn, params, "Item"))).Name());
         });
    HookAfter("WB_CustomizationItemGun_C:BndEvt__WB_CustomizationItem_Button_46_K2Node_ComponentBoundEvent_1_OnButtonPressedEvent__DelegateSignature",
              [](Object item, GUFunction*, void*) {
                  if (GUObject* gun = GunOf(item.ptr)) {
                      s_lastPressedGun = gun;
                      s_lastPressedAt = GetTickCount64();
                  }
              });
    HookAfter("*:ReceiveBeginPlay", [](Object self, GUFunction*, void*) {
        if (!s_receiverClass) {
            Object donor = FindObject(kDonor);
            if (donor) s_receiverClass = donor.Get<GUClass*>("ReceiverClass");
        }
        if (s_receiverClass && self.Class() == s_receiverClass) QueueReceiver(self.ptr);

    });
    for (const char* f : {"ColorComponent:OnRep_ColorOption", "ColorComponent:ApplyColor_Server", "ColorComponent:ApplyColor"})
        HookAfter(f, [](Object cc, GUFunction*, void*) {
            Params o(cc, "GetOwner");
            o.Call();
            Object actor = o.Return<GUObject*>();
            if (!actor || !s_receiverClass || actor.Class() != s_receiverClass) return;
            if (IsDressed(actor) || !IsMarked(actor)) return;
            QueueReceiver(actor.ptr);
        });
    On(GML_EVENT_TICK, [](void*) {
        WatchLevel();
        ResolvePending();
        WatchLoadouts();
        WatchDressedGuns();
        PoseRigs();
    });
    On(GML_EVENT_WORLD_BEGIN_PLAY, [](void*) { NewLevel(); });
}

// ------------------------------------------------------------------ entry

GML_PLUGIN("com.remarrow.uar15", "UAR-15", UAR15_VERSION);

GML_AWAKE() {
    Message(UAR15_MODEL_CREDIT);
    s_offset[0] = kFitShiftX + Config.Bind("Visuals", "OffsetX", 0.0f, "cm, + = toward the muzzle (on top of the plugin's own fit)").Value();
    s_offset[1] = Config.Bind("Visuals", "OffsetY", 0.0f, "cm, + = to the gun's right").Value();
    s_offset[2] = Config.Bind("Visuals", "OffsetZ", 0.0f, "cm, + = up").Value();
    s_fitAttachments = Config.Bind("Visuals", "FitAttachments", true, "Move the M4's muzzle point and stock butt onto the UAR-15's.").Value();
    s_ownMovingParts = Config.Bind("Visuals", "OwnMovingParts", true, "The UAR-15's own bolt carrier, charging handle, trigger, selector and mag release.").Value();
    s_gripPitch = Config.Bind("Hands", "GripPitch", s_gripPitch, "degrees the pistol-grip hand turns nose-up from the M4's pose (the BCM grip is more upright)").Value();
    s_gripX = Config.Bind("Hands", "GripX", s_gripX, "cm the pistol-grip hand moves forward").Value();
    s_gripZ = Config.Bind("Hands", "GripZ", s_gripZ, "cm the pistol-grip hand moves up").Value();
    s_guardIn = Config.Bind("Hands", "HandguardIn", s_guardIn, "cm the handguard hand moves in toward the (narrower) handguard").Value();
    s_guardUp = Config.Bind("Hands", "HandguardUp", s_guardUp, "cm the handguard hand moves up").Value();
    s_guardCurl = Config.Bind("Hands", "HandguardCurl", s_guardCurl, "degrees more curl at the handguard fingers' middle and tip joints").Value();

    HookAfter("BP_GameInstance_C:GetItems", [](Object gi, GUFunction*, void*) { AddGun(gi); });
    On(GML_EVENT_WORLD_BEGIN_PLAY, [](void*) {
        if (Object gi = FindFirstOf("BP_GameInstance_C")) AddGun(gi);  // GetItems ran before we could see it
    });
    InstallVisuals();
    return 0;
}

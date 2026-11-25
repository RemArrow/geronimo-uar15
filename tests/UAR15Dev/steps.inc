// UAR15Dev: the test steps (included at the end of UAR15Dev.cpp).

static void LogBones(const char* when) {
    Object mov = s_gun.GetObj("Movables");
    if (!mov) return;
    std::string out;
    for (const char* b : {"RootBoltCarrierGroup", "RootChargingHandle", "RootMagRelease", "RootTrigger", "RootSelector"}) {
        Params bt(mov, "GetBoneTransformByName");
        *(uint64_t*)bt.Ptr("BoneName") = API->MakeName(b);
        bt.Set("BoneSpace", (uint8_t)1).Call();
        double x[12];
        std::memcpy(x, bt.Ptr("ReturnValue"), sizeof x);
        out += std::format(" {} t({:.2f},{:.2f},{:.2f}) q({:.3f},{:.3f},{:.3f},{:.3f})", b, x[4], x[5], x[6], x[0], x[1], x[2], x[3]);
    }
    Log("BONES [{}]{}", when, out);
}


static Object SpawnAt(const char* cls, Vec at) {
    Params mk(Lib("KismetMathLibrary"), "MakeTransform");
    mk.Set("Location", at).Set("Scale", Vec{1, 1, 1}).Call();
    Params b(Lib("GameplayStatics"), "BeginDeferredActorSpawnFromClass");
    b.SetObj("WorldContextObject", Object(API->WorldContext())).Set("ActorClass", FindClass(cls)).Set("CollisionHandlingOverride", (uint8_t)1);
    std::memcpy(b.Ptr("SpawnTransform"), mk.Ptr("ReturnValue"), 0x60);
    b.Call();
    Object a = b.Return<GUObject*>();
    if (!a) return a;
    Params f(Lib("GameplayStatics"), "FinishSpawningActor");
    f.SetObj("Actor", a);
    std::memcpy(f.Ptr("SpawnTransform"), mk.Ptr("ReturnValue"), 0x60);
    f.Call();
    return a;
}

// The look of the gun beyond its base colour: world normals (flat-normal faces must shade smoothly, as the M4's do)
// and the lit scene under a point light beside the camera (a glossy surface shows a small hard highlight).
static void CaptureShading(const std::string& file, double cx) {
    if (!s_light) {
        s_light = SpawnAt("PointLight", {s_holdAt.x + cx, s_holdAt.y + 150, s_holdAt.z + 60});
        if (Object lc = s_light ? s_light.GetObj("PointLightComponent") : Object()) {
            Params mob(lc, "SetMobility"); mob.Set("NewMobility", (uint8_t)2).Call();
            Params in(lc, "SetIntensity"); in.Set("NewIntensity", 20000.0f).Call();
            Params ar(lc, "SetAttenuationRadius"); ar.Set("NewRadius", 1000.0f).Call();
        }
        Log("SHADING light {}", s_light.Name());
    } else {
        Params mv(s_light, "K2_SetActorLocation");
        mv.Set("NewLocation", Vec{s_holdAt.x + cx, s_holdAt.y + 150, s_holdAt.z + 60}).Set("bSweep", false).Set("bTeleport", true).Call();
    }
    s_capSource = 6;
    CaptureFrom(file + "_normal_right", {cx, 230, 10}, {cx, 0, 0});
    CaptureFrom(file + "_normal_top", {cx, 20, 230}, {cx, 0, 0});
    s_capSource = 2;
    CaptureFrom(file + "_lit_right", {cx, 230, 10}, {cx, 0, 0});
    CaptureFrom(file + "_lit_action", {0, 60, 3}, {0, 0, 0});
    CaptureFrom(file + "_lit_barrel", {32, 28, 8}, {32, 0, 2});
    CaptureFrom(file + "_lit_barrel_top", {32, 3, 30}, {32, 0, 2});
    s_capSource = 7;
    // where the hands and the magazine meet the gun: magwell and grip from the left, charging handle from above-behind
    CaptureFrom(file + "_magwell", {0, -45, -5}, {0, 0, -5});
    CaptureFrom(file + "_handle", {-22, -14, 14}, {-8, 0, 4});
    Log("SHOT {} (normals, lit, close-ups)", file);
}

// What each UAR-15 material instance samples: its Normal and ORM textures and how the engine decodes them.
static void LogMaterials(Object gun) {
    auto parts = Components(gun, "DynamicMeshComponent");
    if (parts.empty()) return;
    Params nm(parts[0], "GetNumMaterials");
    nm.Call();
    int n = nm.Return<int32_t>();
    for (int i = 0; i < n && i < 3; i++) {
        Params gm(parts[0], "GetMaterial");
        gm.Set("ElementIndex", (int32_t)i).Call();
        Object mid = gm.Return<GUObject*>();
        std::string out;
        for (const char* p : {"Diffuse", "Normal", "ORM"}) {
            Params tp(mid, "K2_GetTextureParameterValue");
            *(uint64_t*)tp.Ptr("ParameterName") = API->MakeName(p);
            tp.Call();
            Object tex = tp.Return<GUObject*>();
            out += std::format(" {}={} (SRGB {}, compression {})", p, tex ? tex.Path() : std::string("none"), tex ? tex.GetBool("SRGB") : false,
                               tex ? (int)tex.Get<uint8_t>("CompressionSettings") : -1);
        }
        Log("MATERIAL slot {} {}:{}", i, mid.Name(), out);
    }
}

static int CountOptics(Object gun, const char* when) {
    int n = 0;
    Params ga(gun, "GetAttachedActors");
    ga.Set("bResetArray", true).Set("bRecursivelyIncludeAttachedActors", true).Call();
    if (auto* arr = (GML_TArray*)ga.Ptr("OutActors"))
        for (int i = 0; i < arr->Num; i++) {
            Object a(((GUObject**)arr->Data)[i]);
            if (!a.IsA("GunAttachment")) continue;
            n++;
            Vec l = ToActor(gun, ActorLocation(a));
            Log("OPTIC [{}] on {}: {} at ({:.2f},{:.2f},{:.2f}) attached {}", when, gun.Name(), a.Name(), l.x, l.y, l.z, a.GetBool("bAttached"));
        }
    Log("OPTIC [{}] {} attachment(s) on {}; ACOG actors in the world: {}", when, n, gun.Name(), FindAllOf("ACOG_4x_C").size());
    return n;
}

static int s_mode = 0;
static Object s_uarActor;

static std::string ColorName(Object receiver) {
    Object cc = receiver ? receiver.GetObj("ColorComponent") : Object();
    return cc ? NameStr(cc.Get<uint64_t>("ColorOption")) : std::string("-");
}

static void Step() {
    if (s_wait > 0) { s_wait--; return; }
    switch (s_step) {
        case 0: {
            auto stations = FindAllOf("CustomizationStation_Gun_C");
            Object pawn = Pawn();
            Vec me = ActorLocation(pawn);
            double best = 1e30;
            for (auto& st : stations) {
                Vec l = ActorLocation(st);
                double d = (l.x - me.x) * (l.x - me.x) + (l.y - me.y) * (l.y - me.y);
                if (d < best && st.GetObj("WBGunBuilder")) { best = d; s_station = st; }
            }
            s_widget = s_station.GetObj("WBGunBuilder");
            for (auto& g : FindAllOf("GunAsset_C"))
                if (TextOf(g, "DisplayName") == "UAR-15") s_uar = g;
            s_donor = FindObject(kDonor);
            Log("{} station(s); using {}; widget {}; UAR-15 entry {} (category {}, icon {}); donor {} '{}' (category {})", stations.size(),
                s_station.Name(), s_widget.Name(), s_uar.Name(), s_uar.Get<uint8_t>("Category"), s_uar.GetObj("Icon").Name(), s_donor.Name(),
                TextOf(s_donor, "DisplayName"), s_donor.Get<uint8_t>("Category"));
            if (!s_widget || !s_uar || !s_donor) { Error("missing pieces"); s_step = 30; return; }
            Params sl(s_station, "K2_GetActorLocation");
            sl.Call();
            Params mv(pawn, "K2_SetActorLocation");
            mv.Set("NewLocation", sl.Return<Vec>()).Set("bSweep", false).Set("bTeleport", true).Call();
            OpenCategory(s_donor.Get<uint8_t>("Category"));
            s_step = 1; s_wait = 60;
            return;
        }
        // ---- a stock M4, to read its layout
        case 1:
            if (!Select(s_donor)) { s_step = 10; return; }
            s_step = 2; s_wait = 90;
            return;
        case 2: {
            Object cur = Spawned();
            if (!cur) return;
            s_gun = cur;
            Freeze(s_gun);
            Vec at = ActorLocation(Pawn());
            s_holdAt = {at.x, at.y, at.z + 120};
            s_holding = true;
            s_step = 3; s_wait = 240;
            return;
        }
        case 3:
            DumpGun("stock M4");
            LogBones("stock M4 at rest");
            CaptureGun("m4", 8);
            CaptureShading("m4", 8);
            s_holding = false;
            s_step = 10;
            return;
        // ---- the UAR-15
        case 10:
            OpenCategory(s_donor.Get<uint8_t>("Category"));
            s_step = 11; s_wait = 30;
            return;
        case 11:
            if (!Select(s_uar)) { s_step = 30; return; }
            s_step = 12; s_wait = 90;
            return;
        case 12: {
            Object cur = Spawned();
            if (!cur) return;
            s_gun = cur;
            Freeze(s_gun);
            Vec at = ActorLocation(Pawn());
            s_holdAt = {at.x, at.y, at.z + 120};
            s_holding = true;
            Log("spawned {} - holding it at ({:.0f},{:.0f},{:.0f})", s_gun.FullName(), s_holdAt.x, s_holdAt.y, s_holdAt.z);
            s_step = 13; s_wait = 240;
            return;
        }
        case 13:
            DumpGun("UAR-15");
            LogParts("dressed");
            LogBones("UAR-15 at rest");
            CaptureGun("uar", 12);
            CaptureFrom("uar_action", {0, 60, 3}, {0, 0, 0});
            CaptureShading("uar", 12);
            LogMaterials(s_gun);
            s_step = 40; s_wait = 10;
            return;
        // ---- an optic from the station's menu, as a player picks one: where it mounts, the iron sights
        // The station's optics menu lists nothing without a headset: the optic is mounted the way the attachment itself
        // does it when a hand puts it on a rail (closest rail spot, then AttachToServer onto the top rail's spline).
        case 40: {
            Object rail;
            for (auto& c : Components(s_gun, "SceneComponent"))
                if (c.Name() == "ReceiverAttachmentSpline") rail = c;
            Vec top{s_holdAt.x - 2, s_holdAt.y, s_holdAt.z + 8};  // the gun is held unrotated at s_holdAt
            for (auto& c : Components(s_gun, "SceneComponent"))
                if (c.Name().rfind("ReceiverAttachmentSpline", 0) == 0) { Vec l = ToActor(s_gun, CompLocation(c)); Log("OPTIC rail {} at ({:.2f},{:.2f},{:.2f}) - the UAR-15's top rail is at Z 5.14", c.Name(), l.x, l.y, l.z); }
            Params mk(Lib("KismetMathLibrary"), "MakeTransform");
            mk.Set("Location", top).Set("Scale", Vec{1, 1, 1}).Call();
            Params b(Lib("GameplayStatics"), "BeginDeferredActorSpawnFromClass");
            b.SetObj("WorldContextObject", Object(API->WorldContext())).Set("ActorClass", FindClass("ACOG_4x_C")).Set("CollisionHandlingOverride", (uint8_t)1);
            std::memcpy(b.Ptr("SpawnTransform"), mk.Ptr("ReturnValue"), 0x60);
            b.Call();
            Object optic = b.Return<GUObject*>();
            if (!optic || !rail) { Log("OPTIC no ACOG ({}) or no top rail ({})", optic.Name(), rail.Name()); s_step = 14; return; }
            Params f(Lib("GameplayStatics"), "FinishSpawningActor");
            f.SetObj("Actor", optic);
            std::memcpy(f.Ptr("SpawnTransform"), mk.Ptr("ReturnValue"), 0x60);
            f.Call();
            if (Object root = optic.GetObj("StaticMeshComponent")) { Params sp(root, "SetSimulatePhysics"); sp.Set("bSimulate", false).Call(); }
            Params sr(optic, "SetAttachableReceiver");
            sr.SetObj("NewReceiver", s_gun).Call();
            Params ui(optic, "UpdateAttachInfo");
            ui.Call();
            Params ct(optic, "GetClosestAttachTransform");
            ct.Set("FromLocation", top).Call();
            bool found = ct.Return<bool>();
            Params at(optic, "AttachToServer");
            at.SetObj("Comp", rail);
            std::memcpy(at.Ptr("WorldTransform"), ct.Ptr("OutTransform"), 0x60);
            if (!found) {  // headless: the middle of the top rail, as a hand would put it there
                Params len(rail, "GetSplineLength");
                len.Call();
                Params tx(rail, "GetTransformAtDistanceAlongSpline");
                tx.Set("Distance", len.Return<float>() * 0.5f).Set("CoordinateSpace", (uint8_t)1).Set("bUseScale", false).Call();
                std::memcpy(at.Ptr("WorldTransform"), tx.Ptr("ReturnValue"), 0x60);
            }
            at.Call();
            Log("OPTIC {} spawned, closest rail spot found {}, AttachToServer on {}", optic.Name(), found, rail.Name());
            s_step = 42; s_wait = 120;
            return;
        }
        case 42: {
            Params ga(s_gun, "GetAttachedActors");
            ga.Set("bResetArray", true).Set("bRecursivelyIncludeAttachedActors", true).Call();
            if (auto* arr = (GML_TArray*)ga.Ptr("OutActors"))
                for (int i = 0; i < arr->Num; i++) {
                    Object a(((GUObject**)arr->Data)[i]);
                    Vec l = ToActor(s_gun, ActorLocation(a));
                    Object ap = a.IsA("GunAttachment") ? a.GetObj("AttachablePoint") : Object();
                    Log("OPTIC attached {} [{}] at ({:.2f},{:.2f},{:.2f}) tag '{}' point {} attached {}", a.Name(), ClassName(a), l.x, l.y, l.z,
                        a.IsA("GunAttachment") ? NameStr(a.Get<uint64_t>("AttachPointTag")) : std::string(), ap.Name(), a.GetBool("bAttached"));
                }
            auto parts = Components(s_gun, "DynamicMeshComponent");
            Log("OPTIC iron sights visible: {}", parts.size() > 2 ? Visible(parts[2]) : false);
            CaptureGun("uar_optic", 12);
            CountOptics(s_gun, "mounted, 2 s later");
            s_step = 43; s_wait = 300;
            return;
        }
        case 43: {
            CountOptics(s_gun, "after 5 s more of the plugin watching it");
            s_step = 14; s_wait = 10;
            return;
        }
        case 14: {  // fire through the game's own FireRoundLocal: one 5.56 round
            s_rounds = 0;
            Params fr(s_gun, "FireRoundLocal");
            fr.Call();
            Log("FIRE called FireRoundLocal once: {} round(s) began play", s_rounds);
            s_step = 15; s_wait = 30;
            return;
        }
        case 15:
            SetHandle(0.5f);
            s_step = 16; s_wait = 3;
            return;
        case 16:
            LogParts("handle half back");
            LogBones("handle half back");
            SetHandle(1.0f);
            s_step = 17; s_wait = 3;
            return;
        case 17:
            LogParts("handle all the way back");
            LogBones("handle all the way back");
            CaptureFrom("uar_handleback", {0, 60, 3}, {0, 0, 0});
            CaptureFrom("uar_handleback_top", {-2, 0, 60}, {-2, 0, 0});
            SetHandle(0.0f);
            s_gun.Set("CurrentTrigger", 1.0f);
            s_gun.SetBool("bIsMagReleasePressed", true);
            s_step = 18; s_wait = 30;
            return;
        case 18:
            LogParts("home, trigger pulled, mag release pressed");
            LogBones("trigger pulled, mag release pressed");
            CaptureFrom("uar_trigger", {0, 40, -2}, {0, 0, -2});
            s_gun.Set("CurrentTrigger", 0.0f);
            s_gun.SetBool("bIsMagReleasePressed", false);
            s_mode = 0;
            s_step = 19; s_wait = 10;
            return;
        case 19: {  // each fire mode: the selector follows the M4's
            if (s_mode > 0) {
                std::string tag = std::format("fire mode {}", s_gun.Get<uint8_t>("FireMode"));
                LogParts(tag.c_str());
                LogBones(tag.c_str());
                CaptureFrom(std::format("uar_selector_{}", s_mode), {-4, -35, -1}, {-4, 0, -1});
            }
            if (s_mode == 3) { s_step = 20; s_wait = 10; return; }
            Params fm(s_gun, "UpdateFireModeReplicated");
            fm.Set("NewFireMode", (uint8_t)s_mode).Call();
            s_mode++;
            s_wait = 45;
            return;
        }
        case 20: {  // save it as the game saves a loadout gun, then rebuild the loadout as an operation does
            for (auto& g : FindAllOf("BP_GearManagerComponent_C"))
                if (API->GetOuter(g.ptr) == Pawn().ptr) s_gear = g;
            if (!s_gear) { Error("no gear manager"); s_step = 30; return; }
            Params sv(s_gear, "SaveGunSetup");
            sv.SetObj("GunReceiver", s_gun).Call();
            Log("LOADOUT saved {} (slot SavedGun_{}_{})", s_gun.Name(), FStr8(s_gear, "GunSetupSlotName"), FStr8(s_gear, "PrimaryGunSetupSlotName"));
            s_step = 21; s_wait = 180;  // the plugin re-saves the slot with a stable reference within a second
            return;
        }
        case 21: {
            s_holding = false;
            Params ld(s_gear, "LoadGunSetups");
            ld.Call();
            Log("LOADOUT LoadGunSetups called");
            s_step = 22; s_wait = 300;
            return;
        }
        case 22: {
            Object pg = s_gear.GetObj("PrimaryGun");
            Log("LOADOUT PrimaryGun after load: {} class {} - UAR-15 meshes {}", pg.Name(), ClassName(pg), Components(pg, "DynamicMeshComponent").size());
            int optics = CountOptics(pg, "loadout gun rebuilt");
            Log("LOADOUT optic count on the rebuilt UAR-15: {} -> {}", optics, optics == 1 ? "PASS (one, as saved)" : "FAIL");
            s_uarActor = s_gun;
            s_step = 23;
            return;
        }
        // ---- back to the stock M4 CQBR after the UAR-15: it must stay an M4, at the station and in the loadout
        case 23:
            OpenCategory(s_donor.Get<uint8_t>("Category"));
            s_step = 24; s_wait = 30;
            return;
        case 24:
            if (!Select(s_donor)) { s_step = 30; return; }
            s_step = 25; s_wait = 240;
            return;
        case 25: {
            Object cur = Spawned();
            if (!cur) return;
            s_gun = cur;
            Log("CQBR spawned {} (same actor as the UAR-15: {}) colour '{}' - UAR-15 meshes {}", cur.Name(), cur.ptr == s_uarActor.ptr,
                ColorName(cur), Components(cur, "DynamicMeshComponent").size());
            Params sv(s_gear, "SaveGunSetup");
            sv.SetObj("GunReceiver", s_gun).Call();
            Log("CQBR saved to the loadout");
            s_step = 26; s_wait = 180;
            return;
        }
        case 26: {
            Params ld(s_gear, "LoadGunSetups");
            ld.Call();
            s_step = 27; s_wait = 300;
            return;
        }
        case 27: {
            Object pg = s_gear.GetObj("PrimaryGun");
            Log("CQBR LOADOUT PrimaryGun after load: {} colour '{}' - UAR-15 meshes {} -> {}", pg.Name(), ColorName(pg),
                Components(pg, "DynamicMeshComponent").size(), Components(pg, "DynamicMeshComponent").empty() ? "PASS (an M4)" : "FAIL (dressed as the UAR-15)");
            s_step = 30;
            return;
        }
        case 30:
            Log("DEV TEST COMPLETE");
            s_step = 99;
            return;
    }
}

GML_AWAKE() {
    s_shotDir = Config.Bind<std::string>("Test", "ShotDir", "", "Where the captures go.").Value();
    On(GML_EVENT_WORLD_BEGIN_PLAY, [](void* gm) {
        std::string level = Object((GUObject*)gm).Path();
        if (level.find("/Game/Maps/MainMenu/") == 0) {
            Params p(Lib("GameplayStatics"), "OpenLevel");
            p.SetObj("WorldContextObject", Object(API->WorldContext())).Set("LevelName", API->MakeName("/Game/Maps/TeamRoom/Teamroom")).Set("bAbsolute", true).Call();
        } else if (level.find("Teamroom") != std::string::npos) {
            s_step = 0;
            s_wait = 600;  // let the room and the player's kit load
        }
    });
    HookAfter("BP_GunProjectile_C:ReceiveBeginPlay", [](Object b, GUFunction*, void*) {
        s_rounds++;
        Vec v = b.Get<Vec>("Velocity");
        Log("ROUND {} [{}] Mass {:.4f} kg Diameter {:.3f} cm speed {:.0f} m/s", b.Name(), ClassName(b), b.Get<float>("Mass"), b.Get<float>("Diameter"),
            std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z) / 100);
    });
    On(GML_EVENT_TICK, [](void*) {
        if (s_step >= 0 && s_step < 99) Step();
        Hold();
    });
    HookAfter("CustomizationStation_Gun_C:ReceiveTick", [](Object, GUFunction*, void*) { Hold(); });
    return 0;
}

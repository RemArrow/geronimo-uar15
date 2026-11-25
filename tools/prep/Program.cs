// prep - turns the UAR-15 from the BONELAB Cold Weapon Pack (AssetRipper glb export; the model itself is an
// Arma 3 port with colour maps only) into the plugin's Assets\:
//   uar.obj             receiver (upper, lower), barrel, gas block, handguard, buffer tube, stock, grip (static)
//   uar_muzzle.obj      the A2 flash hider (hidden while a muzzle device is mounted)
//   uar_sights.obj      the Magpul MBUS front and rear sights (hidden while an optic is on the top rail)
//   uar_bolt.obj        bolt carrier                     -> rides the M4's RootBoltCarrierGroup bone
//   uar_charging.obj    charging handle                  -> rides the M4's RootChargingHandle bone
//   uar_trigger.obj / uar_selector.obj / uar_magrelease.obj
//   <Part>_{Diffuse,Normal,ORM}.png for the game's M_DefaultShader, one set per glTF material
//   Icon.png            the spawner thumbnail
//
//   prep <UAR-15.glb> <Assets dir> <m4 static points.obj> [preview dir]
//
// Space: the game's (X forward, Y right, Z up, cm). AssetRipper writes Unity's left-handed frame as glTF by
// negating X, so in the glb +Z is the muzzle, +Y up and -X the gun's right: UE = (z, -x, y) * 100. That is a
// mirror (right-handed glTF to left-handed UE), which with GML's no-flip OBJ import keeps faces the right way
// out, as the Barrett's and Saiga's preps do.
//
// Fit: both guns are mil-spec AR-15s, so the UAR-15's upper and lower receivers are fitted onto the game's
// M4A1 Block II mesh: start with the chambers together, then a trimmed rigid ICP (translation, then rotation).
// The fit is baked into the OBJs. The M4's points come from its cooked static mesh (UAssetParser uexppos).
using System.Globalization;
using System.Numerics;
using System.Text;
using System.Text.Json;
using SixLabors.ImageSharp;
using SixLabors.ImageSharp.PixelFormats;
using SixLabors.ImageSharp.Processing;

CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
if (args.Length < 3) { Console.WriteLine("usage: prep <UAR-15.glb> <Assets dir> <m4 points.obj> [preview dir]"); return 1; }
string outDir = args[1];
string? previewDir = args.Length > 3 ? args[3] : null;
Directory.CreateDirectory(outDir);

var gun = Glb.Load(args[0]);

// texture set of each glTF material (OBJ usemtl group = plugin material slot)
var partOfMaterial = new Dictionary<string, string> {
    ["MfMM_370_co"] = "Barrel", ["MMM_Gas_co"] = "Gas", ["MMM_Bolt_CO"] = "Bolt", ["MMM_Charging_CO"] = "Charging",
    ["MMM_Handguard_co"] = "Handguard", ["MMM_BCM_Mod1_CO"] = "Grip", ["MMM_Tube_CO"] = "Tube", ["MMM_Lower_CO"] = "Lower",
    ["MMM_Upper_CO"] = "Upper", ["MsMM_BCM_Mod0_CO"] = "Stock", ["MMM_USGI_A2_co"] = "Muzzle",
    ["P3D_ mmm_mbus_gen2_rear_co.paa __ mmm_mbus_gen2_rear.rvmat"] = "MbusRear",
    ["sight_rear_all_magpul_mbus_gen2_LOD0_diff_fde 1"] = "SightRear", ["sight_front_all_magpul_mbus_gen2_LOD0_diff_fde 1"] = "SightFront",
};
// the plugin's material slot order (src\UAR15.cpp kParts must match)
string[] parts = ["Barrel", "Gas", "Bolt", "Charging", "Handguard", "Grip", "Tube", "Lower", "Upper", "Stock", "Muzzle", "MbusRear", "SightRear", "SightFront"];
// steel and aluminium parts; the rest is polymer
var metalParts = new HashSet<string> { "Barrel", "Gas", "Bolt", "Charging", "Handguard", "Tube", "Lower", "Upper", "Muzzle" };
const int maxSize = 2048;
// mesh node -> OBJ file. Sights are picked by node index: their names repeat in the LOD children.
var fileOfNode = new Dictionary<string, string> {
    ["Gun Black"] = "uar", ["Stock"] = "uar",
    ["Muzzle"] = "uar_muzzle",
    ["Bolt"] = "uar_bolt", ["Charing Handle"] = "uar_charging",
    ["Trigger"] = "uar_trigger", ["Switch"] = "uar_selector", ["MagRelease"] = "uar_magrelease",
};
string[] sightNodes = ["sight_rear_all_magpul_mbus_gen2_base", "sight_rear_all_magpul_mbus_gen2_unfolded",
                       "sight_front_all_magpul_mbus_gen2_base_LOD0_0", "sight_front_all_magpul_mbus_gen2_folded_LOD0_0"];

// ---- geometry, in UE space before the fit
var meshes = new Dictionary<string, Mesh>();
Mesh Out(string file) => meshes.TryGetValue(file, out var m) ? m : meshes[file] = new Mesh();
int root = gun.FindNode("UAR-15");
var g = gun.Globals(root);
var used = new HashSet<string>();
var nodeBounds = new Dictionary<string, (Vector3 lo, Vector3 hi)>();
foreach (var (node, m) in g.OrderBy(k => k.Key)) {
    string name = gun.NodeName(node);
    if (!gun.HasMesh(node)) continue;
    string? file = fileOfNode.GetValueOrDefault(name) ?? (sightNodes.Contains(name) ? "uar_sights" : null);
    if (file == null || !used.Add(name)) continue;  // first (LOD0) node of each name only
    var target = Out(file);
    int before = target.V.Count;
    gun.AddMesh(node, m, target, partOfMaterial);
    var added = target.V.Skip(before).ToArray();
    nodeBounds[name] = (added.Aggregate(new Vector3(1e9f), Vector3.Min), added.Aggregate(new Vector3(-1e9f), Vector3.Max));
}
foreach (var k in fileOfNode.Keys.Concat(sightNodes).Where(k => !used.Contains(k))) Console.WriteLine($"! mesh node {k} not found");

Vector3 NodeUE(string name) => Glb.ToUE(g[gun.FindNode(name, under: root)].Translation);

// ---- fit: the receivers onto the M4's
var m4 = File.ReadLines(args[2]).Where(l => l.StartsWith("v ")).Select(l => {
    var p = l.Split(' ', StringSplitOptions.RemoveEmptyEntries);
    return new Vector3(float.Parse(p[1]), float.Parse(p[2]), float.Parse(p[3]));
}).ToArray();
var receiver = meshes["uar"].PositionsOf("Upper").Concat(meshes["uar"].PositionsOf("Lower")).ToArray();
var m4Chamber = new Vector3(4.791f, 0, -0.080f);  // Firearm_Rifle_M4A1_BlockIICQBR "Round Chamber Location"
var uarChamber = NodeUE("Case_5.56_Chamb");
var fit = Matrix4x4.CreateTranslation(m4Chamber - uarChamber);
Console.WriteLine($"start: chambers together, offset {V(m4Chamber - uarChamber)}");
var grid = new Grid(m4, 1.0f);
for (int pass = 0; pass < 2; pass++) {
    bool rotate = pass == 1;
    for (int it = 0; it < 100; it++) {
        var src = new List<Vector3>(); var dst = new List<Vector3>();
        foreach (var p in receiver) {
            var q = Vector3.Transform(p, fit);
            if (grid.Nearest(q, 2.5f) is { } t) { src.Add(q); dst.Add(t); }
        }
        // trim the worst 30%: parts that differ between the two guns (forward assist, ejection port cover)
        var keep = Enumerable.Range(0, src.Count).OrderBy(i => Vector3.DistanceSquared(src[i], dst[i])).Take(src.Count * 7 / 10).ToArray();
        var S = keep.Select(i => src[i]).ToArray(); var D = keep.Select(i => dst[i]).ToArray();
        Matrix4x4 step = rotate ? Kabsch.Fit(S, D) : Matrix4x4.CreateTranslation(Centre(D) - Centre(S));
        fit *= step;
        float moved = step.Translation.Length() + (1 - Math.Abs(Quaternion.CreateFromRotationMatrix(step).W)) * 100;
        if (moved < 1e-4f || it == 99) {
            float rms = MathF.Sqrt(keep.Average(i => Vector3.DistanceSquared(Vector3.Transform(src[i], step), dst[i])));
            var q = Quaternion.CreateFromRotationMatrix(fit);
            Console.WriteLine($"{(rotate ? "rigid" : "translation")} fit: {it + 1} steps, rms {rms:F3} cm over {keep.Length} of {receiver.Length} receiver vertices; " +
                              $"offset {V(fit.Translation)}, turn {2 * MathF.Acos(Math.Min(1, Math.Abs(q.W))) * 180 / MathF.PI:F3} deg");
            break;
        }
    }
}
foreach (var m in meshes.Values) m.Apply(fit);
Vector3 Fitted(string node) => Vector3.Transform(NodeUE(node), fit);

// ---- where the UAR-15's parts are (actor space, cm), for the plugin's numbers
Console.WriteLine("\nUAR-15 points (actor space, cm, fitted):");
foreach (var n in new[] { "FirePoint", "Case_5.56_Chamb", "Case_5.56_Next", "EjectPoint", "ShoulderPoint", "BackPoint", "CenterOfMass", "TP", "SP",
                          "CHARGINGHANDLE", "chargingHandle_gripPoint", "HANDGUARDGRIP", "MAGGRIP", "Handle GRIP", "MUZZLEGRIP", "SightsPrefab", "InsertPoint" })
    try { Console.WriteLine($"  {n,-26} {V(Fitted(n))}"); } catch (Exception e) { Console.WriteLine($"  {n,-26} ({e.Message})"); }
foreach (var (file, m) in meshes.OrderBy(k => k.Key)) {
    var (lo, hi) = m.Bounds();
    Console.WriteLine($"  {file + ".obj",-26} X[{lo.X:F2}..{hi.X:F2}] Y[{lo.Y:F2}..{hi.Y:F2}] Z[{lo.Z:F2}..{hi.Z:F2}]  {m.V.Count} verts, {m.TriCount} tris");
}
Console.WriteLine("\nM4 points for comparison: Fire Location (34.364, 0, 2.086), FireChamber (4.366, 0, 2.175), ChargingHandle (-8.964, 0, 4.544), " +
                  "Magazine Well (4.827, 0, -6.418), SecondaryHandSocket (22.379, 0, 1.854), DefaultStockButtPosition (-36.231, 0, -7.070)");
{
    var (lo, hi) = meshes["uar_muzzle"].Bounds();
    Console.WriteLine($"  muzzle device: rear face X {lo.X:F2}, front X {hi.X:F2}, bore Z {(lo.Z + hi.Z) / 2:F3}");
    var top = meshes["uar"].PositionsOf("Upper").Where(p => p.X > -12 && p.X < 6 && MathF.Abs(p.Y) < 0.5f).Select(p => p.Z).DefaultIfEmpty().Max();
    Console.WriteLine($"  upper receiver rail top Z {top:F2}");
}

// ---- OBJs
foreach (var (file, m) in meshes) File.WriteAllText(Path.Combine(outDir, file + ".obj"), m.ToObj(file));

// ---- textures
var done = new HashSet<string>();
foreach (var mat in gun.Materials()) {
    if (!partOfMaterial.TryGetValue(mat.Name, out var part) || !done.Add(part)) continue;
    Textures.Write(gun, mat, part, metalParts.Contains(part), maxSize, outDir);
}
foreach (var p in parts.Where(p => !done.Contains(p))) Console.WriteLine($"! no material for texture set {p}");
Console.WriteLine($"textures: {string.Join(", ", done)}");

Render.Icon(Path.Combine(outDir, "Icon.png"), meshes.Values, outDir);
if (previewDir != null) {
    Directory.CreateDirectory(previewDir);
    Render.Views(previewDir, meshes, outDir, m4);
}
Console.WriteLine("DONE");
return 0;

static Vector3 Centre(IEnumerable<Vector3> ps) { var a = ps.ToArray(); return a.Aggregate(Vector3.Zero, (s, p) => s + p) / a.Length; }
static string V(Vector3 v) => $"({v.X:F3}, {v.Y:F3}, {v.Z:F3})";

// Nearest-point lookup on a uniform grid.
class Grid {
    readonly Dictionary<(int, int, int), List<Vector3>> cells = new();
    readonly float size;
    public Grid(IEnumerable<Vector3> pts, float size) {
        this.size = size;
        foreach (var p in pts) {
            var k = Key(p);
            if (!cells.TryGetValue(k, out var l)) cells[k] = l = new();
            l.Add(p);
        }
    }
    (int, int, int) Key(Vector3 p) => ((int)MathF.Floor(p.X / size), (int)MathF.Floor(p.Y / size), (int)MathF.Floor(p.Z / size));
    public Vector3? Nearest(Vector3 q, float maxDist) {
        var (x, y, z) = Key(q);
        int r = (int)MathF.Ceiling(maxDist / size);
        Vector3? best = null; float bd = maxDist * maxDist;
        for (int i = -r; i <= r; i++) for (int j = -r; j <= r; j++) for (int k = -r; k <= r; k++)
            if (cells.TryGetValue((x + i, y + j, z + k), out var l))
                foreach (var p in l) { float d = Vector3.DistanceSquared(p, q); if (d < bd) { bd = d; best = p; } }
        return best;
    }
}

// Best rigid transform taking S onto D (Horn's quaternion method: the top eigenvector of a 4x4 symmetric
// matrix, by Jacobi rotations).
static class Kabsch {
    static Vector3 Centre(Vector3[] a) => a.Aggregate(Vector3.Zero, (s, p) => s + p) / a.Length;
    public static Matrix4x4 Fit(Vector3[] S, Vector3[] D) {
        Vector3 cs = Centre(S), cd = Centre(D);
        double sxx = 0, sxy = 0, sxz = 0, syx = 0, syy = 0, syz = 0, szx = 0, szy = 0, szz = 0;
        for (int i = 0; i < S.Length; i++) {
            var a = S[i] - cs; var b = D[i] - cd;
            sxx += a.X * b.X; sxy += a.X * b.Y; sxz += a.X * b.Z;
            syx += a.Y * b.X; syy += a.Y * b.Y; syz += a.Y * b.Z;
            szx += a.Z * b.X; szy += a.Z * b.Y; szz += a.Z * b.Z;
        }
        double[,] n = {
            { sxx + syy + szz, syz - szy, szx - sxz, sxy - syx },
            { syz - szy, sxx - syy - szz, sxy + syx, szx + sxz },
            { szx - sxz, sxy + syx, -sxx + syy - szz, syz + szy },
            { sxy - syx, szx + sxz, syz + szy, -sxx - syy + szz } };
        var (vals, vecs) = Jacobi(n);
        int best = 0; for (int i = 1; i < 4; i++) if (vals[i] > vals[best]) best = i;
        var q = Quaternion.Normalize(new Quaternion((float)vecs[1, best], (float)vecs[2, best], (float)vecs[3, best], (float)vecs[0, best]));
        // x -> R (x - cs) + cd
        return Matrix4x4.CreateTranslation(-cs) * Matrix4x4.CreateFromQuaternion(q) * Matrix4x4.CreateTranslation(cd);
    }
    static (double[], double[,]) Jacobi(double[,] a) {
        int n = 4; var v = new double[n, n]; for (int i = 0; i < n; i++) v[i, i] = 1;
        a = (double[,])a.Clone();
        for (int sweep = 0; sweep < 50; sweep++) {
            double off = 0; for (int p = 0; p < n; p++) for (int q = p + 1; q < n; q++) off += a[p, q] * a[p, q];
            if (off < 1e-20) break;
            for (int p = 0; p < n; p++) for (int q = p + 1; q < n; q++) {
                if (Math.Abs(a[p, q]) < 1e-30) continue;
                double th = (a[q, q] - a[p, p]) / (2 * a[p, q]);
                double t = Math.Sign(th) / (Math.Abs(th) + Math.Sqrt(th * th + 1)); if (th == 0) t = 1;
                double c = 1 / Math.Sqrt(t * t + 1), s = t * c;
                for (int k = 0; k < n; k++) { double akp = a[k, p], akq = a[k, q]; a[k, p] = c * akp - s * akq; a[k, q] = s * akp + c * akq; }
                for (int k = 0; k < n; k++) { double apk = a[p, k], aqk = a[q, k]; a[p, k] = c * apk - s * aqk; a[q, k] = s * apk + c * aqk; }
                for (int k = 0; k < n; k++) { double vkp = v[k, p], vkq = v[k, q]; v[k, p] = c * vkp - s * vkq; v[k, q] = s * vkp + c * vkq; }
            }
        }
        return (new[] { a[0, 0], a[1, 1], a[2, 2], a[3, 3] }, v);
    }
}

class Mesh {
    public List<Vector3> V = new(), N = new();
    public List<Vector2> T = new();
    public List<(string group, int a, int b, int c)> Tris = new();
    public int TriCount => Tris.Count;
    public Vector3[] PositionsOf(string group) => Tris.Where(t => t.group == group).SelectMany(t => new[] { t.a, t.b, t.c }).Distinct().Select(i => V[i]).ToArray();
    public void Apply(Matrix4x4 m) {
        for (int i = 0; i < V.Count; i++) V[i] = Vector3.Transform(V[i], m);
        for (int i = 0; i < N.Count; i++) N[i] = Vector3.Normalize(Vector3.TransformNormal(N[i], m));
    }
    public (Vector3, Vector3) Bounds() => (V.Aggregate(new Vector3(1e9f), Vector3.Min), V.Aggregate(new Vector3(-1e9f), Vector3.Max));
    public string ToObj(string name) {
        var sb = new StringBuilder($"# {name}: UAR-15 (BONELAB Cold Weapon Pack export), Unreal space, cm\n");
        foreach (var v in V) sb.Append($"v {v.X:0.#####} {v.Y:0.#####} {v.Z:0.#####}\n");
        foreach (var t in T) sb.Append($"vt {t.X:0.######} {1 - t.Y:0.######}\n");  // OBJ V runs bottom-up, glTF's top-down
        foreach (var n in N) sb.Append($"vn {n.X:0.#####} {n.Y:0.#####} {n.Z:0.#####}\n");
        foreach (var grp in Tris.GroupBy(t => t.group)) {
            sb.Append($"usemtl {grp.Key}\n");
            foreach (var (_, a, b, c) in grp) sb.Append($"f {a + 1}/{a + 1}/{a + 1} {b + 1}/{b + 1}/{b + 1} {c + 1}/{c + 1}/{c + 1}\n");
        }
        return sb.ToString();
    }
}

record Material(string Name, int? BaseColor, Vector4 BaseColorFactor);

// A binary glTF (.glb) with its buffer and images embedded, as AssetRipper writes them.
class Glb {
    public JsonElement J;
    byte[] bin = [];

    public static Glb Load(string path) {
        var b = File.ReadAllBytes(path);
        int jsonLen = BitConverter.ToInt32(b, 12);
        var g = new Glb { J = JsonDocument.Parse(b.AsMemory(20, jsonLen)).RootElement };
        int at = 20 + jsonLen, binLen = BitConverter.ToInt32(b, at);
        g.bin = b.AsSpan(at + 8, binLen).ToArray();
        return g;
    }
    JsonElement Node(int i) => J.GetProperty("nodes")[i];
    public string NodeName(int i) => Node(i).TryGetProperty("name", out var n) ? n.GetString()! : "";
    public bool HasMesh(int i) => Node(i).TryGetProperty("mesh", out _);
    public int FindNode(string name, int under = -1) {
        int n = J.GetProperty("nodes").GetArrayLength();
        for (int i = 0; i < n; i++) if (NodeName(i) == name && (under < 0 || IsUnder(i, under))) return i;
        throw new Exception($"node {name} not found");
    }
    bool IsUnder(int i, int root) {
        if (i == root) return true;
        int n = J.GetProperty("nodes").GetArrayLength();
        for (int p = 0; p < n; p++)
            if (Node(p).TryGetProperty("children", out var ch) && ch.EnumerateArray().Any(c => c.GetInt32() == i)) return IsUnder(p, root);
        return false;
    }
    Matrix4x4 Local(int i) {
        var n = Node(i);
        var t = n.TryGetProperty("translation", out var tt) ? new Vector3(F(tt[0]), F(tt[1]), F(tt[2])) : Vector3.Zero;
        var r = n.TryGetProperty("rotation", out var rr) ? new Quaternion(F(rr[0]), F(rr[1]), F(rr[2]), F(rr[3])) : Quaternion.Identity;
        var s = n.TryGetProperty("scale", out var ss) ? new Vector3(F(ss[0]), F(ss[1]), F(ss[2])) : Vector3.One;
        return Matrix4x4.CreateScale(s) * Matrix4x4.CreateFromQuaternion(r) * Matrix4x4.CreateTranslation(t);
    }
    static float F(JsonElement e) => e.GetSingle();
    // Every node under `root`, in root's frame (root's own transform left out).
    public Dictionary<int, Matrix4x4> Globals(int root) {
        var d = new Dictionary<int, Matrix4x4>();
        void Walk(int i, Matrix4x4 parent) {
            if (!Node(i).TryGetProperty("children", out var ch)) return;
            foreach (var c in ch.EnumerateArray()) { int k = c.GetInt32(); var m = Local(k) * parent; d[k] = m; Walk(k, m); }
        }
        d[root] = Matrix4x4.Identity;
        Walk(root, Matrix4x4.Identity);
        return d;
    }
    ReadOnlySpan<byte> View(int bufferView, out int stride) {
        var bv = J.GetProperty("bufferViews")[bufferView];
        int off = bv.TryGetProperty("byteOffset", out var o) ? o.GetInt32() : 0;
        stride = bv.TryGetProperty("byteStride", out var st) ? st.GetInt32() : 0;
        return bin.AsSpan(off, bv.GetProperty("byteLength").GetInt32());
    }
    float[] Read(int accessor) {
        var a = J.GetProperty("accessors")[accessor];
        var view = View(a.GetProperty("bufferView").GetInt32(), out int stride);
        int count = a.GetProperty("count").GetInt32(), ct = a.GetProperty("componentType").GetInt32();
        int comps = a.GetProperty("type").GetString() switch { "SCALAR" => 1, "VEC2" => 2, "VEC3" => 3, "VEC4" => 4, var x => throw new Exception(x) };
        int size = ct switch { 5126 or 5125 => 4, 5123 => 2, 5121 => 1, _ => throw new Exception($"componentType {ct}") };
        int start = a.TryGetProperty("byteOffset", out var ao) ? ao.GetInt32() : 0;
        if (stride == 0) stride = size * comps;
        var r = new float[count * comps];
        for (int i = 0; i < count; i++)
            for (int c = 0; c < comps; c++) {
                int at = start + i * stride + c * size;
                r[i * comps + c] = ct switch {
                    5126 => BitConverter.ToSingle(view.Slice(at, 4)), 5125 => BitConverter.ToUInt32(view.Slice(at, 4)),
                    5123 => BitConverter.ToUInt16(view.Slice(at, 2)), _ => view[at] };
            }
        return r;
    }
    public byte[] Image(int textureIndex) {
        int src = J.GetProperty("textures")[textureIndex].GetProperty("source").GetInt32();
        return View(J.GetProperty("images")[src].GetProperty("bufferView").GetInt32(), out _).ToArray();
    }
    public IEnumerable<Material> Materials() {
        foreach (var m in J.GetProperty("materials").EnumerateArray()) {
            var pbr = m.GetProperty("pbrMetallicRoughness");
            var f = pbr.TryGetProperty("baseColorFactor", out var bf) ? new Vector4(F(bf[0]), F(bf[1]), F(bf[2]), F(bf[3])) : Vector4.One;
            int? tex = pbr.TryGetProperty("baseColorTexture", out var t) ? t.GetProperty("index").GetInt32() : null;
            yield return new Material(m.GetProperty("name").GetString()!, tex, f);
        }
    }
    public void AddMesh(int node, Matrix4x4 world, Mesh into, Dictionary<string, string> partOfMaterial) {
        var mats = J.GetProperty("materials");
        foreach (var prim in J.GetProperty("meshes")[Node(node).GetProperty("mesh").GetInt32()].GetProperty("primitives").EnumerateArray()) {
            var at = prim.GetProperty("attributes");
            string matName = mats[prim.GetProperty("material").GetInt32()].GetProperty("name").GetString()!;
            if (!partOfMaterial.TryGetValue(matName, out var group)) { Console.WriteLine($"! {NodeName(node)}: material '{matName}' has no texture set - skipped"); continue; }
            var pos = Read(at.GetProperty("POSITION").GetInt32());
            var nrm = Read(at.GetProperty("NORMAL").GetInt32());
            var uv = Read(at.GetProperty("TEXCOORD_0").GetInt32());
            var idx = Read(prim.GetProperty("indices").GetInt32());
            int baseIndex = into.V.Count;
            for (int i = 0; i < pos.Length / 3; i++) {
                into.V.Add(ToUE(Vector3.Transform(new Vector3(pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]), world)));
                into.N.Add(DirToUE(Vector3.TransformNormal(new Vector3(nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]), world)));
                into.T.Add(new Vector2(uv[i * 2], uv[i * 2 + 1]));
            }
            for (int i = 0; i < idx.Length; i += 3)
                into.Tris.Add((group, baseIndex + (int)idx[i], baseIndex + (int)idx[i + 1], baseIndex + (int)idx[i + 2]));
        }
    }
    public static Vector3 ToUE(Vector3 w) => new(w.Z * 100, -w.X * 100, w.Y * 100);
    static Vector3 DirToUE(Vector3 n) => Vector3.Normalize(new Vector3(n.Z, -n.X, n.Y));
}

static class Textures {
    static float ToLinear(float c) => c <= 0.04045f ? c / 12.92f : MathF.Pow((c + 0.055f) / 1.055f, 2.4f);
    static float ToSrgb(float c) => c <= 0.0031308f ? c * 12.92f : 1.055f * MathF.Pow(c, 1 / 2.4f) - 0.055f;
    static byte B(float x) => (byte)Math.Clamp((int)MathF.Round(x * 255), 0, 255);

    // The model has colour maps only. Normal: flat. ORM: no occlusion, roughness from the colour map's brightness
    // around a per-material base (worn edges come out a little smoother), metallic by part.
    public static void Write(Glb gun, Material m, string part, bool metal, int max, string outDir) {
        using var img = m.BaseColor is int t ? Image.Load<Rgba32>(gun.Image(t)) : new Image<Rgba32>(4, 4, new Rgba32(128, 128, 128, 255));
        if (img.Width > max || img.Height > max) img.Mutate(x => x.Resize(Math.Min(img.Width, max), Math.Min(img.Height, max)));
        using var orm = new Image<Rgba32>(img.Width, img.Height);
        float baseRough = metal ? 0.55f : 0.75f;
        // Metallic only for bare steel (bolt, charging handle). Anodized aluminium, parkerized steel and polymer
        // read as dielectrics; a dark metallic surface reflects almost nothing and looks black in the game's light.
        byte metallic = part is "Bolt" or "Charging" ? (byte)180 : (byte)0;
        // The colour maps are made for Arma's lighting and are very dark in Unreal's: one averaging under 5% linear
        // brightness is brought up to that (at most 6x), keeping its detail and hue.
        double sum = 0; long n = 0;
        img.ProcessPixelRows(rows => {
            for (int y = 0; y < rows.Height; y += 4) {
                var r = rows.GetRowSpan(y);
                for (int x = 0; x < r.Length; x += 4) {
                    var p = r[x];
                    sum += 0.2126 * ToLinear(p.R / 255f) * m.BaseColorFactor.X + 0.7152 * ToLinear(p.G / 255f) * m.BaseColorFactor.Y + 0.0722 * ToLinear(p.B / 255f) * m.BaseColorFactor.Z;
                    n++;
                }
            }
        });
        double mean = n > 0 ? sum / n : 0.1;
        float gain = (float)Math.Clamp(mean > 0 && mean < 0.05 ? 0.05 / mean : 1.0, 1.0, 6.0);
        Console.WriteLine($"  {part,-11} mean linear brightness {mean:F3} -> {mean * gain:F3} (x{gain:F2}), metallic {metallic / 255f:F2}");
        img.ProcessPixelRows(orm, (rows, ormRows) => {
            for (int y = 0; y < rows.Height; y++) {
                var r = rows.GetRowSpan(y); var o = ormRows.GetRowSpan(y);
                for (int x = 0; x < r.Length; x++) {
                    var p = r[x];
                    r[x] = new Rgba32(B(ToSrgb(Math.Min(1f, ToLinear(p.R / 255f) * m.BaseColorFactor.X * gain))), B(ToSrgb(Math.Min(1f, ToLinear(p.G / 255f) * m.BaseColorFactor.Y * gain))),
                                      B(ToSrgb(Math.Min(1f, ToLinear(p.B / 255f) * m.BaseColorFactor.Z * gain))), 255);
                    float lum = (0.2126f * p.R + 0.7152f * p.G + 0.0722f * p.B) / 255f;
                    // The plugin's runtime textures are always sampled as sRGB: store sRGB-encoded values so the
                    // shader reads the linear roughness and metallic meant here (0.55 would otherwise read as 0.26).
                    o[x] = new Rgba32(255, B(ToSrgb(Math.Clamp(baseRough - (lum - 0.2f) * 0.3f, 0.25f, 0.95f))), B(ToSrgb(metallic / 255f)), 255);
                }
            }
        });
        img.SaveAsPng(Path.Combine(outDir, part + "_Diffuse.png"));
        orm.SaveAsPng(Path.Combine(outDir, part + "_ORM.png"));
        using var flat = new Image<Rgba32>(4, 4, new Rgba32(128, 128, 255, 255));
        flat.SaveAsPng(Path.Combine(outDir, part + "_Normal.png"));
    }
}

// A textured, lit render of the converted meshes (orthographic: right side, left side, top), each with the
// M4's points over it in blue: checks the fit, the faces' winding and the UVs before the game sees them.
// Also the spawner icon, drawn like the game's own thumbnails (right side, transparent, 512 x 512).
static class Render {
    static Dictionary<string, Image<Rgba32>> Textures(IEnumerable<Mesh> meshes, string assets) =>
        meshes.SelectMany(m => m.Tris.Select(t => t.group)).Distinct().ToDictionary(p => p, p => Image.Load<Rgba32>(Path.Combine(assets, p + "_Diffuse.png")));

    static float[] Raster(Image<Rgba32> img, IEnumerable<Mesh> meshes, Dictionary<string, Image<Rgba32>> tex, Func<Vector3, Vector3> f,
                          Vector3 eye, float x0, float y0, float s, bool markBack) {
        int w = img.Width, h = img.Height;
        var depth = new float[w * h];
        Array.Fill(depth, float.NegativeInfinity);
        var light = Vector3.Normalize(eye + new Vector3(0.3f, 0, 0.6f));
        foreach (var m in meshes)
            foreach (var (part, a, b, c) in m.Tris) {
                var pa = f(m.V[a]); var pb = f(m.V[b]); var pc = f(m.V[c]);
                Vector2 A = new((pa.X - x0) * s, (pa.Y - y0) * s), B = new((pb.X - x0) * s, (pb.Y - y0) * s), C = new((pc.X - x0) * s, (pc.Y - y0) * s);
                float area = (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X);
                if (MathF.Abs(area) < 1e-6f) continue;
                int minX = Math.Max(0, (int)MathF.Floor(MathF.Min(A.X, MathF.Min(B.X, C.X)))), maxX = Math.Min(w - 1, (int)MathF.Ceiling(MathF.Max(A.X, MathF.Max(B.X, C.X))));
                int minY = Math.Max(0, (int)MathF.Floor(MathF.Min(A.Y, MathF.Min(B.Y, C.Y)))), maxY = Math.Min(h - 1, (int)MathF.Ceiling(MathF.Max(A.Y, MathF.Max(B.Y, C.Y))));
                var t = tex[part];
                for (int y = minY; y <= maxY; y++)
                    for (int x = minX; x <= maxX; x++) {
                        float px = x + 0.5f, py = y + 0.5f;
                        float wa = ((B.X - px) * (C.Y - py) - (B.Y - py) * (C.X - px)) / area;
                        float wb = ((C.X - px) * (A.Y - py) - (C.Y - py) * (A.X - px)) / area;
                        float wc = 1 - wa - wb;
                        if (wa < 0 || wb < 0 || wc < 0) continue;
                        float z = wa * pa.Z + wb * pb.Z + wc * pc.Z;
                        if (z <= depth[y * w + x]) continue;
                        depth[y * w + x] = z;
                        var uv = wa * m.T[a] + wb * m.T[b] + wc * m.T[c];
                        var n = Vector3.Normalize(wa * m.N[a] + wb * m.N[b] + wc * m.N[c]);
                        bool back = Vector3.Dot(n, eye) < -0.2f;  // drawn red: wrong winding or normals
                        int tx = (int)((uv.X - MathF.Floor(uv.X)) * t.Width) % t.Width, ty = (int)((uv.Y - MathF.Floor(uv.Y)) * t.Height) % t.Height;
                        var c0 = t[tx, ty];
                        float lit = 0.45f + 1.4f * MathF.Max(0, Vector3.Dot(n, back ? -n : light));
                        img[x, y] = back && markBack ? new Rgba32(255, 0, 0, 255)
                            : new Rgba32((byte)Math.Min(255, c0.R * lit), (byte)Math.Min(255, c0.G * lit), (byte)Math.Min(255, c0.B * lit), 255);
                    }
            }
        return depth;
    }

    public static void Views(string dir, Dictionary<string, Mesh> meshes, string assets, Vector3[] donor) {
        var tex = Textures(meshes.Values, assets);
        (string name, Func<Vector3, Vector3> f, Vector3 eye)[] views = [
            ("right", p => new(p.X, -p.Z, p.Y), new(0, 1, 0)),
            ("left", p => new(-p.X, -p.Z, -p.Y), new(0, -1, 0)),
            ("top", p => new(p.X, p.Y, p.Z), new(0, 0, 1)),
        ];
        const float s = 12;
        foreach (var (name, f, eye) in views) {
            float x0 = -45, x1 = 55, y0 = -12, y1 = 22;
            if (name == "left") { x0 = -55; x1 = 45; }
            if (name == "top") { y0 = -10; y1 = 10; }
            using var img = new Image<Rgba32>((int)((x1 - x0) * s), (int)((y1 - y0) * s), new Rgba32(235, 235, 235, 255));
            var depth = Raster(img, meshes.Values, tex, f, eye, x0, y0, s, true);
            using var bare = img.Clone();
            foreach (var p in donor) {
                var q = f(p);
                int x = (int)((q.X - x0) * s), y = (int)((q.Y - y0) * s);
                if (x >= 0 && x < img.Width && y >= 0 && y < img.Height && q.Z >= depth[y * img.Width + x] - 0.3f) img[x, y] = new Rgba32(0, 200, 255, 255);
            }
            img.SaveAsPng(Path.Combine(dir, $"fit_{name}.png"));
            bare.SaveAsPng(Path.Combine(dir, $"render_{name}.png"));
        }
        foreach (var t in tex.Values) t.Dispose();
    }

    public static void Icon(string path, IEnumerable<Mesh> meshes, string assets) {
        var list = meshes.ToList();
        var tex = Textures(list, assets);
        var lo = list.Select(m => m.Bounds().Item1).Aggregate(Vector3.Min);
        var hi = list.Select(m => m.Bounds().Item2).Aggregate(Vector3.Max);
        const int size = 512, ss = 4;
        float s = size * 0.86f / (hi.X - lo.X) * ss;
        float cx = (lo.X + hi.X) / 2, cz = (lo.Z + hi.Z) / 2;
        float half = size * ss / 2f / s;
        using var big = new Image<Rgba32>(size * ss, size * ss, new Rgba32(0, 0, 0, 0));
        Raster(big, list, tex, p => new(p.X, -p.Z, p.Y), new(0, 1, 0), cx - half, -cz - half, s, false);
        using var icon = new Image<Rgba32>(size, size);
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) {
                float r = 0, g = 0, b = 0, a = 0;
                for (int j = 0; j < ss; j++)
                    for (int i = 0; i < ss; i++) {
                        var p = big[x * ss + i, y * ss + j];
                        float pa = p.A / 255f;
                        r += p.R * pa; g += p.G * pa; b += p.B * pa; a += pa;
                    }
                icon[x, y] = a > 0 ? new Rgba32((byte)(r / a), (byte)(g / a), (byte)(b / a), (byte)(a / (ss * ss) * 255)) : new Rgba32(0, 0, 0, 0);
            }
        icon.SaveAsPng(path);
        foreach (var t in tex.Values) t.Dispose();
    }
}

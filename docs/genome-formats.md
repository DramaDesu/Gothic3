# Genome binary formats

Derived from the open-source parsers (rmtools/mimicry C++, g3blend Python, g3dit Java)
and verified against the shipping archives. Where a claim was measured, the count is given.


## Getting the bytes

## Container recap (.pak / .p00), verified against F:\Projects\Gothic3\G3Archive (C#) AND against the real files

All Gothic 3 archives are "G3Pak" volumes. Little-endian throughout.

HEADER (48 bytes) — G3Pak_Archive_Header.cs:30-46
  u32 Version          (0 in all retail archives)
  u32 Product          = 0x30563347 ('G3V0'); reject otherwise (line 34)
  u32 Revision         (0)
  u32 Encryption       (0 everywhere — see validation below)
  u32 Compression      (1 = Auto; archive-level default only)
  u32 Reserved
  u64 OffsetToFiles    -> the SINGLE root file-table entry
  u64 OffsetToFolders, u64 OffsetToVolume  (not needed for reading)

FILE-TABLE ENTRY — G3Pak_FileTableEntry.cs:16-31, header in G3Pak_FileTableEntry_Header.cs:23-28
  u64 FileTime1, FileTime2, FileTime3   (create/access/write, Windows FILETIME)
  u32 FileSizeHigh, u32 FileSizeLow     (0 in game data — do NOT use for sizes)
  u32 Attributes
  if (Attributes & 0x10) Directory  -> G3Pak_DirectoryEntry.cs:18-42
        FileString name; u32 DirCount; DirCount x entry; u32 FileCount; FileCount x entry
  else                   File       -> G3Pak_FileEntry.cs:21-32
        u64 Offset; u64 Bytes; u64 Size; u32 Encryption; u32 Compression;
        FileString FileName; FileString Comment

FileString — G3Pak_FileString.cs:32-50: u32 Length; if Length>0 { Length bytes + ONE trailing 0 byte }. Length 0 means "root" and consumes no bytes. Encoding is windows-1252/latin-1.

Enums: G3Pak_Compression.cs — None=0, Auto=1, Zip=2. G3Pak_FileAttribute.cs — Directory=0x10, Compressed=0x800, Deleted=0x8000, Packed=0x20000.

### Gotchas that actually bite
1. **FileName is the FULL relative path**, not a leaf name (G3Pak_FileEntry.ReadFromFile uses Path.GetRelativePath from the archive root; ExtractFile does Path.Combine(Dest, FileName) with a flat Dest — G3Pak_FileTableEntry.cs:110). The directory entry's own name is informational; you can build a flat map from file entries alone and ignore the tree.
2. Read exactly ONE entry at OffsetToFiles — it is the root directory and recurses.
3. **Decompression predicate: `if (Compression == 2) zlib_inflate(raw)`** — a plain zlib stream (0x78 first byte), NOT raw deflate, NOT gzip. I validated all 37,826 entries across _compiledMesh/.p00/_compiledAnimation/_compiledImage/_compiledMaterial/Templates/Projects_compiled/_compiledPhysic: the flags are perfectly consistent — `(comp=0, Bytes==Size, attr&0x800 clear)` 14,952 entries; `(comp=2, Bytes!=Size, attr&0x800 set)` 22,874 entries. Zero anomalies, zero Encryption!=0, zero Deleted entries. So the sniff-for-0x78 heuristic in my reader is dead code; drop it in C++.
4. Bytes = on-disk size, Size = inflated size. Preallocate with Size.
5. Names are mixed-case on disk; index them lowercased and look up case-insensitively (the engine does; e.g. `G3_orc_warrior_body_Diffuse_S10.ximg` sits next to `G3_Orc_Warrior_Body_Diffuse_S1.ximg`).
6. Compression is *per entry*, so you can mmap the .pak and inflate lazily; no whole-archive pass needed.

### .p00 overlay semantics — verified empirically, not assumed
`<name>.pak` plus sibling `<name>.p00`, `.p01`… Later volumes win. Measured:
 - _compiledMesh.p00: 59 files — 41 REPLACE base entries (I md5'd three: same path, different bytes, e.g. g3_items_armor_01/g3_armor_shield_knight_01.xcmsh 21401 B in .pak vs 21436 B in .p00), 18 are NEW (g3_master_collisions_01/*_col.xcmsh).
 - Projects_compiled.p00: 2 new (compiledinfos_g3_world_01.bin, stringtable.bin), 0 overrides.
 - Speech_English.p00: 4 replace, 24 new. Music.p00: 2 new.
So: mount base first, then each .pNN in ascending order, last writer wins on the path key. Loose files under Data\<subdir>\ (this install has empty Data\_compiledImage\_Intern\ and Data\_Intern\ from the patch) should sit on top of that — that is how the community patches ship, but I could NOT confirm the engine's precedence from the SDK: ge_archivemanager.h only exposes GetFile/FindFiles with no mount-order API, so treat loose-file priority as convention, not proven.

### Working code, still present in the scratchpad
- C:\Users\gotow\AppData\Local\Temp\claude\F--Projects\1a4c13ea-b969-468f-b2b4-990e6442110f\scratchpad\pak.py — the original one-shot script (2,249 B). Still works.
- ...\scratchpad\g3pak.py — the better reusable module (`Pak`, `PakSet`, `Entry`); `PakSet` already implements the .p00 overlay. This is the one to port. Verified this session by re-running it over all archives.
- ...\scratchpad\pak_filelist.txt, npc_names.tsv, templates.tsv — all still there.
- ...\scratchpad\list_compiledMesh.txt / list_compiledAnimation.txt / list_compiledImage.txt / list_compiledMaterial.txt — path<TAB>size<TAB>bytes<TAB>comp listings, regenerated and confirmed accurate.

New helpers I wrote this session (all runnable, all in the same scratchpad dir):
- psread.py — generic Genome property-set reader (GENOMFLE header, DEADBEEF-tail string table, sub-class identifier 01 00 01 01 00 01, Property = entry name + entry type + u16 + u32 size + payload). Grounded in g3blend io/genome_file.py, property_sets/{util,property_set,property}.py, binary.py:166-178.
- xactlite.py — minimal .xact walker to reach the MaterialsLoDMappings trailer.
- visanim.py — decodes the eCVisualAnimation_PS body-part slot table.
- resolve_tex.py — material -> .ximg resolution including MaterialSwitch.
- chain.py / final.py / verify.py / extract_set.py — the end-to-end checks.
- ...\scratchpad\milestone1\ — the 16 files of the asset set below, already extracted and byte-verified.

CRITICAL for a C++ port: `bCString` and every `*ResourceString` property is NOT an inline string. It is a u16 INDEX into the file's string table (g3blend io/property_types/string.py:23 -> binary.py:166-171). The string table lives AFTER the 4-byte DEADBEEF marker at the offset stored at file byte 10. Files that lack the GENOMFLE wrapper (499 of the 1150 .xshmat!) have no string table, and then the same fields are inline u16-length strings. You must support both paths.


## Dependency chain

## Full chain, each hop grounded and each one actually executed this session

### 1. Template -> visual  (Templates.pak/*.tple)
A .tple is a Genome property-set file. The NPC's visual is `eCVisualAnimation_PS`. Its property list matches gothic3sdk/g3/Engine/include/g3sdk/Engine/animation/ge_visualanimation_ps.h:151-187 exactly, in order.

  `eCVisualAnimation_PS.ResourceFilePath` : bCAnimationResourceString

Two shapes, both real:
 - MONSTERS name the finished actor directly. wolf_bogir_wolf.tple -> `G3_Wolf_Body_01.FXA`, 0 body-part slots.
 - HUMANOIDS name the bare SKELETON and compose parts. orcs_orc_warrior_01.tple -> `G3_Orc_Skeleton.FXA`; humans_master_pc_hero.tple -> `G3_Hero_Skeleton.FXA`.

**`.FXA` in the data means `.xact` on disk.** No .fxa file exists anywhere; substitute the extension and look up in _compiledAnimation.pak (flat, no folders). Casing varies inside one file ('G3_Orc_Skeleton.FXA' as the property, 'G3_Orc_Skeleton.fxa' in the slot table) — compare case-insensitively.

### 2. Body-part slots — post-version data of eCVisualAnimation_PS
NOT a property; it lives in the property set's tail between the class version and the DEADCODE marker. Layout from g3dit/LrentNode/.../classes/eCVisualAnimation_PS.java:28-55 and MaterialSwitchSlot at :108-121. I decoded orcs_orc_warrior_01.tple byte-by-byte against that and it lands exactly on the DEADCODE:

  u16 classVersion (64); skip 2 (always 05 00)
  fxaSlot: MaterialSwitchSlot WITHOUT a name  { skip u16(=4); entry fxaFile; u32 fxaSwitch; u8 hasAlt; if(hasAlt){ skip u16; entry fxaFile2; u32 fxaSwitch2 } }
  u32 slotCount; slotCount x { MaterialSwitchSlot WITH a leading entry name; then u8(=1) }
  u8(=1); u32 stCount; stCount x entry
  u32 attachmentCount; each { skip u16; u8 hasGuid; if so 20-byte guid; entry slotName; 72 raw bytes (transform) }
  bCBox boundary (6 floats)

Measured for orcs_orc_warrior_01.tple:
  fxaSlot          -> G3_Orc_Skeleton.fxa           switch 0
  Slot_Head        -> G3_Orc_Head_Head01.fxa        switch 6   (alt: G3_Orc_Head_Head01_Animated.fxa, switch 6)
  Slot_Body        -> G3_Orc_Body_Warrior.fxa       switch 2
  boundary         -> min(-1,-1,-1) max(1,1,1)   [the wolf's is a real AABB: min(-29.7,-2.1,-141.9) max(29.6,120.3,115.2)]
And for humans_master_pc_hero.tple: Slot_Head -> G3_Head_Hero_Hero_01.fxa (0), Slot_Body -> G3_Hero_Body_Player.fxa (0).
The "alt" second file is the facially-animated variant, used for dialogue.

### 3. Visual -> mesh + skeleton  (the .xact IS both)
There is no separate mesh file for characters. Character body meshes are NOT in _compiledMesh.pak — I searched it: 'G3_Orc_Body', 'Hero_Body_Player' and 'Hero_Skeleton' return zero .xcmsh hits. _compiledMesh.pak is world/props/items only.

A .xact = GENOMFLE wrapper + eCResourceAnimationActor_PS (version 54) + one or more embedded EMotionFX 2 actors ('gena' magic, 'FXA ' tag, v1.1):
  u16 resVersion; u32 resSize; f32 prio; u64 filetime; u32 nativeSize; bCBox bbox (usually FLT_MAX sentinel = invalid)
  u32 lookAtCount; each { entry nodeName; f32 speed; vec3 min; vec3 max }
  u32 lodCount; lodCount x (actor + trailer); then the main actor + trailer
Chunk ids seen: 0 NODE, 3 MESH, 4 SKINNINGINFO, 6 MATERIAL, 7 MATERIALLAYER, plus SCENE_INFO, LIMIT, PHYSICSINFO.
NODE chunk = pos vec3, rot quat, scaleOrient quat, scale vec3, shear vec3, u32-length name, u32-length parent name -> this IS your bind pose, and the parent link is BY NAME, not index.

### 4. Mesh -> material  (the trailer, not the MATERIAL chunk)
The in-actor MATERIAL chunks carry Maya-ish names ('EMFX Default', 'G3_Animation_Monster_Wolf_01_A|', 'monster_zaehne') with no usable file reference. The real link is the **MaterialsLoDMappings** table right after the actor's chunk stream:
  u32 count; count x { u16 lodIndex; u16 matIndex; entry name }
matIndex is the submesh's material id; name is the .xshmat FILE NAME. Confirmed by g3dit eCResourceAnimationActor_PS.java:44-68 ("MaterialsLoDMappings: MaterialID -> MaterialReference") and read live:
  G3_Orc_Body_Warrior.xact -> [(0, 1, 'G3_Animation_Orc_Warrior_01.xshmat')]
  G3_Orc_Head_Head01.xact  -> [(0,1,'Orc_Face.xshmat'), (0,2,'Orc_beard.xshmat')]
  G3_Wolf_Body_01.xact     -> [(0,1,'G3_Animation_Monster_Wolf_01_A.xshmat'), (0,3,'monster_zaehne.xshmat')]
Look the name up in _compiledMaterial.pak (flat). Only slots that appear in this table are drawn; the wolf has 4 MATERIAL chunks but only 2 mappings.
After the mappings comes: u8 prefix; u32 aoArrayCount; each { u8; u32 n; n x u32 AO } ; then n x 12-byte tangent per array.

### 5. Material -> image  (.xshmat -> .ximg)
.xshmat = eCResourceShaderMaterial_PS holding one eCShaderBase subtree (g3blend property_sets/resource_shader_material.py:12-17). Shader classes seen on characters: eCShaderSkin (body/face) and eCShaderDefault (teeth/beard). Each colour channel is an eCColorSrcProxy pointing at eCColorSrcSampler / eCColorSrcConstant / eCColorSrcCombiner / eCColorSrcBlend.
The texture name is `eCColorSrcSampler.ImageFilePath` (type bCImageResourceString) and it names a **.tga**, e.g. 'G3_Orc_Warrior_Body_Diffuse_S1.tga'. There is no .tga in the shipped data — you strip the path, strip the extension, append `.ximg`, and look up in _compiledImage.pak.
I verified the basename namespace is GLOBALLY UNIQUE: 1,902 entries, 1,902 distinct basenames, 0 collisions. So a flat lowercase basename->path map is a correct resolver even though _compiledImage.pak has folders (animation/, level/, items/, effects/, speedtree/, sky/, _special/, _Intern/, testlevel/, masterhuman/, editsupporter/).

**MaterialSwitch (skin variants) — do not skip this, or your orc is the wrong colour.**
Rule from g3dit XshmatLoader.java:176-233, enum values from gothic3sdk EngineEnum.h:121-126 (Repeat=0, Clamp=1, PingPong=2):
  if name ends with "_s1" (case-insensitive) AND materialSwitch != 0:
      base = name minus the final char        // "..._S"
      count = number of consecutive existing base+1, base+2, ... .ximg
      Repeat  : idx = switch % count
      Clamp   : idx = clamp(switch, 0, count-1)
      PingPong: idx = switch % count; if (idx & 1) idx = count - idx - 1
      name = base + (idx+1)
  load name + ".ximg"
The switch is **0-based**. Which switch applies: the per-slot `fxaSwitch` for that body part, and the `eCVisualAnimation_PS.MaterialSwitch` int property for the top-level fxaSlot (g3dit CheckFaultyBodyParts.java:74-94 asserts those two agree). Worked examples I ran: orc body switch 2, 10 diffuse variants -> _S3. Orc face switch 6, 22 variants -> _S7. Orc beard switch 6, 2 variants -> _S1. Wolf MaterialSwitch=3, 5 variants -> _S4. Hero switch 0 -> no switching at all, keeps the authored _S1/_S3.
Normal maps usually have only an _S1, so count=1 and any switch collapses to _S1 — this is why the rule must count real files rather than blindly substituting.

.ximg itself: GENOMFLE wrapper, then a block with magic 'G3IMG', a variable-length source-path string (so field offsets shift between files — do not hardcode), a 4-char FourCC ('DXT1' observed on all five orc textures), and the total payload size, followed by the raw compressed mip chain, then DEADBEEF + an empty string table. Payload sizes match textbook mip chains exactly: 0x000AAAAA = 699,050 = DXT1 1024x1024 full chain; 0x0002AAAA = 174,762 = DXT1 512x512. Exact field offsets are the format agent's job — I only confirmed enough to size the data.

### 6. Motions -> actor  (BY BONE NAME — there is no list anywhere)
Nothing binds a motion to an actor explicitly. I checked both ends:
 - the .xact holds no motion list (eCResourceAnimationActor_PS = box + lookAt + LODs + actor + material mappings + AO/tangents; nothing else),
 - the .xmot holds no actor reference (eCResourceAnimationMotion_PS = version/size/prio/filetimes + frame effects + an LMA v1.1 motion, per g3dit eCResourceAnimationMotion_PS.java:86-96).

The binding is: each MotionPart chunk carries a u32-length NAME, and you match it against the actor's NODE names. Parts with no matching node are silently dropped. Confirmed by running it: Orc walk 60/64 parts bind to G3_Orc_Body_Warrior; Wolf walk 43/44. Unmatched parts are 3ds Max rig residue ('!FH_..._NEWIKGoalHelper', '!FH_..._GradientRotation', 'Dummy01', 'Object01', 'Particle View 01') plus weapon props. `!DMW_<Species>_BaseMotion_Spline` appears in every locomotion motion and in no actor — strongly suggests the root-motion/displacement track, but I did NOT verify that, flagging it for whoever does .xmot.

Discovery is by FILENAME CONVENTION. .xmot files are flat at the root of _compiledAnimation.pak, named:
  <Species>_<Stance>_<LeftHand>_<Weapon>_<P?>_<Group>_<Action>_<Flag>_<Direction>_<var>_%_<varCount>_<P?>_<speed>[_L].xmot
e.g. Orc_Stand_None_Fist_P0_Move_Walk_N_Fwd_00_%_00_P0_200.xmot
The first token is the species/skeleton family and it matches the bone-name prefix (Orc_* bones <-> Orc_* motions; Hero_, Wolf_, Fat_, Slave_ likewise). Filter on `startswith("Orc_")` to get an actor's motion set. Field 6/7 is the action (Move_Walk, Move_Run, Move_Stand, Move_Turn90, Ambient_Loop, Attack_Hit, Die_Begin, ...); the last numeric field is the authored ground speed in cm/s (Walk 200, Run 400 for the orc). I did NOT reverse the runtime rule that picks a specific motion from gameplay state — that is Game.dll logic and irrelevant to a bring-up milestone; just load the file by name.

Caveat worth flagging: dumping a motion part prints identical denormal garbage for the bind_pose_position/rotation/scale fields across every part. g3blend's chunks.py:360-367 uses the same layout, so this is most likely genuinely-unused data in the files rather than a misparse, but I have not proven that — use the pose_* fields, and let the .xmot agent settle it.

### Dead end worth knowing
`G3_Hero_Head_Player.xact` maps to `lambert2SG.xshmat`, which points at BLOODFLY textures. It is junk left in the build. The player's real head is `G3_Head_Hero_Hero_01.xact` -> G3_Human_HERO_Face_Diffuse_01_A/B.xshmat -> G3_Animation_Head_Hero_01_S1 + _normal_S1 + eyes. Always come at heads through the template's Slot_Head, never by guessing the name.


## First character asset set

G3_Orc_Body_Warrior is the best first target: it is humanoid, iconic, and — the decisive property — the body .xact is SELF-CONTAINED. I diffed its node names against G3_Orc_Skeleton.xact: the body's 123 nodes are a strict SUPERSET of the skeleton's 112 (skel-only = EMPTY SET), adding only the mesh root 'OrcWarrior' and 11 equipment attachment points (Slot_RightHand_Weapon, Slot_Bow, Slot_AxeBack, ...). So for milestone 1 you load ONE .xact, get bind pose + skinned mesh + skinning weights + material names in a single file, and play the motions straight on it. The skeleton .xact is only needed later, for the ragdoll (PHYSICSINFO x17, LIMIT x53) and the 4 look-at constraints, which the body file does not carry. Mesh: 5,276 original verts / 8,176 after duplication, 30,618 indices, ONE submesh, ONE UV set, max 9 influences per vertex. I verified motion binding by name: the walk motion has 64 parts, 60 of which resolve to real bones (93%); the 4 that do not are Max rig leftovers ('!DMW_Orc_BaseMotion_Spline' — almost certainly the root-motion spline, 'Ork_face', 'G3_Weapon_Orc_Axe_02', 'OrcBoss') and must simply be skipped. The idle binds 56/66 (84%); the unbound ones are '!FH_*_NEWIKGoalHelper'/'GradientRotation' helpers, Dummy01/Dummy02, Object01, 'Particle View 01', plus toe bones the orc body does not have. Wolf (G3_Wolf_Body_01.xact, 85 nodes, 97-100% motion binding, one file, no head slot) is the even simpler fallback if you want a quadruped first — but it is not humanoid. Textures below already have MaterialSwitch applied (see dependency_chain); everything listed here has been extracted, inflated and byte-checked into ...\scratchpad\milestone1\. Total 2.77 MB uncompressed, 16 files.


**mesh files**

- `_compiledAnimation.pak :: G3_Orc_Body_Warrior.xact  (647,688 B raw / 345,025 B packed) - contains BOTH the skinned mesh and the full 123-node bind-pose skeleton; this is the only file needed for stage 1`
- `_compiledAnimation.pak :: G3_Orc_Head_Head01.xact  (207,109 B raw) - OPTIONAL stage 2. 113 nodes, mesh 2,251 verts / 11,046 indices, 2 submeshes. Attaches at Slot_Head.`


**skeleton files**

- `_compiledAnimation.pak :: G3_Orc_Skeleton.xact  (21,996 B raw) - the master skeleton the template actually names. 112 nodes, ZERO meshes, 53 LIMIT + 17 PHYSICSINFO chunks (ragdoll) and 4 look-at constraints on Orc_Head_Neck_1/2/3 and Orc_Head_Head_1. NOT required to render or animate in stage 1 (the body actor already carries every one of these 112 bones); needed for ragdoll and head-tracking later.`
- `(note) root nodes in G3_Orc_Body_Warrior.xact are 'Orc_ROOT' (the skeleton root) and 'OrcWarrior' (the mesh node); both have an empty parent string.`


**motion files**

- `_compiledAnimation.pak :: Orc_Stand_None_Fist_P0_Move_Stand_N_Fwd_00_%_00_P0_0.xmot  (7,910 B raw / 2,985 packed) - locomotion idle, 66 parts, 56 bind. Speed field = 0.`
- `_compiledAnimation.pak :: Orc_Stand_None_Fist_P0_Move_Walk_N_Fwd_00_%_00_P0_200.xmot  (22,841 B raw / 11,871 packed) - forward walk, 64 parts, 60 bind (93%). Trailing 200 = authored ground speed in cm/s.`
- `_compiledAnimation.pak :: Orc_Stand_None_Fist_P0_Move_Run_N_Fwd_00_%_00_P0_400.xmot  (18,221 B raw / 9,240 packed) - forward run, 64 parts, 60 bind. Speed 400.`
- `_compiledAnimation.pak :: Orc_Stand_None_Fist_P0_Ambient_Loop_N_Fwd_00_%_00_P0_0.xmot  (31,362 B raw / 17,689 packed) - breathing idle, 66 parts, 56 bind (84%).`


**material files**

- `_compiledMaterial.pak :: G3_Animation_Orc_Warrior_01.xshmat  (2,204 B) - shader eCShaderSkin. This is the ONLY material the body actor needs (MaterialsLoDMappings has one entry: lod=0, slot=1).`
- `_compiledMaterial.pak :: Orc_Face.xshmat  (2,188 B) - eCShaderSkin, head slot 1. Stage 2.`
- `_compiledMaterial.pak :: Orc_beard.xshmat  (1,527 B) - eCShaderDefault, head slot 2. Stage 2. NOTE: this file has NO GENOMFLE wrapper (starts 01 00 01 01) and therefore no string table - its strings are inline. 499 of 1,150 .xshmat are like this.`


**texture files**

- `_compiledImage.pak :: animation/G3_Orc_Warrior_Body_Diffuse_S3.ximg  (699,160 B) - diffuse. NOT S1: the material names _S1 but the template's Slot_Body carries fxaSwitch=2, and the Repeat rule gives index 2 -> _S3.`
- `_compiledImage.pak :: animation/G3_Orc_Warrior_Body_normal_S1.ximg  (349,648 B) - normal map. Stays S1 because only one variant exists, so count=1 and 2%1=0.`
- `_compiledImage.pak :: animation/G3_Orc_Face_Diffuse_S7.ximg  (174,872 B) - head diffuse. fxaSwitch=6 on Slot_Head, 22 variants exist, 6%22=6 -> _S7. Stage 2.`
- `_compiledImage.pak :: animation/G3_Orc_Face_normal_S1.ximg  (349,648 B) - head normal, only S1 exists. Stage 2.`
- `_compiledImage.pak :: animation/G3_Orc_Beard_Diffuse_S1.ximg  (349,648 B) - beard diffuse; 2 variants, 6%2=0 -> _S1. Stage 2.`
- `(context) Templates.pak :: npc/_npcs/orcs_orc_warrior_01.tple  (16,136 B) - not needed to render, but this is the file that ties the whole set together; keep it to drive the composition.`


## Scale

## Scale of the data (measured this session by walking every file table)

Archive-level. All are G3V0 with Version=0, Revision=0, Encryption=0, Compression=1(Auto).

| archive | on disk | files | packed | inflated | zlib entries |
|---|---|---|---|---|---|
| _compiledMesh.pak | 469.9 MB | 6,282 | 446.7 MB | 886.4 MB | 4,713 |
| _compiledMesh.p00 | 4.8 MB | 59 (41 override + 18 new) | | | |
| _compiledAnimation.pak | 153.2 MB | 6,324 | 144.5 MB | 252.9 MB | 6,213 |
| _compiledImage.pak | 611.8 MB | 1,902 | 583.0 MB | 929.4 MB | 1,863 |
| _compiledMaterial.pak | 2.7 MB | 1,151 | 2.3 MB | 2.4 MB | 18 |
| Templates.pak | 21.9 MB | 7,273 | 19.1 MB | 40.0 MB | 4,047 |
| Projects_compiled.pak (+.p00 6.5 MB) | 180.4 MB | 8,100 (+2) | 169.3 MB | 509.1 MB | 1,471 |
| _compiledPhysic.pak | 112.2 MB | 6,735 | 105.6 MB | 161.9 MB | 4,501 |

Total for the 8 above: ~1.56 GB on disk, ~2.78 GB inflated, 37,826 file entries. The whole Data\ dir is ~3.5 GB, the rest being Speech_*.pak (699 MB EN + 963 MB RU, 48,476 entries in EN), Music.pak 187 MB, Sound.pak 87 MB, gui.pak 80 MB, Lightmaps.pak 138 MB.

### What lives where

**_compiledMesh.pak** — 5,392 .xcmsh (835.0 MB inflated, avg 162 KB), 782 .xnvmsh (51.4 MB), 108 .xlmsh (0.03 MB, avg 294 B). Static/world geometry ONLY — no character bodies. Foldered by asset pack; biggest folders G3_World_Sea_01 (987), G3_Varant_Landscape_01 (720), G3_Myrtana_Landscape_01 (606), G3_Objects_Myrtana_Misc_01 (418), G3_World_Depthmesh_01 (417). Items are G3_Items_Armor_01/, G3_Items_Weapons_Melee_Swords_01/ etc. The .xnvmsh here are landscape LOD cells (G3_*_Landscape_01/LOD/*_Cell_NNN.xnvmsh); the .xlmsh are tiny per-building lightmap meshes.

**_compiledAnimation.pak** — FLAT, no folders. 387 .xact (110.8 MB inflated, avg 300 KB) + 5,937 .xmot (142.1 MB, avg 25 KB).
 .xact naming: `G3_<Species>_<Part>_<Variant>[_LODn].xact`. Four bare skeletons: G3_Hero_Skeleton, G3_Orc_Skeleton, G3_Fat_Skeleton, G3_Slave_Skeleton. Bodies: G3_Hero_Body_* (44 of them: Player, Paladin, Mercenary, Nomad, Ranger, Skeleton, Xardas, ...), G3_Orc_Body_{Boss,Elite,Scout,Shaman,Warrior}, G3_Slave_Body_*, G3_Fat_Body_*. Heads: G3_Hero_Head_Head01..15 (+_LOD1/_LOD2 each), G3_Hero_Head_{Diego,Gorn,Lee,Lester,Milten,Thorus,Xardas,Zuben,Rhobar,Saturas}, G3_Head_Hero_*_Animated_* (dialogue variants), G3_Head_Hero_Myrtana_01..09, NordmarHead01..04, OrientHead01..06. Creatures are single files: G3_{Wolf,Boar,Troll,Ogre,Demon,Dragon,Golem,Lurker,Scavenger,Snapper,Sabertooth,Shadowbeast,Skeleton,Waran,TRex,Rhino,ScorpionKing,Minecrawler,Gargoyle,Meatbug,...}_Body_NN.xact. Also 24 bows/crossbows and a handful of animated props (chests, doors, grindstone).
 .xmot per species: Hero 2,753, Orc 927, Slave 204, Fat 182, Snapper 119, Goblin 118, Troll 89, Ogre 83, Demon 83, Stalker 81, Minecrawler 81, Golem 81, Lurker 71, Scavenger 68, Wolf 67, Sabertooth 65, Rhino 60, Shadowbeast 56, Waran 55, TRex 55, Ripper 55, Alligator 54, Bison 52, ScorpionKing 51, Boar 51, Dragon 42, Gargoyle 40, Bloodfly 30, Cow 27, Pig 25, Deer 23, Meatbug 17, and single digits for Fish/Seagull/Chicken/Lizard/Snake/Rabbit/Turtle/Vulture.
 Per-species locomotion subset is tiny: the orc's Stand/Fist set is 12 files (Ambient_Loop, Move_Stand, Move_Walk x4 dirs, Move_Run x4 dirs, Move_Turn90 x2). Most common actions across Hero: Move_Walk 218, Ambient_Loop 182, Move_Run 158, Move_Turn90 96.

**_compiledImage.pak** — 1,897 .ximg (928.8 MB inflated, avg 513 KB) + 4 .tga + 1 .dds. Foldered: level/ 1,063, animation/ 484 (this is where ALL character textures live), effects/ 99, items/ 80, speedtree/ 53, _special/ 49, _Intern/ 28, testlevel/ 21, sky/ 13, masterhuman/ 2, editsupporter/ 4, 6 at root. Suffix convention: `_Diffuse_S<n>` (935+29), `_Normal_S<n>` (350+62), `_Specular_S<n>` (232+4), `_Alpha_` (5). Typical inflated sizes: 699,160 B = DXT1 1024², 349,648 B = 512² DXT5-class, 174,872 B = DXT1 512². Basenames are globally unique across folders (1,902/1,902).

**_compiledMaterial.pak** — FLAT. 1,150 .xshmat (2.3 MB total, avg 2.1 KB) + _ShaderCacheMaterialList.ini (53 KB). 651 are GENOMFLE-wrapped with a string table, 499 are bare property sets with inline strings — you must handle both. Names are free-form artist names, no scheme: 'G3_Animation_Orc_Warrior_01.xshmat', 'Hero_player.xshmat', 'monster_zaehne.xshmat', 'lambert2SG.xshmat', '01 - Default.xshmat', 'BEARD.xshmat'.

**Templates.pak** — 6,295 .tple (39.4 MB, avg 6.6 KB) + 328 .lrtpldatasc + 325 .lrtpl + 325 .lrtpldat. Foldered NPC/<Region>/ (Myrtana, Nordmar, Varant, _NPCs, _Monster, __Master_Humans, ZTest_NPC), plus Editor/, Items/ etc. An NPC .tple carries ~20 property sets: gCNavigation_PS, eCRigidBody_PS, eCCollisionShape_PS + 2x eCCollisionShape, gCCharacterMovement_PS (36 props incl. ForwardSpeedMax/StepHeight/MoveAcceleration), gCNPC_PS (43), gCInventory_PS + gCInventorySlots, gCScriptRoutine_PS, gCInteraction_PS, gCDamage_PS, gCDamageReceiver_PS, gCFocus_PS (80 props!), gCDialog_PS, eCIlluminated_PS, gCParty_PS, gCEffect_PS, eCVisualAnimation_PS (31). 24 templates reference G3_Orc_Body_Warrior, 19 reference G3_Wolf_Body_01, 2 reference G3_Hero_Body_Player.

**_compiledPhysic.pak** — 6,735 .xnvmsh (161.9 MB), collision meshes, one per world/prop mesh.

### Milestone-1 footprint
One orc, body only: 1 .xact (648 KB) + 1 .xshmat (2 KB) + 2 .ximg (1,049 KB) + 4 .xmot (80 KB) = **1.74 MB inflated, 8 files**. With the head: 2.77 MB, 16 files. Already extracted and verified at C:\Users\gotow\AppData\Local\Temp\claude\F--Projects\1a4c13ea-b969-468f-b2b4-990e6442110f\scratchpad\milestone1\.
If you want every playable humanoid + creature ever: the 387 .xact are 110.8 MB and the 5,937 .xmot are 142.1 MB — the entire character animation corpus fits in ~253 MB of RAM uncompressed. Character textures (animation/ folder, 484 files) are the real cost driver.


## Container

ALL of .xcmsh / .xshmat / .xlmsh / .xnvmsh (and the outer wrapper of .ximg) are "Genome property-set" files. Little-endian throughout. Strings are windows-1252, NOT NUL-terminated.

=== A. OPTIONAL "GENOMFLE" WRAPPER + STRING TABLE ===
Sniff the first 8 bytes. Two flavours coexist in the same archive with the same extension (measured: 3350 wrapped / 2042 raw out of 5392 .xcmsh; 651/349 out of 1000 .xshmat):

 (a) WRAPPED:
   0x00 char[8]  "GENOMFLE"
   0x08 u16      version == 1
   0x0A u32      stringTableOffset (absolute)
   0x0E ...      payload (a property set) ...
   @stringTableOffset:
     u32  0xDEADBEEF  (bytes EF BE AD DE)
     u8   hasStringTable
     if hasStringTable: u32 count; count x { u16 len; char[len] }
   Read the string table FIRST, then parse the payload.
 (b) RAW: payload starts at offset 0, no string table.

readEntry() = stringTable ? stringTable[readU16()] : (u16 len + chars).
Class names, property names, property TYPE names, submesh material names and texture paths all go through readEntry(). This is the single biggest trap: in wrapped files a "string" is 2 bytes.
(g3blend io/genome_file.py:6-39 + io/binary.py:166-178; g3dit LrentNode/.../util/ClassUtil.java:33-45)

=== B. PROPERTY-SET HEADER (identical for every class) ===
   u8[6]  01 00 01 01 00 01              // SUB_CLASS_IDENTIFIER, constant
   entry  className                      // "eCResourceMeshComplex_PS", ...
   u8     1
   u16    0
   u16    psVersion                      // observed 1, 81, 82, 83
   u16    psVersion                      // written twice; readers call the 2nd "objectVersion"
   u32    sizeToDeadCode                 // declaredEnd = tell() + sizeToDeadCode
   if psVersion < 81: entry objectName    // e.g. "G3_Object_Barrel_01"
   if psVersion < 82: u8[20] guid         // 16-byte GUID + u32 validFlag (valid iff low byte != 0)
   u16    propertyVersion                 // always 30 (0x1E) in shipping data
   u32    propertyCount
   propertyCount x { entry name; entry typeName; u16 30; u32 valueSize; u8[valueSize] value }
   u16    classVersion
   ... class-specific body, ends exactly at declaredEnd ...
ALWAYS seek to declaredEnd after a set rather than assuming you consumed it (see gotchas).
(g3blend io/property_sets/property_set.py:24-38 and property.py:282-287; g3dit G3Class.java:77-99; mimicry mi_xcmshreader.cpp:36-53)

Property value encodings measured on _compiledMaterial.pak: bool=1, char=1, float=4, bCVector2=8, bCVector=12, bCBox=24 (min xyz, max xyz), bCFloatColor=16 (u32 vftable + 3 floats RGB, NO alpha), bTPropertyContainer<enum X>=6 (u16 version=1 + u32 value), bCString / bC*ResourceString = one entry.

=== C. eCResourceBase_PS SUB-BODY (prefix of every resource body) ===
   u16 resourceVersion                   // 30 (0x1E) everywhere in shipping data
   if resourceVersion >= 0x17: u32 size  // engine hint, not a file offset
   if resourceVersion <  0x1E: { u16 t; if t > 1: u8 }
(g3blend property_sets/resource_base.py:99-104; g3dit eCResourceBase_PS.java:167-179)

=== D. HOW TO SKIP TO THE PAYLOAD ===
.xcmsh/.xshmat/.xlmsh: there is no "skip to payload" — you must walk the header above, because the property blob sizes and the string table are what make the offsets resolvable. .ximg is the exception: its payload is a fixed 87 bytes from file start (verified on all 1897 files).

Archives: .pak files are multi-volume (a .p00 sibling is a SECOND volume with its own 48-byte header and its own file table; _compiledMesh.p00 holds 59 further entries). Header: u32 version, product, revision, encryption, compression, reserved, then u64 offFiles, offFolders, offVolume at +24; the directory tree at offFiles is 24 bytes of FILETIMEs + 8 size + u32 attrs, then either (dir) name + child counts or (file) u64 offset, u64 storedBytes, u64 rawSize, u32 enc, u32 comp, name, comment; per-entry zlib when comp != 0. (F:\Projects\Gothic3\G3Archive\src\classes\*; working C++ implementation already in F:\Projects\Gothic3\runtime\src\genome\pak.cpp)


## eCResourceMeshComplex_PS body (.xcmsh)

Confidence: high. Source: `F:\Projects\Gothic3\g3blend\g3blend\io\property_sets\resource_mesh_complex.py:74-81 (cross-checked against F:\Projects\Gothic3\rmtools\mimicry\source\Mimicry\mi_xcmshreader.cpp:54-74 and my scan script C:\Users\gotow\AppData\Local\Temp\claude\F--Projects\1a4c13ea-b969-468f-b2b4-990e6442110f\scratchpad\mesh\scan2.py)`

```
After the property-set header (className "eCResourceMeshComplex_PS", classVersion 34 or 35 — 5346x v35, 46x v34):
  <eCResourceBase_PS sub-body: u16 resourceVersion=30; u32 size>
  f32  resourcePriority            (0.0 in every shipping file)
  u32  meshElementCount            (1..26; 3903 files have exactly 1)
  meshElementCount x eCMeshElement
The two declared properties are always exactly: BoundingBox (bCBox, 24 bytes = f32[3] min, f32[3] max) and ResourcePriority (float). The whole-mesh AABB is the BoundingBox property; per-submesh AABBs live inside each eCMeshElement.
classVersion < 34 is a legacy layout that is only a list of names (mimicry mi_xcmshreader.cpp:63-68) — does not occur in shipping data.
There is NO LOD chain inside .xcmsh. LODs are a separate .xlmsh file.
VERIFIED: my independent parser consumed exactly 5392/5392 .xcmsh in _compiledMesh.pak, ending byte-exactly at the string-table offset (wrapped) or EOF (raw).
```


## eCMeshElement (the submesh / draw-call unit)

Confidence: high. Source: `F:\Projects\Gothic3\g3dit\LrentNode\src\main\java\de\george\lrentnode\structures\eCMeshElement.java:342-418 (identical in F:\Projects\Gothic3\g3blend\g3blend\io\structs\mesh_element.py:315-353; the trailing blocks also match F:\Projects\Gothic3\rmtools\mimicry\source\Mimicry\mi_xcmshreader.cpp:144-150)`

```
u16    version          // 2,3,4,5 observed (8247x v5, 903x v4, 129x v3, 7x v2)
u32    fvf              // D3D9 FVF bitmask, see below
f32[3] bboxMin
f32[3] bboxMax          // 24 bytes total, submesh-local AABB. VERIFIED: every sampled vertex lies inside it.
u32    size             // engine memory-footprint hint, NOT a byte count in the file. DO NOT SEEK WITH IT.
entry  materialName     // e.g. "G3_Objects_Barrelmetal_01_A.xshmat" (or ".xmat" for 41 dead refs)
u32    streamCount      // 3..10
streamCount x eCVertexStructArray  (see next structure)
// ---- trailing per-element blocks, all skippable ----
if version >= 3:   // eSLightmapPerVertexGroup { bTValArray<u32>; bTValArray<u32>; }
    u8 1; u32 n1; u32[n1]
    u8 1; u32 n2; u32[n2]
if version >= 2:   // bTValArray<eSLightmapUVGroup>
    u8 1; u32 groupCount
    groupCount x { u8 1; u32 a; u32[a]; u8 1; u32 b; u32[b]; f32[3] vec; f32[16] matrix; f32[2] vec2 }   // 12+64+8 = 84 trailing bytes
if version >= 4:   // eCSpatialHierarchy
    u32 hierCount; hierCount x { f32[4] bCSphere(cx,cy,cz,radius); u32 firstIndex?; u32 count? }   // 24 bytes each, NOTE: NO leading bool byte here
    u8 1; u32 n; u32[n]                                                                            // remap/index array, count == triangle count in the case I decoded

FVF is exactly reconstructible (VERIFIED 1033/1033 elements): 0x2 if a VertexPosition stream exists | 0x10 if Normal | 0x40 if Diffuse | 0x80 if Specular | (numberOfTexCoordStreams << 8). Observed values: 0x1D2 (6845), 0x4D2 (764), 0x92 (568), 0x2D2 (469), 0x192 (415), 0xD2 (162), 0x3D2 (63). NOTE the FVF does NOT flag the tangent stream (64) or the lightmap-UV stream (73) — you must enumerate the streams.

Submesh semantics: one eCMeshElement == one material == one draw call. Its index stream is LOCAL (indices are 0..vertexCount-1 within this element, VERIFIED: 0 out-of-range over 1033 elements). All per-vertex streams inside one element have identical element counts (VERIFIED, 0 mismatches) — they are parallel arrays, so you can interleave them straight into one vertex buffer.

The `size` field for the barrel reproduces as 124 + streamCount*20 + triangleCount*12 + vertexCount*bytesPerVertex, but that formula only matched 260/600 elements across a wider sample, so treat `size` as an opaque engine hint.
```


## eCVertexStructArray (one vertex stream) + stream-type table

Confidence: high. Source: `F:\Projects\Gothic3\g3blend\g3blend\io\structs\mesh_element.py:139-303 (table) and :237-243 (array framing); identical table in F:\Projects\Gothic3\g3dit\LrentNode\src\main\java\de\george\lrentnode\structures\eCMeshElement.java:110-218; winding/handedness/colour measurements from C:\Users\gotow\AppData\Local\Temp\claude\F--Projects\1a4c13ea-b969-468f-b2b4-990e6442110f\scratchpad\mesh\winding.py, colors.py, bitan.py`

```
Per stream:
  u32  streamType      // eEVertexStreamArrayType
  u16  1               // array version, always 1 (VERIFIED 67472/67472)
  u8   1               // bTArray's spurious leading byte (VERIFIED always 1)
  u32  elementCount
  elementCount x element    // element size looked up from streamType

Stream type -> element struct (the eSFVF table). ESZ: bCVector2=8, bCVector3=12, bCVector4=16, GEU32=4, GEU16=2, GEFloat=4.
  0  Face                       GEU32     (D3D index)
  1  VertexPosition             bCVector3 (D3DFVF_XYZ, bit 0x2)
  2  VertexPositionTransformed  bCVector4 (D3DFVF_XYZRHW, bit 0x4)
  3  Normal                     bCVector3 (D3DFVF_NORMAL, bit 0x10)
  4  Diffuse                    GEU32     (D3DFVF_DIFFUSE, bit 0x40, D3DCOLOR = 0xAARRGGBB, stored B,G,R,A in memory)
  5  Specular                   GEU32     (D3DFVF_SPECULAR, bit 0x80)
  6  PointSize                  GEFloat   (bit 0x20)
  7..11 XYZB1..XYZB5            GEFloat   (skin weights — NEVER present in .xcmsh; skinning lives in .xact)
  12 TextureCoordinate          bCVector2 (UV set 0)
  13..59  cycle bCVector3 / bCVector4 / bCVector2 starting at 13
  60..63  bCVector4
  64..67  bCVector3  (64 = TangentVector)
  68..71  bCVector2
  72      bCVector3
  73      UVLightmapGroups      bCVector2

What actually occurs in _compiledMesh.pak (9286 mesh elements): type 0 Face 9286, 1 Position 9286, 3 Normal 9286, 5 Specular 9286, 12 UV0 8556, 64 Tangent 8307, 4 Diffuse 8303, 73 lightmap-UV 2263, 15 UV1 1296, 21 UV3 821, 18 UV2 770, 72 (bCVector3, unknown) 12. So EVERY submesh always has position+normal+specular+indices; UV/tangent/diffuse are optional.

Stream ORDER IS NOT FIXED. Most common orders: (12,5,4,3,1,0,64), (12,0,1,3,5,73), (5,3,1,0). Dispatch on the type field, never on position.

INDEX BUFFER: type 0, always u32 on disk, count always divisible by 3 (VERIFIED 9286/9286), i.e. an unindexed-strip-free plain triangle LIST. Max index seen across all 5392 files = 88638, so 2 of 9286 elements genuinely need 32-bit indices; the other 9284 fit in u16 and can be narrowed.

WINDING: measured by dot(cross(p[i1]-p[i0], p[i2]-p[i0]), normal[i0]) over 1926 triangles: 1916 negative, 10 positive. So in the file's own left-handed Y-up space the triangle order (i0,i1,i2) is CLOCKWISE when seen from the front — the D3D9 default front face.

Normals and tangents are already unit length (measured: 10932/10932 and 9973/9973 at |v|=1.00).
UVs are far outside [0,1] (measured range -1883.7 .. +1593.9) — wrap addressing is mandatory.

DIFFUSE STREAM IS NOT A VERTEX COLOUR. Dominant DWORD values are 0x00FF0000, 0xFFFFFFFF, 0x00FFFFFF, 0x00000000. The R byte (bits 16-23) encodes the BITANGENT HANDEDNESS: predicting sign = (R >= 128 ? +1 : -1) matched the geometric handedness of cross(N,T) vs the UV-derived bitangent on 11269/12136 triangles (92.9%). This matches the G3MC manual note quoted in the parsers ("Bi-Tangent Heading - 00FF0000"). G and B toggle 0/255 for reasons I did not identify; A varies (G3MC calls it "Texture Fading").
SPECULAR STREAM: B=G=R=0 in 20819/20819 samples; only the A byte varies (0xFF, 0xFE, 0xFD, ... a smooth ramp) — a single per-vertex scalar (mimicry maps it to vertex alpha).
```


## Coordinate system / units conversion

Confidence: high. Source: `F:\Projects\Gothic3\rmtools\mimicry\source\Mimicry\mi_coordshifter.cpp:43-76`

```
Genome is LEFT-HANDED, Y-UP, units = centimetres (barrel_01 AABB is -42.79..42.79 x 4.63..99.76 x -40.70..40.70).
The reference exporter's Genome->3dsMax(Z-up right-handed) transform is: swap Y and Z of every position/normal/tangent; multiply texcoord.y by -1; swap face vertices A and C (reverse winding). The matrix form swaps rows/cols 1<->2.
For a Vulkan runtime: the UV convention already matches D3D/Vulkan (V axis down, origin top-left) so DO NOT flip V. If you convert LH->RH by negating one axis you must also reverse the index triples (or flip VkPipelineRasterizationStateCreateInfo frontFace).
```


## eCResourceMeshLoD_PS (.xlmsh) — the LOD chain

Confidence: high. Source: `F:\Projects\Gothic3\g3dit\LrentNode\src\main\java\de\george\lrentnode\classes\eCResourceMeshLoD_PS.java:24-27 and F:\Projects\Gothic3\rmtools\mimicry\source\Mimicry\mi_xlmshreader.cpp:26-35`

```
Property-set header, className "eCResourceMeshLoD_PS", classVersion 23 (all 108 shipping files).
Body:
  <eCResourceBase_PS: u16 resourceVersion=30; u32 size>
  (mimicry only: if classVersion < 23: u32 — never triggers in shipping data; g3dit omits this branch)
  u32 count
  count x entry     // .xcmsh file names, LOD0 (highest detail) first
Measured: 104 files have 2 entries, 4 have 3. Example: ["G3_Myrtana_Ardea_House_2_Story_01.xcmsh", "G3_Myrtana_Ardea_House_2_Story_01_LOD1.xcmsh"].
NOTE: no switch distances/thresholds are stored here. LOD selection metrics must come from elsewhere (entity-side eCVisualMeshStatic_PS / engine LOD settings) — not resolved by this task.
VERIFIED: 108/108 parse and end byte-exactly.
```


## eCResourceShaderMaterial_PS (.xshmat) — top level

Confidence: high. Source: `F:\Projects\Gothic3\g3blend\g3blend\io\property_sets\resource_shader_material.py:123-128 and F:\Projects\Gothic3\g3dit\LrentNode\src\main\java\de\george\lrentnode\classes\eCResourceShaderMaterial_PS.java:130-138`

```
Property-set header, className "eCResourceShaderMaterial_PS", classVersion 64 (991) or 1 (9 legacy).
Declared properties (all 5 present on modern materials): PhysicMaterial (bTPropertyContainer<enum eEShapeMaterial>, 6 bytes), IgnoredByTraceRay (bool), DisableCollision (bool), DisableResponse (bool), ResourcePriority (float). These are collision/physics attributes, not rendering.
Body:
  <eCResourceBase_PS: u16 resourceVersion=30; u32 size>
  <nested property set>   // the shader; parse it with the SAME reader (SUB_CLASS_IDENTIFIER + entry name + filler + header)
VERIFIED: 1000/1150 .xshmat parse and end byte-exactly; the other 150 are legacy (see gotchas).
```


## eCShaderBase / eCShaderDefault / Skin / Leaf / Water / Particle (the effect)

Confidence: high. Source: `F:\Projects\Gothic3\g3blend\g3blend\io\property_sets\shader.py:20-31 (slot order), shader_base.py:182-192, shader_element_base.py:205-214, types/color_src_proxy.py:82-87, types/guid.py:11-20; property lists cross-checked against F:\Projects\Gothic3\gothic3sdk\g3\Engine\include\g3sdk\Engine\ge_shaderbase.h:71-79 and ge_shaderdefault.h:39-42; enum values from F:\Projects\Gothic3\g3dit\LrentNode\src\main\java\de\george\lrentnode\enums\G3Enums.java:535-566`

```
The nested shader is itself a property set; its className IS the effect name. Distribution over 1000 materials: eCShaderDefault 864, eCShaderSkin 118, eCShaderLeaf 12, eCShaderWater 5, eCShaderParticle 1. classVersion: 2 (982), 1 (12), 3 (6).

Body, read in this exact order (derived class first, then bases):
  1) N x eCColorSrcProxy, one per slot (order fixed per class):
       eCShaderDefault : Diffuse, Opacity, SelfIllumination, Specular, SpecularPower, Normal, [Distortion if classVersion > 1]
       eCShaderSkin    : Diffuse, Opacity, SelfIllumination, Specular, SpecularPower, Normal, SubSurface
       eCShaderLeaf    : Diffuse, Specular, SpecularPower, Normal
       eCShaderWater   : Diffuse, StaticBump, FlowingBump, Specular, SpecularPower, Reflection, [Distortion if classVersion > 1]
       eCShaderParticle: Diffuse, [Distortion if classVersion > 1]
     eCColorSrcProxy = { u32 colorComponent; u8[20] bCGuid }   (24 bytes)
       bCGuid = 16 raw bytes + u32 validFlag; the proxy is UNSET iff (validFlag & 0xFF) == 0, and when unset the 16 GUID bytes are uninitialised heap garbage.
       colorComponent (eEShaderColorSrcComponent) observed: 0 for RGB slots, 5 for SpecularPower (915/1000) — a swizzle/channel selector. The SDK does not enumerate it.
  2) u16 == 1                       // eCShaderBase version
  3) u16 == 1; u8[20] token(bCGuid); i32[4] editorLayout(bCRect: topLeft.x,y bottomRight.x,y)   // eCShaderEllementBase
  4) u32 elementCount; elementCount x <nested property set>   // the colour-source DAG nodes

RENDER-RELEVANT PROPERTIES (declared on the shader property set):
  BlendMode  (enum eEShaderMaterialBlendMode) 0=Normal 1=Masked 2=AlphaBlend 3=Modulate 4=AlphaModulate 5=Translucent 6=Darken 7=Brighten 8=Invisible.
     Measured: 872 Normal, 83 Masked, 41 AlphaBlend, 3 Brighten, 1 Invisible.
  MaskReference (char, 1 byte) — alpha-test reference for Masked. 904 files use 0; others 0x7D, 0x7F, 0x80, 0xB0, 0xB4, 0x8C, 0x3C.
     g3dit's renderer converts it to a discard threshold as 0.95 - MaskReference/255 (a heuristic, not engine truth).
  MaxShaderVersion (enum eEShaderMaterialVersion) 0=ps_1_1 1=ps_1_4 2=ps_2_0 3=ps_3_0.
  TransformationType (enum eEShaderMaterialTransformation) 0=Default 1=Instanced 2=Skinned 3=Tree_Branches 4=Tree_Fronds 5=Tree_Leafs 6=Billboard. THIS is what tells you which vertex path a mesh needs.
  EnableSpecular (bool), DisableLighting (bool), UseDepthBias (bool), FallbackMaterial (bCImageOrMaterialResourceString).
  eCShaderSkin adds: EnableRimLighting(bool), RimColor(bCFloatColor), RimPower(float), SubSurfaceRollOff(float).
  eCShaderLeaf adds: EnableSubSurface(bool). eCShaderWater adds: FresnelConstant, ShoreFadingScale, ReflectionColor, DepthRed/Green/BlueHalfLife, DepthScale.
No cull-mode, depth-write or two-sided flag is stored — those must be inferred (or defaulted) by the runtime.
```


## Colour-source graph nodes (texture slots live here)

Confidence: high. Source: `F:\Projects\Gothic3\g3blend\g3blend\io\property_sets\color_src.py:225-321 and io\types\tex_coord_src_proxy.py:100-105; property names from F:\Projects\Gothic3\gothic3sdk\g3\Engine\include\g3sdk\Engine\ge_colorsrcsampler.h (GE_PROPERTY block); enums from G3Enums.java:227-250; resolution behaviour from F:\Projects\Gothic3\g3dit\g3dit\src\main\java\de\george\g3dit\jme\asset\XshmatLoader.java:186-258`

```
Each element in the shader's element list is a nested property set. A slot's proxy GUID is resolved by SEARCHING the element list for the element whose `token` bCGuid equals it — there is no index.
Every node ends with: [class-specific fields] then u16==1 (eCColorSrcBase) then u16==1; u8[20] token; i32[4] editorLayout (eCShaderEllementBase).
Measured element census over 1000 materials: eCColorSrcSampler 2117, eCColorSrcConstant 1027, eCColorSrcCombiner 739, eCColorSrcBlend 228, eCTexCoordSrcScale 163, eCColorSrcVertexColor 96, eCTexCoordSrcBumpOffset 38, eCColorSrcCubeSampler 21, eCColorSrcSkydomeSampler 14, eCTexCoordSrcScroller 10, eCTexCoordSrcOscillator 8, eCTexCoordSrcRotator 6, eCTexCoordSrcColor 1.

Class-specific fields:
  eCColorSrcSampler   : eCTexCoordSrcProxy texCoordSrc { u32 vertexTexCoordIndex; u8[20] bCGuid };  u32 samplerType (eEColorSrcSamplerType, values 0xFFFFFFFF:1590, 0:419, 1:84, 2:15, 3:9 — enum not documented in the SDK)
     Properties: ImageFilePath (bCImageResourceString), TexRepeatU / TexRepeatV (enum eEColorSrcSampleTexRepeat 0=Wrap 1=Clamp 2=Mirror — measured 2116x Wrap, 1x Clamp), AnimationSpeed (float), SwitchRepeat (enum eEColorSrcSwitchRepeat 0=Repeat 1=Clamp 2=PingPong)
  eCColorSrcCombiner  : 2 x eCColorSrcProxy (src1, src2). Property CombinerType (enum eEColorSrcCombinerType 0=Add 1=Subtract 2=Multiply 3=Max 4=Min)
  eCColorSrcBlend     : 3 x eCColorSrcProxy (src1, src2, blend)
  eCColorSrcConstant  : no extra fields. Properties Color (bCFloatColor, 16 bytes = u32 vftable + 3 floats RGB) and Alpha (float)
  eCColorSrcVertexColor / eCColorSrcCubeSampler : no extra fields
  eCTexCoordSrc* and eCColorSrcSkydomeSampler : layout NOT reverse-engineered by any parser — skip them via the node's own declaredEnd (this works: I verified 1000 materials parse byte-exactly while skipping 230 such nodes).

PRACTICAL EXTRACTION for an approximate PBR-ish render (what g3dit's own renderer does):
  albedo   = element(shader.Diffuse.guid)   -> if Sampler use ImageFilePath; if Combiner recurse into src1
  normal   = element(shader.Normal.guid)
  specular = element(shader.Specular.guid)
  opacity  = element(shader.Opacity.guid); blend state from BlendMode; alpha-test ref from MaskReference
A worked example (G3_Objects_Barrelmetal_01_A.xshmat) resolves to Diffuse->G3_Objects_Barrelmetal_01_Diffuse_01.tga, Normal->..._Normal_01.tga, Specular via a Multiply-Combiner chain onto ..._Specular_01.tga, SpecularPower->an eCColorSrcConstant (0.941 grey, alpha 0.3).

TEXTURE PATH RESOLUTION (measured over 2279 sampler references): strip any directory and the extension from ImageFilePath, lowercase, append ".ximg", and look up by BASENAME in _compiledImage.pak (its 1902 basenames are globally unique, so the archive's internal directories are irrelevant). That resolves 2096. Adding the fallback "trailing _NN -> _s1" (the compiler renames numbered variants into switched sets, e.g. G3_Monster_Bunny_Body_Diffuse_01.tga -> animation/g3_monster_bunny_body_diffuse_s1.ximg) reaches 2113/2279; the remaining 166 are dead references from legacy/editor materials and simply do not ship. ImageFilePath extensions in the data: .tga 2020, .dds 90, .bmp 5.
```


## .ximg header (fixed 87-byte prefix)

Confidence: high. Source: `field offsets confirmed by F:\Projects\Gothic3\g3dit\G3Utils\src\main\java\de\george\g3utils\io\XimgIO.java:20-46 (skip 8, skip 2, readInt=end, skip 33, width, height, skip 8, mipmapCount, skip 4, 4-byte type) and F:\Projects\Gothic3\rmtools\QImageIOPlugin\source\ximgplugin\ximghandler.cpp:78-100; every constant/flag measured by C:\Users\gotow\AppData\Local\Temp\claude\F--Projects\1a4c13ea-b969-468f-b2b4-990e6442110f\scratchpad\mesh\ximg_final.py over all 1897 files`

```
A GENOMFLE-wrapped file whose payload is NOT a property set but a raw eCResourceImage-style struct. Field offsets are absolute from file start and FIXED (verified on all 1897 shipping .ximg):
  0x00 char[8]  "GENOMFLE"
  0x08 u16      1
  0x0A u32      dataEndOffset      // also the DEADBEEF/string-table offset. THE PIXEL DATA ENDS HERE.
  0x0E u16      classVersion = 32  (32 in 1897/1897)
  0x10 u32      logicalSize        // sum over levels of w*h*bitsPerPixel/8 WITHOUT block rounding (e.g. 0x5555 for a 128x256 DXT1). Informational only.
  0x14 u32      UNKNOWN / uninitialised — literally contains ASCII fragments ("Stan", "on_S") in some files. Ignore.
  0x18 u64      Windows FILETIME of the source image
  0x20 u32      source file size in bytes (e.g. 98348 = 128*256*3 + 44 TGA header; 262272 = 256*256*4 + 128 DDS header)
  0x24 u16      1  (1 in 1897/1897)
  0x26 char[5]  "G3IMG"
  0x2B u16      2  (image-block version; 2 in 1897/1897)
  0x2D u16      UNKNOWN, 28 distinct values, no correlation found. Probably padding/garbage.
  0x2F u32      width
  0x33 u32      height
  0x37 u32      0  (0 in 1897/1897) — depth/volume?
  0x3B u8       isCubeMap  (0 in 1895 files, 1 in the two level/g3_misc_cubemap_0N.ximg)
  0x3C u8[3]    uninitialised padding (contains ASCII garbage such as "3_O", "t -")
  0x3F u32      mipMapCount
  0x43 u32      0  (0 in 1897/1897)
  0x47 u32      D3DFORMAT  — either a FourCC ('DXT1' 1125, 'DXT5' 418, 'DXT3' 353) or a small D3DFMT_* integer (21 = D3DFMT_A8R8G8B8, 1 file)
  0x4B u32      1  (1 in 1897/1897)
  0x4F u32      logicalSize again (same value as 0x10)
  0x53 u32      0  (0 in 1897/1897)
  0x57 = 87     start of pixel data
Tail: at dataEndOffset there is EF BE AD DE, then 01 (hasStringTable) and 00 00 00 00 (count 0) — i.e. exactly 9 bytes after the payload; every shipping .ximg has an empty string table.
All shipping textures are power-of-two and multiples of 4 (0 exceptions in 1897).
Gothic 3 .ximg has NOTHING in common with Risen's "GR01IM04" .ximg (the rmtools Qt plugin handles both; only its second branch is ours).
```


## .ximg mip chain layout (mips are stored SMALLEST FIRST)

Confidence: high. Source: `F:\Projects\Gothic3\g3dit\G3Utils\src\main\java\de\george\g3utils\io\XimgIO.java:34-47; F:\Projects\Gothic3\g3dit\g3dit\src\main\java\de\george\g3dit\jme\asset\XimgLoader.java:85-100; F:\Projects\Gothic3\rmtools\QImageIOPlugin\source\ximgplugin\ximghandler.cpp:95-97; ordering proof in C:\Users\gotow\AppData\Local\Temp\claude\F--Projects\1a4c13ea-b969-468f-b2b4-990e6442110f\scratchpad\mesh\miporder.py`

```
Payload occupies [87, dataEndOffset). Total payload bytes == faces * sum over i in [0, mipMapCount) of levelSize(w >> i, h >> i), where faces = isCubeMap ? 6 : 1.
  levelSize(w,h) for DXT1  = max(1,ceil(w/4)) * max(1,ceil(h/4)) * 8
  levelSize(w,h) for DXT2-5= max(1,ceil(w/4)) * max(1,ceil(h/4)) * 16
  levelSize(w,h) for D3DFMT_A8R8G8B8 (21) = w*h*4
VERIFIED: this formula gives a data start of exactly 87 for 1897/1897 files, including both cube maps and the one A8R8G8B8 file.

ORDER IS REVERSED relative to DDS: the SMALLEST mip is first at offset 87 and MIP LEVEL 0 (full resolution) IS LAST, ending exactly at dataEndOffset. Two independent confirmations:
  (a) g3dit computes imageStart = dataEndOffset - baseLevelSize and decodes the base mip from there (XimgIO.java:37-47, XimgLoader.java:85-100).
  (b) I decoded the DXT endpoint means per candidate level under both orderings: smallest-first keeps a stable mean colour down the chain ((125,124,132)...(131,125,131)) while largest-first diverges wildly ((126,125,132)...(110,52,53)).
So to upload: read backwards, or read the whole block and index level L at offset dataEndOffset - sum(levelSize(0..L)).

Mip chains are often TRUNCATED: 316/1897 files store fewer levels than a full chain to 1x1 (e.g. 512x256 with 9 instead of 10; 1024x1024 with 1 level only, 33 files). Always use the stored mipMapCount, never log2(max(w,h))+1.
Cube maps: payload is exactly 6 x the chain, but the face order and whether it is face-major or mip-major is NOT determined (only 2 cube maps ship).
DXT blocks are byte-identical to D3D/DDS/BCn — no swizzling, no per-block transform. Copy straight into a VkImage.
```


## Direct-to-GPU vs needs-conversion summary

Confidence: high. Source: `synthesis of the above; blend-mode mapping mirrors F:\Projects\Gothic3\g3dit\g3dit\src\main\java\de\george\g3dit\jme\asset\XshmatLoader.java:107-140`

```
MAPS DIRECTLY ONTO A MODERN GPU PIPELINE (memcpy, no transform):
  - Positions / normals / tangents: 3 x f32 -> VK_FORMAT_R32G32B32_SFLOAT. UVs: 2 x f32 -> VK_FORMAT_R32G32_SFLOAT. All streams are parallel arrays of identical length, so you can interleave into one vertex buffer in one pass.
  - Index data: triangle list, u32 -> VK_INDEX_TYPE_UINT32 verbatim (or narrow to UINT16 when maxIndex <= 65535, which holds for 9284/9286 submeshes).
  - DXT1/DXT3/DXT5 blocks -> VK_FORMAT_BC1_RGBA_UNORM_BLOCK / BC2_UNORM_BLOCK / BC3_UNORM_BLOCK, byte-for-byte. Use BC1_RGBA (not BC1_RGB) because D3DFMT_DXT1 carries 1-bit alpha via the c0<=c1 encoding.
  - D3DFMT_A8R8G8B8 (21) -> VK_FORMAT_B8G8R8A8_UNORM verbatim (D3DCOLOR is BGRA in memory).
  - Per-submesh AABB (bboxMin/bboxMax) and the eCSpatialHierarchy bCSphere array -> culling structures as-is.
  - TexRepeatU/V -> VK_SAMPLER_ADDRESS_MODE_REPEAT / CLAMP_TO_EDGE / MIRRORED_REPEAT 1:1.
  - UV convention (V down, origin top-left) already matches Vulkan/D3D — no flip.

NEEDS CONVERSION:
  - MIP ORDER: the file stores smallest-first; every GPU API wants level 0 first. Reverse while uploading.
  - HANDEDNESS: left-handed Y-up. If you mirror an axis to reach a right-handed convention you must reverse index triples or set frontFace accordingly; as-is the winding is clockwise-front.
  - BITANGENT: not stored as a vector. Compute B = handedness * cross(N, T) with handedness = (diffuseDWORD >> 16 & 0xFF) >= 128 ? +1 : -1. When there is no diffuse stream, fall back to +1 or recompute from UVs.
  - VERTEX 'COLOUR': the diffuse and specular D3DCOLOR streams are NOT colours (see the stream structure). Do not feed them to a shader as vertex colour.
  - sRGB: nothing in the file says which textures are colour. You must decide per slot — albedo/self-illumination as BC*_SRGB, normal/specular/opacity as UNORM.
  - THE SHADER GRAPH: eCColorSrc*/eCTexCoordSrc* is a small DAG of samplers/constants/combiners/blends with per-node token GUIDs. There is no HLSL in the file (the engine compiled it at runtime). A modern runtime must collapse the DAG to a fixed material model: take the sampler under each of Diffuse/Normal/Specular/Opacity, recursing through Combiner.src1 as g3dit does, and drop animated tex-coord nodes on first pass.
  - BLEND STATE: BlendMode is an engine enum, not a D3D/VK blend equation. Normal -> opaque, Masked -> opaque + alpha discard at MaskReference, AlphaBlend/AlphaModulate/Translucent -> src-alpha/one-minus-src-alpha, Modulate -> multiplicative, Brighten/Darken -> additive/subtractive, Invisible -> skip the draw.
  - Non-power-of-two: none occur, so no special handling needed.
```


### Gotchas

- ENDIANNESS/ENCODING: everything is little-endian. Strings are windows-1252, prefixed by u16 length, NOT NUL-terminated.
- TWO CONTAINER FLAVOURS PER EXTENSION. Files with the same extension in the same .pak may or may not start with "GENOMFLE". If wrapped, the string table sits at the END of the file and EVERY name (class, property, property type, submesh material, texture path) is a 2-byte index into it. Sniff the magic first, then load the table, then parse. 3350 of 5392 .xcmsh are wrapped, 2042 are not; 651 of 1000 .xshmat wrapped, 349 not.
- THE 5-BYTE 'FILLER' BETWEEN THE CLASS NAME AND THE VERSION IS NOT CONSTANT. g3blend and g3dit both hardcode 01 00 00 53 00 and just skip 5 bytes. It is really u8(1), u16(0), u16(psVersion), and psVersion is then written AGAIN. Observed psVersion: 83 (modern Genome files), 82, 81, 1 (legacy raw files). Parse it, do not skip it — it gates the next two fields.
- VERSION-GATED HEADER FIELDS: an object NAME string is present only when psVersion < 81, and a 20-byte GUID only when psVersion < 82. Both branches occur in shipping data (1055 .xcmsh at psVersion 1, 6 at 81). PARSER DISAGREEMENT: mimicry uses `< 81` for the name (mi_xcmshreader.cpp:47) while g3blend uses `== 0x01` (property_set.py:28). They coincide on shipping data because only version 1 carries a name; mimicry's rule is the safer generalisation, but nothing in the shipped data distinguishes them for versions 2..80.
- bTArray SERIALISES A USELESS LEADING BYTE (always 0x01) BEFORE ITS u32 COUNT. Every vertex stream and every lightmap index array has it. The ONE exception is the eCSpatialHierarchy element array in eCMeshElement version >= 4, which is a bare u32 count + payload with no leading byte (both mimicry and g3dit agree on this asymmetry).
- ALWAYS SEEK TO THE PROPERTY SET'S DECLARED END. The u32 after the doubled version gives declaredEnd = tell() + size. 150 of 1150 .xshmat (legacy editor materials: boden.xshmat, '02 - default.xshmat', editsupporter_*, g3_lightstreaks_01, g3_varant_water_oasis_01) have ~1 KB of extra data after the shader element list that NEITHER g3blend NOR g3dit accounts for — it looks like a repeated { u16 1; bCGuid } proxy list, and I did not decode it. Every field you actually need (shader class, blend mode, samplers, texture paths) is read before that tail, so the fix is simply to trust declaredEnd instead of assuming you consumed the whole set. Only 13 of 611 mesh-referenced materials, and 92 of 9286 submeshes, are affected.
- eCMeshElement's `size` field is an engine memory-footprint estimate, NOT a byte count in the file. It reproduced as 124 + streams*20 + tris*12 + verts*bytesPerVertex for the barrel but only for 260/600 elements across a wider sample. Never use it to seek past a submesh.
- eCResourceBase_PS's `size` is likewise not a file size — in g3_object_barrel_01.xcmsh it equals the LAST mesh element's `size` (11268), i.e. it is just an overwritten accumulator.
- VERTEX STREAM ORDER IS NOT FIXED. Common orders include (12,5,4,3,1,0,64) and (12,0,1,3,5,73) and (5,3,1,0). Always dispatch on the u32 type tag; and read the element size from the type table, not from an assumption.
- THE FVF DOES NOT DESCRIBE ALL STREAMS. It encodes only position(0x2), normal(0x10), diffuse(0x40), specular(0x80) and the texcoord-set count (<<8). Tangents (type 64) and lightmap UVs (type 73) are invisible to it. Verified exactly reconstructible from the stream set on 1033/1033 elements — so use it as a checksum, not as the source of truth.
- THE DIFFUSE VERTEX STREAM IS NOT A VERTEX COLOUR. Its R byte carries the bitangent handedness (predicting sign from R>=128 matched geometry on 92.9% of 12136 triangles; the G3MC manual quoted inside both parsers calls it "Bi-Tangent Heading - 00FF0000"). The specular stream has B=G=R=0 in 20819/20819 samples and only its A byte varies. Rendering either as a colour will be wrong.
- INDEX WIDTH: indices are u32 on disk and always a multiple of 3 (verified 9286/9286). Max index across the whole archive is 88638, so 2 of 9286 submeshes truly need 32-bit indices — you cannot assume 16-bit, but you can narrow per-submesh.
- WINDING: measured clockwise-front in the file's left-handed Y-up space (1916 of 1926 triangles had cross(b-a,c-a) . N < 0). Any left-to-right-handed conversion (mimicry swaps Y/Z) must also swap index 0 and 2 or flip frontFace.
- TEXCOORD V: Genome uses the D3D convention. The reference Max exporter multiplies V by -1; a Vulkan runtime must NOT. UVs range far outside [0,1] (measured -1883.7 .. 1593.9), so wrap addressing is mandatory.
- .ximg MIPS ARE STORED SMALLEST-FIRST. Mip level 0 is the LAST block and ends exactly at the u32 at offset 0x0A. Reading the file forward as a DDS-style chain gives you a garbage image at every level. Verified on all 1897 files.
- .ximg PIXEL DATA ALWAYS STARTS AT ABSOLUTE OFFSET 87. The header is fixed-size — verified on 1897/1897 including cube maps and the one uncompressed file. (The rmtools Qt plugin's own field offsets are shifted by 16 relative to reality; trust g3dit's XimgIO offsets, which I re-verified byte by byte.)
- .ximg MIP CHAINS ARE OFTEN TRUNCATED: 316/1897 store fewer levels than log2(max(w,h))+1 (e.g. 33 files are 1024x1024 with a single level). Always honour the stored mipMapCount.
- .ximg CUBE MAPS: the byte at 0x3B (which naive parsers read as part of a u32) is a cube flag — 1 in exactly the two level/g3_misc_cubemap_0N.ximg, 0 elsewhere; the payload is then 6 x the chain. The 3 bytes at 0x3C-0x3E, the u32 at 0x14 and the u16 at 0x2D are UNINITIALISED HEAP GARBAGE (they contain readable ASCII fragments like "Stan", "3_O", "t -"). Never validate a file against them.
- .ximg's format field is a D3DFORMAT, not just a FourCC: DXT1/DXT3/DXT5 as FourCCs plus small integers for uncompressed formats (21 = D3DFMT_A8R8G8B8 in _special/vegetaion_alphadisolve.ximg). Your reader must handle both shapes.
- TEXTURE NAME RESOLUTION: ImageFilePath points at the SOURCE asset (.tga 2020x, .dds 90x, .bmp 5x) and 1089 of them still carry a directory prefix. Strip directory + extension, lowercase, append .ximg and look up by BASENAME (the archive's 1902 basenames are unique). That resolves 2096/2279; adding the compiler's numbered->switched rename rule (trailing _NN -> _s1) reaches 2113. The remaining 166 references do not ship at all — you need a fallback texture.
- MATERIAL REFERENCES CAN DANGLE: 41 distinct material names referenced by .xcmsh submeshes end in .xmat (uncompiled) and have no .xshmat in _compiledMaterial.pak.
- .pak ARCHIVES ARE MULTI-VOLUME. _compiledMesh.p00 is a separate volume with its own 48-byte header and its own file table holding 59 further entries; the 6282 entries listed in _compiledMesh.pak all live inside the .pak itself. Enumerate every volume, do not assume one file table covers the set.
- LOD LIVES IN A SEPARATE FILE. .xcmsh has no LOD chain; .xlmsh (eCResourceMeshLoD_PS) is just an ordered list of .xcmsh names, LOD0 first, with NO switch distances. mimicry has an extra `if classVersion < 23: u32` that g3dit lacks — all 108 shipping files are exactly version 23, so the disagreement is untestable on shipping data.
- bCFloatColor IS 16 BYTES AND HAS NO ALPHA: u32 vftable pointer + 3 floats RGB (verified: the declared property size is 16 in 339/339 occurrences). eCColorSrcConstant carries opacity in a SEPARATE `Alpha` float property. Reading it as 4 floats will silently pick up the next property's bytes.


### Open questions

- The ~1 KB tail on 150 legacy .xshmat files (psVersion 1 / shader classVersion 1-2) after the shader element list. It looks like a repeating { u16 1; 20-byte bCGuid } sequence with several all-zero entries, possibly the eCShaderEllementProxy graph (eCShaderBase::AddSwitchEllement / GetProxyAt exist in the SDK) written in an older order. Neither g3blend nor g3dit decodes it. Skipping to declaredEnd is safe for rendering.
- eEColorSrcSamplerType (the u32 after eCColorSrcSampler's texCoordSrc) is an undocumented enum: 0xFFFFFFFF (1590), 0 (419), 1 (84), 2 (15), 3 (9). The SDK header declares the enum with no enumerators. My guess is it is a cache of the derived sampler kind (2D / normal-map / cube / ...), with 0xFFFFFFFF meaning 'not yet resolved', but that is unverified.
- eEShaderColorSrcComponent (the u32 in eCColorSrcProxy). Value 0 for RGB slots and 5 for SpecularPower in 915/1000 materials — plausibly a channel-swizzle selector (rgb / r / g / b / a / ...), but the SDK does not enumerate it and I did not confirm which index maps to which channel.
- The eCMeshElement version>=4 eCSpatialHierarchy: the layout (bCSphere + 2 u32) is documented by g3dit, and the values decode plausibly (nested spheres, the trailing u32 array has exactly triangleCount entries in the case I checked), but the exact meaning of the two u32s (first-child/count vs first-triangle/count) is unconfirmed, and one decoded sphere had a slightly negative radius.
- The G and B channels of the diffuse vertex stream toggle 0/255 independently of the R handedness bit, and the A byte varies smoothly; the G3MC manual calls the diffuse stream 'Texture Fading' in addition to bi-tangent heading. Their exact roles are unknown.
- Cube-map .ximg face order and whether the 6 faces are stored face-major or mip-major. Only two cube maps ship, so I could not disambiguate by decoding.
- LOD switch distances/metrics: .xlmsh carries names only. Where the engine gets the switch thresholds (entity-side eCVisualMeshStatic_PS, a global LOD table, or screen-space size) was outside this task's files.
- No cull mode, depth-write, depth-test or two-sided flag exists anywhere in .xshmat. Whether foliage/masked materials are meant to be two-sided must be inferred (probably from BlendMode == Masked and/or eCShaderLeaf) or hardcoded.
- The u32 at .ximg offset 0x14 and the u16 at 0x2D are unexplained. They contain uninitialised memory in at least some files, so they may be entirely vestigial, or a valid checksum field that is only sometimes written.
- Whether any Gothic 3 content (mods, the Community Patch, or Content Mod assets) ships .ximg in D3DFMT_* forms other than DXT1/3/5 and A8R8G8B8. Only those four appear in the stock archives, but the format field is a full D3DFORMAT so a reader should handle at least X8R8G8B8, R5G6B5, A1R5G5B5, A4R4G4B4, and L8/A8.


## Container

EVERYTHING IS LITTLE-ENDIAN, NO ALIGNMENT PADDING (all "padding" bytes are uninitialised junk).

Outer wrapper (identical for .xact, .xmot, .xcmsh, .xshmat, …) — "Genome file":
  0x00  char[8]  "GENOMFLE"
  0x08  u16      container version (== 1 in all shipped files)
  0x0A  u32      tailOffset  (absolute offset of the 0xDEADBEEF marker)
  0x0E  ...      payload (the property-set / resource object), runs until tailOffset
  tailOffset+0  u32   0xDEADBEEF
  tailOffset+4  u8    stringTablePresent (0/1)
  if present:   u32   count, then count × { u16 len; char[len] } (cp1252, no NUL)
Source: F:\Projects\Gothic3\g3blend\g3blend\io\genome_file.py:6-39 and F:\Projects\Gothic3\g3blend\g3blend\io\binary.py:166-178.
READ THE TAIL FIRST: any field the parsers call "entry" (material file names, look-at bone names, frame-effect names) is a u16 index into that table. Strings *inside* LMA chunks are different — they are u32-length-prefixed raw bytes with no NUL and no table.

Skipping to the payload: the payload starts at 0x0E and is a versioned resource object, NOT a property set, for both formats (no property descriptors, no GE_PROPERTY blocks) — read it as a flat struct.
  .xact  payload = eCResourceAnimationActor_PS (u16 version, ==54 in all 387 shipped files)
  .xmot  payload = eCResourceAnimationMotion_PS (u16 version, ==5 in all 5937 shipped files)
Both then embed an EMotionFX-2 "LMA" chunk stream (magic 'FXA ' inside a 'gena' wrapper for actors, magic 'LMA ' for motions). Every chunk is { u32 chunkId; u32 chunkSize; u32 chunkVersion; payload[chunkSize] } — chunkSize EXCLUDES the 12-byte header (g3blend chunks.py:485-495; rmtools mi_xactreader.cpp:57-59 adds 12 back).

Verification: I re-implemented the whole container + every chunk type in Python and re-walked the shipped archive "F:\SteamLibrary\steamapps\common\Gothic 3\Data\_compiledAnimation.pak" (387 .xact, 5937 .xmot). For 387/387 .xact and 1200 sampled .xmot the computed payload end lands exactly on tailOffset, and every chunk's computed size equals its declared size (52163 NODE, 495 MESH, 495 SKININFO, 1716 MATERIAL, 1907 MATERIALLAYER, 2039 LIMIT, 75362 MOTIONPART, 46081 ANIM). Only 2 files mismatch (see gotchas). Scripts: C:\Users\gotow\AppData\Local\Temp\claude\F--Projects\1a4c13ea-b969-468f-b2b4-990e6442110f\scratchpad\skel\ (pakread.py, g3.py, dump_xact.py, dump_xmot.py, validate.py, pipeline.py).


## .xact top level — eCResourceAnimationActor_PS (version 54)

Confidence: high. Source: `F:\Projects\Gothic3\g3blend\g3blend\io\animation\xact.py:108-143 (ResourceAnimationActor.read); F:\Projects\Gothic3\g3dit\LrentNode\src\main\java\de\george\lrentnode\archive\animation\eCResourceAnimationActor_PS.java:153-170; SDK field list F:\Projects\Gothic3\gothic3sdk\g3\Engine\include\g3sdk\Engine\animation\ge_resourceanimationactor_ps.h:12-19,61-66`

```
Offsets are absolute in the file (payload starts at 0x0E).
0x0E u16   version                 (54; parsers reject anything else)
0x10 u32   resourceSize            (0 in every shipped file)
0x14 f32   resourcePriority        (uninitialised junk in shipped files)
0x18 u64   nativeFileTime          (Windows FILETIME)
0x20 u32   nativeFileSize          (== byte count of the 'gena' payload)
0x24 bCBox boundary = f32[3] min, f32[3] max  (24 B; shipped files store +FLT_MAX/-FLT_MAX = 'invalid', recompute yourself)
0x3C u32   lookAtConstraintCount
     then count × eSLookAtConstraintData { u16 stringTableIndex(nodeName); f32 interpolationSpeed; f32[3] minConstraints(radians); f32[3] maxConstraints } = 30 B each
     u32   lodActorCount
     then lodActorCount × eCWrapper_emfx2Actor  (extra LoD actors, coarsest first)
     then 1 × eCWrapper_emfx2Actor              (the base/LOD0 actor)
Observed over 387 files: version always 54; lookAtConstraintCount is 0 in 383 files and 4 in 4 files (all Hero head/neck aim constraints); lodActorCount is 0 in 325 files, 1 or 2 in 62 files.
```


## eCWrapper_emfx2Actor ('gena' + 'FXA ' block, actor version 4 / FXA 1.1)

Confidence: high. Source: `F:\Projects\Gothic3\g3blend\g3blend\io\animation\xact.py:29-88 (+6-27 MaterialReference); F:\Projects\Gothic3\g3dit\...\eCResourceAnimationActor_PS.java:89-121; rmtools F:\Projects\Gothic3\rmtools\mimicry\source\Mimicry\mi_xactreader.cpp:46-53`

```
+0  char[4] 'gena'
+4  u16     version (== 4)
+6  u32     payloadSize  — measured from the byte AFTER this field; chunkStreamEnd = pos_after_size + payloadSize
+10 char[4] 'FXA '
+14 u8      highVersion (1)
+15 u8      lowVersion  (1)
+16 ...     LMA chunk stream until chunkStreamEnd
Then, AFTER the chunk stream (still part of the actor!):
  u32 materialRefCount, then count × { u16 lodIndex; u16 matIndex; u16 stringTableIndex(name) }  — 6 B each; name is the real material resource, e.g. 'Hero_player.xshmat'
  u8  bTArray junk byte
  u32 aoLodCount            (== 1 whenever the actor has a mesh, 0 otherwise)
  aoLodCount × { u8 junk; u32 n; u32 colour[n] }        — per-vertex ambient-occlusion colours, n == MeshChunk.totalVerts
  aoLodCount × { f32[3] tangent[n] }                    — per-vertex tangents, same n, no count prefix (reuse n from the AO list)
Measured: lodIndex is 0 in all 1136 material refs; matIndex 1..5 (matIndex 0 once) and equals the index of the MATERIAL chunk in the same actor (chunk index 0 is always the dummy 'EMFX Default'); AO array length == totalVerts in 493/494 actors; every AO u32 has its low 24 bits zero (values look like 0xNN000000, NN = 0..255 concentrated near 255) → the AO byte is the most-significant byte.
Submesh.matId indexes this same material space, so: submesh → matIndex → .xshmat file name.
```


## LMA chunk header + chunk id/version table actually present in G3 data

Confidence: high. Source: `F:\Projects\Gothic3\g3blend\g3blend\io\animation\chunks.py:11-41 (LMA_CHUNK enum), 471-505 (walker); F:\Projects\Gothic3\g3dit\...\Chunks.java:17-48,567-580; rmtools mi_xactreader.cpp:5-15 (its own subset of ids)`

```
Chunk = { u32 id; u32 size; u32 version; u8 payload[size] }, size excludes the 12-byte header. Walk until the container's declared end offset.
Ids seen in shipped .xact (id,version → count over 501 actors):
  0  NODE           v3  52163
  3  MESH           v3    495
  4  SKINNINGINFO   v1    495
  6  MATERIAL       v5   1716
  7  MATERIALLAYER  v4   1907
  8  LIMIT          v1   2039
  9  PHYSICSINFO    v1   1228
  16 SCENE_INFO     v1    501
Ids seen in shipped .xmot (over all 5937 files):
  1  MOTIONPART     v3 374764
  2  ANIM(keyframe) v1 227807
Nothing else occurs — no COLLISIONMESH(5), no LIMIT/PHYSICSINFO in motions, no phoneme (12) / FX material (13) / expression parts (10,11) anywhere.
Ordering is semantic: a MATERIALLAYER applies to the most recent MATERIAL chunk; an ANIM chunk applies to the most recent MOTIONPART chunk (g3blend io_import_xmot.py:84-104). NODE order defines the node index space used by MESH.nodeNumber and SkinInfluence.nodeIndex.
```


## LMA_CHUNK_NODE (id 0, v3) — bone / node, .xact only

Confidence: high. Source: `F:\Projects\Gothic3\g3blend\g3blend\io\animation\chunks.py:103-131; F:\Projects\Gothic3\g3dit\...\Chunks.java:104-134; rmtools mi_xactreader.cpp:61-77`

```
0x00 f32[3] position       — LOCAL, parent-relative translation (cm)
0x0C f32[4] rotation       — LOCAL quaternion, component order x,y,z,w
0x1C f32[4] scaleOrient    — quaternion (scale-orientation)
0x2C f32[3] scale
0x38 f32[3] shear
0x44 u32 nameLen + char[nameLen]
     u32 parentNameLen + char[parentNameLen]   (empty string = root)
Total size = 68 + 4+nameLen + 4+parentNameLen — verified against the declared chunk size for all 52163 node chunks.
PARENTS ARE BY NAME, NOT INDEX, and the array is not topologically sorted (5065 nodes across the set have their parent later in the array) → resolve in two passes.
Measurements over all shipped actors: shear is exactly 0 everywhere; scale is exactly (1,1,1) except 58 nodes, all of which are uniform (-1,-1,-1) mirrors on head actors; scaleOrient is non-identity on many nodes but irrelevant while scale is uniform; rotation quaternions are unit (0 violations).
CONVENTION (proven, see gotchas): the stored quaternion is the plain local rotation in the file's own left-handed Y-up frame; feeding (x,y,z,w) into the standard right-handed quat→matrix formula and using the stored axes verbatim is correct. rmtools' q.Inverse() (mi_xactreader.cpp:74) is only compensating for its row-vector matrices (translation in row 3, mi_matrix4.cpp:248-251), and g3blend's (x,z,y,-w) (util.py:35-36) is exactly the Y/Z-swap similarity S·M(q)·S — I verified S·M(q)·S == M(x,z,y,−w) numerically.
Locality is confirmed by rmtools composing child *= parent (mi_xactreader.cpp:24-37) and by my numeric match against .xmot rest poses.
```


## LMA_CHUNK_MESH (id 3, v3) + Submesh + Vertex — the skinned mesh, .xact only

Confidence: high. Source: `F:\Projects\Gothic3\g3blend\g3blend\io\animation\chunks.py:134-232; F:\Projects\Gothic3\g3dit\...\Chunks.java:136-267; rmtools mi_xactreader.cpp:78-142`

```
0x00 u32 nodeNumber      — index into the NODE chunk order of THIS actor (the mesh node, e.g. 'Player')
0x04 u32 numOrgVerts     — original (pre-split) vertex count; the skinning domain
0x08 u32 totalVerts      — sum of submesh vertex counts (after UV/normal splitting)
0x0C u32 totalIndices
0x10 u32 numSubmeshes
0x14 u32 numUvSets       — 1 (485 actors) or 2 (10 actors)
0x18 u8  isCollisionMesh (0 in all shipped data) + 3 junk bytes
0x1C numSubmeshes × Submesh:
     u8  matId            — material index (see eCWrapper_emfx2Actor material refs)
     u8  numUvSets        — per-submesh copy; the ENGINE IGNORES IT and uses the mesh-level value (g3blend chunks.py:172-186)
     u16 junk
     u32 numIndices
     u32 numVerts
     numVerts × Vertex { u32 orgVertex; f32[3] position; f32[3] normal; f32[2] uv[mesh.numUvSets] }  → stride 28 + 8*numUvSets
     numIndices × u32 index — LOCAL to this submesh (0..numVerts-1), triangle list
Verified on 493/494 actors: Σ submesh verts == totalVerts, Σ submesh indices == totalIndices, and AO array length == totalVerts, so the actor-level AO/tangent arrays are indexed by the submesh-concatenated vertex order.
GEOMETRY FACTS (measured on g3_hero_body_player.xact): positions are (x, y, z) with Y UP, in centimetres (bbox -67.7..66.7 x, -2.9..176.5 y, -23.1..23.1 z for a 1.76 m human). Normals are unit. UVs are already in [0,1] with V not flipped relative to the stored data (rmtools negates V only for its 3ds-Max target, mi_coordshifter.cpp:72-75). Winding: cross(b-a, c-a) agrees with the stored vertex normal (mean dot +0.92…+0.98, >99.8% of triangles) → the index order is CCW-front if you consume the stored coordinates in a right-handed system.
NOTE: g3blend/g3dit comment the position/normal as '# Z, Y, X' and provide getPositionXYZ() that swaps x/z — that comment is wrong and the helper is unused; both importers actually use (x, z, y) i.e. a plain Y↔Z swap into Blender/jME Z-up.
Mesh vertices are already in ACTOR/MODEL space (= bind space). Do NOT apply the mesh node's transform: for g3_head_hero_hero_01.xact the mesh node sits at y=163 and the vertices already span y=160.8..198.6; applying the node matrix moves them 167 units away from their bones.
```


## LMA_CHUNK_SKINNINGINFO (id 4, v1) — vertex weights, .xact only

Confidence: high. Source: `F:\Projects\Gothic3\g3blend\g3blend\io\animation\chunks.py:235-272; F:\Projects\Gothic3\g3dit\...\Chunks.java:269-316; rmtools mi_xactreader.cpp:143-177`

```
0x00 u32 nodeNumber   — always equal to MeshChunk.nodeNumber (494/494 actors)
then, ONE RECORD PER ORIGINAL VERTEX, in order 0..numOrgVerts-1, until the chunk size is exhausted (there is no count field):
     u8 influenceCount
     influenceCount × SkinInfluence { u16 nodeIndex; u16 junk; f32 weight }   — 8 B each
Binding to the vertex stream: rendered vertex v uses influences[ mesh.vertices[v].orgVertex ] (g3blend io_import_xact.py:134-141; rmtools rebuilds the same mapping at mi_xactreader.cpp:107-110,153-163).
nodeIndex is an index into the NODE chunk order of the SAME actor (no out-of-range index in any shipped file; each in-file LoD actor has its own node order, e.g. the mesh node is #120 in the LoD actors and #57 in the base actor of g3_fat_body_asstrader.xact).
MEASURED over all 387 files: influences per vertex range 1..20 (histogram 1:578254, 2:144722, 3:99755, 4:90675, 5:42401, 6:38141, 7:15001, 8:8373, 9:6263, 10:4465, 11:2907, 12:2284, 13:2145, 14:1566, 15:925, 16:917, 17:1206, 18:125, 19:9, 20:5). Weights sum to exactly 1.0 for 275194/275194 vertices in a 120-file sample; 14 weights are negative; the influence list is NOT sorted by weight (only 66% descending) → sort, truncate and renormalise yourself for a 4/8-bone GPU path.
The 2 bytes after nodeIndex are uninitialised junk (they carry the same stale value as other junk fields in the same file, e.g. 0x1400/0x0FEF), NOT a high word of a u32 index.
```


## LMA_CHUNK_MATERIAL (id 6, v5) — legacy EMotionFX material, .xact only

Confidence: high. Source: `F:\Projects\Gothic3\g3blend\g3blend\io\animation\chunks.py:387-436; F:\Projects\Gothic3\g3dit\...\Chunks.java:475-526; rmtools mi_xactreader.cpp:178-185`

```
0x00 f32[3] ambient
0x0C f32[3] diffuse
0x18 f32[3] specular
0x24 f32[3] emissive (self-illumination)
0x30 f32 shine
0x34 f32 shineStrength
0x38 f32 opacity
0x3C f32 indexOfRefraction
0x40 u8 doubleSided; u8 wireFrame; char transparencyType ('F' filter / 'S' subtractive / 'A' additive / 'U' unknown); u8 pad
0x44 u32 nameLen + char[]; u32 shaderFileNameLen + char[]   (shader name is empty in all shipped data)
Total = 68 + strings; validated against declared size for all 1716 chunks.
NOTE: FloatColor is 3 floats, not 4 (g3blend io/types/float_color.py:6-16) — this is what makes the 68-byte prefix match rmtools' blind `Skip(68)`.
Chunk index 0 in every actor is the dummy 'EMFX Default'; real materials start at index 1 and their index is what Submesh.matId and MaterialReference.matIndex refer to. For rendering you want the .xshmat named in the actor's material-reference list, not these values.
```


## LMA_CHUNK_MATERIALLAYER (id 7, v4) — texture layer, .xact only

Confidence: medium. Source: `rmtools F:\Projects\Gothic3\rmtools\mimicry\source\Mimicry\mi_xactreader.cpp:186-200 (only reference implementation; g3blend/g3dit treat id 7 as an opaque UnknownChunk)`

```
0x00 u8  mapType     (1 = diffuse, 3 = normal, 9 = specular — rmtools mi_xactreader.cpp:17-22,194-199; values confirmed against G3 texture names ending _Diffuse/_Normal/_specular)
0x01 f32[6] amount / uOffset / vOffset / uTiling / vTiling / rotation  (ALL ZERO in every shipped file — field meaning inferred from EMotionFX, unverified)
0x19 u16 (0)
0x1B u8  (0)
0x1C u32 pathLen + char[pathLen]  — texture base name without extension, e.g. 'G3_Human_Hero_Body_Diffuse_S1' (resolves to .ximg)
Total = 32 + pathLen; validated against declared size for all 1907 chunks. The layer belongs to the preceding MATERIAL chunk.
```


## LMA_CHUNK_LIMIT (id 8, v1) — per-node DOF limits, .xact only

Confidence: low. Source: `Decoded by me from F:\SteamLibrary\steamapps\common\Gothic 3\Data\_compiledAnimation.pak (g3_hero_skeleton.xact); no parser support — g3blend chunks.py:439-466 and g3dit Chunks.java:528-565 fall through to UnknownChunk`

```
Always exactly 88 bytes (2039/2039 chunks):
0x00 u32 nodeNumber
0x04 f32[3] translationMin
0x10 f32[3] translationMax
0x1C f32[3] rotationMin (radians)
0x28 f32[3] rotationMax
0x34 f32[3] scaleMin
0x40 f32[3] scaleMax
0x4C 12 bytes — per-channel enable flags (9 bytes in EMotionFX) + junk; observed tail e.g. 00 00 00 00 00 00 00 00 00 40 da 12 (last bytes are clearly stale memory)
Evidence for the grouping: the scale pair reads (1,1,1)/(1,1,1) and the translation/rotation pairs read 0/0 in the sample I decoded, which is self-consistent, but no parser decodes this chunk so the field order is inferred, not proven. Not needed for skinning/animation playback.
```


## LMA_CHUNK_PHYSICSINFO (id 9, v1) — per-bone ragdoll/collision shape, .xact only

Confidence: low. Source: `Decoded by me from _compiledAnimation.pak (g3_hero_skeleton.xact, g3_alligator_body_01.xact); no parser support (UnknownChunk in g3blend/g3dit, ignored by rmtools)`

```
Always exactly 72 bytes (1228/1228 chunks):
0x00 char[36] bone name, NUL-terminated, uninitialised junk after the NUL (e.g. 'Aligator_Head_Head_1\0' + garbage)
0x24 u32 (== 1 in every sample; type/flags?)
0x28 f32[8]  observed pattern (0,0,0, a,b,c, d, 0) with a,b,c dimension-like (27.4/44.4/115.2 for a crocodile head, 24.5/24.5/13.5 for a neck) and d a small scalar (5, 7, 10)
Interpretation (offset vec3, half-extents/radius vec3, radius-or-mass, unused) is a guess; nothing decodes this chunk. Present only in skeleton-bearing actors, always references bones by name. Irrelevant to rendering/animation.
```


## LMA_CHUNK_SCENE_INFO (id 16, v1) — exporter metadata, .xact only

Confidence: high. Source: `Decoded by me from _compiledAnimation.pak (all 387 .xact files scanned); no parser support (UnknownChunk in g3blend/g3dit)`

```
0x00 u32 — low word constant 0x1002, high word is junk (it is literally the same stale value that shows up in the submesh padding of the same file)
0x04 u32 — always 1
0x08 u32 len + char[len]  exporter, e.g. '3D Studio Max 7 (regular commercial version)'
     u32 len + char[len]  source path, e.g. 'Z:\GothicIII_Figuren\Humans\Paladin_HERO\Hero_Skeleton_Limits_02.max'
     u32 len + char[len]  build date, e.g. 'Mar  1 2005'
Matches the declared chunk size in 499/501 actors; the two exceptions are a shipped bug (see gotchas). Purely informational.
```


## .xmot top level — eCResourceAnimationMotion_PS (version 5) + eSFrameEffect

Confidence: high. Source: `F:\Projects\Gothic3\g3blend\g3blend\io\animation\xmot.py:8-19,58-86; F:\Projects\Gothic3\g3dit\...\eCResourceAnimationMotion_PS.java:17-32,88-98; SDK F:\Projects\Gothic3\gothic3sdk\g3\Engine\include\g3sdk\Engine\ge_resourceanimationmotion_ps.h:12-15,49 and ...\animation\ge_visualanimation_ps.h:268`

```
0x0E u16 version              (5 in all 5937 shipped files; reader branches exist for >=2 and >=3)
0x10 u32 resourceSize         (0 in shipped files)
0x14 f32 resourcePriority     (junk)
0x18 u64 nativeFileTime       (FILETIME)
0x20 u32 nativeFileSize
0x24 u64 second FILETIME      (only if version >= 3; both parsers label it 'unknown', probably the source actor's timestamp)
0x2C u16 frameEffectCount     (only if version >= 2)
     count × eSFrameEffect { u16 keyFrame; u16 stringTableIndex(effectName) }   — 4 B each
     then eCWrapper_emfx2Motion
FRAME EFFECTS (the 'event markers' g3blend exports): keyFrame is a FRAME INDEX on the file's key grid, i.e. t = keyFrame / fps with fps = 1/baseDelta (25 in practice), NOT a time and not a key ordinal of a particular track. Measured over a 1500-file sample: 1126 files have none, the rest 1..7 (max 7); 405 indices fall inside [0, frameCount), 2 land exactly on frameCount and 8 beyond it. Names are effect/sound resources, e.g. 'eff_step_creature01_walk_earth_01', 'EFF_Ani_Fight_Whoosh_Normal_01', occasionally with a '.wav' suffix. The engine consumes them in eCVisualAnimation_PS::UpdateFrameEffects().
```


## eCWrapper_emfx2Motion ('LMA ' block)

Confidence: high. Source: `F:\Projects\Gothic3\g3blend\g3blend\io\animation\xmot.py:22-55; F:\Projects\Gothic3\g3dit\...\eCResourceAnimationMotion_PS.java:34-74 (rmtools has NO .xmot support at all — `grep -ril xmot` over the whole repo hits nothing but an unrelated .resX)`

```
+0  u32     payloadSize — measured from the byte AFTER this field; chunkStreamEnd = pos_after_size + payloadSize
+4  char[4] 'LMA '
+8  u8      highVersion (1)
+9  u8      lowVersion  (1)
+10 u8      isActor — must be 0 for a motion (the same wrapper class can hold an actor)
+11 ...     LMA chunk stream (only ids 1 and 2 occur) until chunkStreamEnd
Nothing follows the chunk stream: chunkStreamEnd == the container's DEADBEEF offset in 1200/1200 sampled files.
```


## LMA_CHUNK_MOTIONPART (id 1, v3) — one animated node, .xmot only

Confidence: high. Source: `F:\Projects\Gothic3\g3blend\g3blend\io\animation\chunks.py:348-376; F:\Projects\Gothic3\g3dit\...\Chunks.java:422-452; usage in g3blend operators\io_import_xmot.py:107-141`

```
0x00 f32[3] posePosition     — rest/reference LOCAL translation for this part
0x0C f32[4] poseRotation     — rest LOCAL quaternion (x,y,z,w)
0x1C f32[3] poseScale        — rest LOCAL scale (1,1,1) in 18755/18856 sampled parts
0x28 f32[3] + f32[4] + f32[3] — 40 bytes that g3blend/g3dit call bindPosePosition/Rotation/Scale.
              *** THESE 40 BYTES ARE UNINITIALISED GARBAGE IN EVERY SHIPPED FILE ***
              Over 18856 sampled motion parts: 0 have a unit-length 'bind' quaternion, 0 have 'bind' scale ≈ 1, 0 equal the pose block; the values are denormal noise like 5.5e-12 / 2.8e-45. Ignore them.
0x50 u32 nameLen + char[nameLen]
Total = 84 + nameLen; validated against declared size for 75362 chunks.
The name is matched to a skeleton node BY NAME; a file also contains parts for non-bones (Max helpers such as 'Dummy01', 'Fbx_Root', '!DMW_Aligator_BaseMotion_Spline', 'Slot_RightHand_Weapon') — skip unknown names. 130 duplicate names occur across the set (always helpers such as 'Fbx_Root').
157515 of 374764 motion parts carry NO keyframe chunk at all — such a part just holds its pose (this is why g3blend synthesises a key on import, io_import_xmot.py:287-312).
A part never carries two tracks of the same type (checked on 200 files): at most one P, one R, one S.
```


## LMA_CHUNK_ANIM (id 2, v1) — one keyframe track, .xmot only

Confidence: high. Source: `F:\Projects\Gothic3\g3blend\g3blend\io\animation\chunks.py:275-345; F:\Projects\Gothic3\g3dit\...\Chunks.java:318-420; fps handling g3blend operators\io_import_xmot.py:57-81 and io_export_xmot.py:189`

```
0x00 u32  keyCount
0x04 char interpolationType — 'L' Linear, 'B' Bezier, 'T' TCB. ALL 227807 shipped tracks are 'L'.
0x05 char animationType     — 'P' position, 'R' rotation, 'S' scale. Counts: R 214915, P 12883, S 9.
0x06 u16  junk (NOT zero — e.g. 0x00c6; do not assert)
0x08 keys:
       'R': { f32 time; f32[4] quat(x,y,z,w) }  → 20 B
       'P' / 'S': { f32 time; f32[3] value }    → 16 B
Total = 8 + keyCount*stride; validated for 46081 chunks. The track applies to the most recent MOTIONPART chunk. NO COMPRESSION and NO QUATERNION PACKING — raw float32 everywhere (this is the whole reason .xmot files are large).
MEASURED TIMING: time is absolute seconds from the start of the motion. First key is at t=0 (only 31 tracks of 227807 start later). Key times lie on a uniform grid: for 771/775 sampled files every key time in every track is an integer multiple of the file's smallest key delta. That delta is 0.04 s (25 fps) in 752/775 files, 1/30 s in 9, and a multiple of 0.04 (0.08/0.12/0.16/0.4) in the rest — i.e. still a 25 Hz grid with thinned keys. Per-file derived fps over all 5937 files: 25.0 for 5613 files, 30.0 for 62, 12.5 for 46, the remainder odd. Redundant keys are dropped PER CHANNEL, so different tracks in the same file have different key times (identical time sets in only 23/775 files).
Duration = max key time over all tracks; frame count = maxTime/baseDelta + 1 (integral in 5727/5779 files). Typical clips are 11, 21, 10, 26, 16, 51 frames; longest track seen 526 keys.
QUATERNION HYGIENE: 71 of 130080 sampled rotation keys are not unit length; 105 of 124334 consecutive key pairs have dot<0 (hemisphere flip) → normalise and take the shortest path before lerp/slerp.
LOOPING: in '_loop_' clips the last key equals the first (mean |q_first − q_last| ≈ 0.000–0.002 over rotation tracks), whereas '_begin_'/'_end_' clips differ by 0.03–0.30. So a looping clip's period is the last key time, and you must not add an extra frame.
```


## RECIPE (a): build the bind-pose skeleton

Confidence: high. Source: `rmtools mi_xactreader.cpp:24-37 + 203-206 (identical composition); g3blend operators\io_import_xact.py:200-257; verified by me in scratchpad\skel\pipeline.py`

```
1. Read all NODE chunks of the chosen actor in file order → array N[0..n-1] with (name, parentName, pos, rot, scale).
2. Resolve parents by NAME into indices (two passes; the array is not sorted, and 'no parent' = empty parent string). Root count is 2 in 473 of 501 actors (the skeleton root, e.g. 'Hero_ROOT', plus the mesh node, e.g. 'Player'), 1 in 14, 3 in 12.
3. Local matrix L_i = T(pos) · R(quat as-is) · S(scale)  (column-vector convention; if you use row vectors, transpose, which is what rmtools' q.Inverse() is doing).
4. Global bind B_i = B_parent · L_i, roots B_i = L_i.
5. Keep B_i^-1 for skinning. Nothing else is needed — the .xact stores no inverse-bind matrices.
Sanity check performed: composing this and comparing against the .xmot rest poses reproduces them to <1e-3 cm / <1e-4 quaternion distance for every bone that has no rotation track.
```


## RECIPE (b): skin the mesh

Confidence: high. Source: `g3blend operators\io_import_xact.py:123-141 (vertex-group binding via orgVertex); SDK F:\Projects\Gothic3\gothic3sdk\g3\Engine\include\g3sdk\Engine\animation\ge_wrapper_emfx2actor.h:95-96 and ...\ge_visualanimationlod.h:37; verified in scratchpad\skel\pipeline.py`

```
Vertex buffer = concatenation of the submeshes (keep the per-submesh index base). Vertex v carries orgVertex o.
For each influence (nodeIndex j, weight w) in SkinningInfo.influences[o]:
   skinMatrix_j = G_j · B_j^-1        (G_j = current global of node j, B_j = global bind)
   p' = Σ w · (skinMatrix_j · p)      with p = the stored position (already in model/bind space)
Normals: transform with the same matrices (inverse-transpose is unnecessary while scale is uniform, which it is except for 58 mirror nodes).
Influence counts go up to 20 and are unsorted: sort by weight, keep 4 (or 8), renormalise. Note the shipped engine split the mesh into 'hard' single-bone buffers and 'soft' multi-bone buffers with a per-LoD bone palette map (eCWrapper_emfx2Actor::FillHardBuffersAndPrimitves / FillSoftBuffers, eCVisualAnimationLoD::GetHardBufferBoneMap) — a modern runtime does not need that split.
I validated the whole chain numerically: bind mesh bbox 176.5 cm tall, arms spread ±67 (T-pose); after applying an idle clip the skinned mesh is 175.5 cm tall with arms at ±38 (arms down); after a run clip it is 165 cm with ±67 in Z (stride). Nothing explodes, weights sum to 1.
```


## RECIPE (c): play one motion — the hierarchy-cleanup step you cannot skip

Confidence: high. Source: `SDK ge_wrapper_emfx2actor.h:81-82 (CleanUpHierachy); g3blend operators\io_import_xact.py:176-197 (the exact name rule) and io_import_xmot.py:107-141,268-312; measured in scratchpad\skel\fold_check.py, static_check.py, pipeline.py`

```
*** .xmot MOTIONPART transforms are expressed against the CLEANED hierarchy, not the raw .xact node tree. ***
The engine runs eCWrapper_emfx2Actor::CleanUpHierachy, which deletes every node whose name ends in '_ROOT' or '_END' AND has more than two '_'-separated components (so 'Hero_ROOT' survives, 'Hero_Left_Arm_Arm_ROOT' does not), folding the deleted node's transform into its children.
Proof (g3_hero_body_player.xact vs hero_stand_1h_none_p0_move_run_...xmot): comparing MotionPart.posePosition against the raw .xact node gives errors of 10–91 cm; after folding the helper chain (fold: p' = p_helper + R_helper·p_child, q' = q_helper·q_child) the error is 0.0000–0.0022 cm and, for bones with no rotation track, the rotations match to 1e-4 as well.
End-to-end proof: skinning with folding gives a correct human (bbox x −40.8..33.5, y −2.5..165.1); applying the same motion values to the raw hierarchy sends the mesh to x −239.9..−92.1 (mean vertex error 142 cm).
Sampling one frame:
  for each cleaned node c:  local = TRS(pose) of its MotionPart, overridden per channel by the sampled track value (P→translation, R→rotation, S→scale); if no MotionPart matches the name, use the node's folded rest local.
  G_c = G_effectiveParent · local, walking the cleaned tree; keep the mapping cleaned-node → original node index so B_i^-1 (from recipe a) still applies for skinning.
  Interpolation: linear in time, hold before the first / after the last key; for quaternions negate the second key when dot<0 and nlerp+normalise (or slerp).
ROOT MOTION: there is no dedicated root channel. Position tracks are rare (12883 of 227807) and sit almost exclusively on the pelvis (*_Spine_Spine_1) plus weapon slots; over a 0.8 s 'run' clip the pelvis moves ~5 cm, i.e. the clip is in-place and world locomotion comes from outside the file (the speed token in the file name, e.g. '..._p0_400', and eCVisualAnimation_PS::ConstrainRootToWorld).
```


## LOD levels

Confidence: high. Source: `SDK ge_wrapper_emfx2actor.h:90,103,109,133 and ...\ge_visualanimationlod.h:31-44; g3blend io\animation\xact.py:120-134 (lods list); measured across _compiledAnimation.pak`

```
TWO independent mechanisms, both in use:
1. Separate files: g3_<name>.xact, g3_<name>_lod1.xact, g3_<name>_lod2.xact. The engine binds one eCVisualAnimationLoD per level, each holding an actor FILE NAME (eCVisualAnimationLoD::m_strActorFileName / SetActorFileName); the base file comes from eCVisualAnimation_PS.ResourceFilePath.
2. In-file: eCResourceAnimationActor_PS.lodActorCount > 0 in 62 of 387 shipped files; each entry is a COMPLETE eCWrapper_emfx2Actor (own NODE array, own MESH, own SKININFO, own materials/AO/tangents), consumed by eCWrapper_emfx2Actor::AddLoDLevel / GetLoDCount / SetCurrentLoDLevel.
ORDER (proven): in g3_fat_body_asstrader.xact the in-file actors have 961, 1913 and 3771 original vertices; g3_fat_body_asstrader_lod1.xact contains exactly the 1913-vertex mesh and _lod2.xact exactly the 961-vertex one. So the lods[] array is stored COARSEST FIRST and the trailing 'actor' field is LOD0 (highest detail): lods[i] == LOD level (lodActorCount − i).
Each LoD actor has its own node ordering (the mesh node is #120 in the LoD actors vs #57 in the base actor of that file), so bone indices are never shared across levels.
The MaterialReference.lodIndex field is 0 in all 1136 shipped references, so the 'MaterialID = matIndex | lodIndex' comment in g3blend/g3dit cannot be confirmed from data.
```


### Gotchas

- Endianness/alignment: everything is little-endian, tightly packed, no alignment padding. Every field either parser calls 'padding' is uninitialised stack garbage — the submesh 2-byte pad, the SkinInfluence 2-byte pad, the ANIM chunk's u16 pad (seen as 0x00c6), the SCENE_INFO first u32's high word, and the whole 40-byte 'bind pose' block of MOTIONPART. Never validate them, never assume zero, never write them back expecting a byte-identical round-trip.
- rmtools/Rimy3D CANNOT read .xmot at all — grep for 'xmot' over the whole rmtools tree hits nothing but an unrelated Risen .resX. For motions the only references are g3blend (Python) and g3dit (Java), and those are the same author's code (georgeto), so they are ONE source, not two. Every .xmot statement in this report that is not a plain field offset was re-verified by me directly against 5937 shipped files.
- rmtools' .xact reader is hard-coded to `Seek(74)` for the 'gena' size field (mi_xactreader.cpp:46), which silently assumes zero look-at constraints AND zero in-file LoDs. 62 of 387 shipped .xact files have in-file LoDs and 4 have look-at constraints (e.g. g3_hero_skeleton.xact, whose header ends at 60 + 4 constraints × 30 B), so Rimy3D mis-parses them. Parse the header properly instead of seeking.
- Two shipped files have a WRONG SCENE_INFO chunk size: g3_head_hero_diego_01.xact declares 128 where the content is 152, and g3_head_hero_diego_animated_01.xact declares 144 vs 186. A strict size-driven chunk walk desyncs and then reads a garbage chunk id/size. Guard: for chunk id 16 parse the content (2 u32 + three u32-length-prefixed strings) and advance by max(declaredSize, parsedSize), or simply abort the chunk stream when the next chunk header is implausible.
- Node parents are stored as NAMES, not indices, and the node array is NOT topologically sorted (5065 nodes across the shipped set have a parent that appears later). Build a name→index map first, then compute globals recursively/memoised.
- Node index spaces are per-actor: MeshChunk.nodeNumber and SkinInfluence.nodeIndex index the NODE chunks of the SAME eCWrapper_emfx2Actor. In a file with in-file LoDs the same bone has different indices in each LoD actor (mesh node #120 vs #57 in g3_fat_body_asstrader.xact).
- THE BIG ONE for animation: .xmot motion parts address the CLEANED skeleton (eCWrapper_emfx2Actor::CleanUpHierachy removes '*_ROOT'/'*_END' nodes that have more than two '_'-separated name parts and folds their transform into the children). Driving the raw .xact hierarchy with .xmot values double-counts those helper transforms: measured pose mismatch of 10–91 cm per bone and a fully broken skinned mesh (mean vertex error 142 cm). Fold first, and keep cleaned-node → original-node-index mapping so skinning indices still resolve.
- MOTIONPART's second TRS block (named bind_pose_position/rotation/scale in g3blend and g3dit) is uninitialised junk in 100% of shipped files (0 of 18856 sampled parts have a unit 'bind' quaternion or scale ≈ 1). The usable rest pose is the FIRST block (pose_*), and only for channels that have no track — for animated channels it may differ from the actor's bind pose.
- Quaternion convention: component order on disk is x,y,z,w, and the value is the plain local rotation in the file's own left-handed, Y-up, centimetre space. Both reference implementations appear to 'invert' it, but neither actually does: rmtools uses row-vector matrices (translation in row 3, mi_matrix4.cpp:248-251), so q.Inverse() is just a transpose; g3blend's to_blend_quat = (x, z, y, −w) is exactly the Y↔Z mirror similarity — I verified numerically that S·M(q)·S == M(x,z,y,−w). If you keep the file's axes, apply the quaternion as-is with the standard formula.
- Coordinate system / winding: X right, Y up, Z depth, units = centimetres (a human is ~176 tall, hips at y≈103). Triangle indices are CCW-front when the stored (x,y,z) are read as a right-handed frame (cross(b−a,c−a)·N ≈ +0.92…+0.98 over every actor tested, mirrored 'scale = −1' actors included). Any axis swap you introduce (e.g. Y↔Z to get Z-up) is a mirror and flips winding — rmtools compensates by swapping vertices A and C (mi_coordshifter.cpp:43-53).
- Mesh vertices/normals are already in ACTOR (bind) space — do NOT apply the mesh node's transform. 59 of 494 actors have a non-identity mesh node (offsets, 90/180° rotations, even uniform −1 'mirror' scale on some head actors); those values are leftover 3ds Max node data. Proof: g3_head_hero_hero_01.xact's mesh node sits at y=163 while the vertices already span y 160.8..198.6, and applying the node matrix pushes them 167 units away from their own bones.
- Submesh indices are LOCAL to the submesh (0..numVerts−1). If you merge submeshes into one vertex buffer you must add the running vertex base (rmtools does this at mi_xactreader.cpp:125-131). Also, the per-submesh numUvSets byte is unreliable — the engine uses the mesh-level numUvSets for the vertex stride (both g3blend and g3dit override it explicitly).
- Skinning weights: up to 20 influences per vertex (histogram peaks at 1 and 2 but 1206 vertices have 17), NOT sorted by weight (~34% of vertices are not descending), sums are exactly 1.0, and 14 negative weights exist in the shipped set. Sort, clamp to your 4/8-bone limit, renormalise, and clamp negatives.
- The skinning record count is implicit: SkinningInfo has no vertex count — you read u8-prefixed influence lists until the chunk size is consumed, and the i-th record belongs to original vertex i (0..numOrgVerts−1). Rendered vertices reach it through Vertex.orgVertex (orgVertex is monotonically non-decreasing within a submesh and covers 0..numOrgVerts−1 across all submeshes).
- Keyframe data is completely uncompressed: raw float32 time + float32[3]/[4] value, 16/20 bytes per key. There is no quaternion packing, no key-time quantisation field, no per-track time base. The 'compression' is only redundant-key removal, done per channel, which is why tracks in one file have different key times.
- There is NO fps or duration field anywhere in .xmot. Times are absolute seconds; 25 Hz is a property of the data, not of the format (g3blend hard-codes 25 on both import and export — io_import_xmot.py:57-81, io_export_xmot.py:189 — while g3dit's commented-out algorithm derives fps = 1/min-positive-delta and throws if it isn't integral). Measured: 5613 of 5937 files sit on a 0.04 s grid, 62 on 1/30 s, 46 report 12.5 (= a thinned 25 Hz grid), and 4 sampled files are not on a uniform grid at all. Compute duration as the max key time and do not assume 25 fps.
- Frame effects index FRAMES, not keys and not seconds: t = keyFrame / fps. A handful (8 of 415 in a sample, plus 2 exactly at frameCount) point at or past the last frame — clamp instead of asserting.
- Looping clips duplicate the first key at the end (|q_first − q_last| ≈ 0 for '_loop_' names, 0.03–0.30 for '_begin_/_end_'). Loop period = last key time; adding one more frame double-counts a frame.
- Only 'L' (linear) interpolation occurs in shipped data (227807/227807 tracks); 'B' (Bezier) and 'T' (TCB) are defined but never used, and g3blend's importer raises on TCB. Rotation keys are nearly always unit and hemisphere-aligned, but not always (71/130080 non-unit, 105/124334 pairs with dot<0) — normalise and take the shortest path.
- Motion parts exist for non-bones (3ds Max helpers: 'Dummy01', 'Fbx_Root', 'Paladin', '!DMW_Aligator_BaseMotion_Spline', 'Slot_*'), and 157515 of 374764 parts carry no track at all. Match by name against your skeleton and silently skip the rest; duplicated part names occur (130 cases, all helpers).
- Version gates observed in the shipped set — reject anything else rather than guessing: eCResourceAnimationActor_PS = 54 (387/387), 'gena' = 4, 'FXA ' = 1.1, eCResourceAnimationMotion_PS = 5 (5937/5937), 'LMA ' = 1.1 with isActor = 0; chunk versions NODE 3, MOTIONPART 3, ANIM 1, MESH 3, SKININFO 1, MATERIAL 5, MATERIALLAYER 4, LIMIT 1, PHYSICSINFO 1, SCENE_INFO 1.
- FloatColor in the MATERIAL chunk is 3 floats (RGB), not 4 — this is what makes the material prefix 68 bytes and matches rmtools' blind Skip(68). Getting this wrong overruns the 87/88-byte material chunks.
- Material index 0 in every actor is a dummy called 'EMFX Default'; the real material for a submesh is looked up through the actor's trailing material-reference list (matIndex == submesh.matId) and is a .xshmat resource name from the genome string table. The embedded MATERIAL/MATERIALLAYER chunks are legacy authoring data (all layer float parameters are zero).
- The actor's ambient-occlusion and tangent arrays live AFTER the chunk stream, are sized by MeshChunk.totalVerts, and are indexed in submesh-concatenation order. The tangent array has no count of its own — you must carry the count over from the AO list. AO values are u32 with the low 24 bits always zero (0xNN000000), i.e. a single 8-bit occlusion term in the most significant byte.


### Open questions

- Playback semantics live outside the file: loop flag, blend-in/out, play speed, motion slot (eEMotionType), and the mapping motion→actor all come from the entity/template side (eCVisualAnimation_PS.ResourceFilePath, eSMotionDesc, SetPlaySpeed/SetPlayTime/GetMaxTime in ge_wrapper_emfx2actor.h:111-140) plus the file-name convention (…_loop_…, …_p0_400 where 400 looks like cm/s). I did not decode Templates.pak, so the exact mapping and the meaning of the numeric name tokens are unverified.
- Root motion: no root channel exists in the format and the pelvis translation over a 'run' clip is only ~5 cm, so world locomotion must be synthesised by the runtime. Exactly how the original engine does it (speed from the name? from the template? ConstrainRootToWorld?) is unknown.
- Whether the engine interpolates rotations with slerp or nlerp, and whether it evaluates continuous time or snaps to the 25 Hz grid, is unknown — the SDK only exposes seconds-based GetPlayTime/GetMaxTime/SetPlaySpeed. My recommendation (shortest-path nlerp + normalise) matches the data but is not proven against the engine.
- The 4 sampled .xmot files whose key times are not integer multiples of the smallest delta (e.g. orc_lieknockout_none_2h_p0_ambient_loop_n_fwd..., dragon_stand_none_cast_p0_move_stand_n_fwd...) and the 31 tracks whose first key is not at t=0 were not investigated further.
- LMA_CHUNK_LIMIT (id 8) field order (translation/rotation/scale min-max pairs) and its 12-byte tail (activation flags?) are inferred from one decoded sample; no parser decodes this chunk. Same for LMA_CHUNK_PHYSICSINFO (id 9): 36-byte name buffer + u32 + 8 floats is confirmed by size, but the float semantics (capsule/box dimensions? mass?) are a guess.
- MATERIALLAYER's 24 bytes of float parameters are all zero in every shipped file, so the field order (amount, uOffset, vOffset, uTiling, vTiling, rotation) is inherited from EMotionFX convention and unverified; likewise the map-type codes beyond 1/3/9 that rmtools handles.
- MaterialReference.lodIndex is 0 everywhere, so the 'MaterialID = matIndex | lodIndex' combined-key comment in g3blend/g3dit cannot be confirmed — nor whether a per-LoD material override was ever intended.
- Negative uniform scale (−1,−1,−1) appears on 58 nodes of some head actors (and on some mesh nodes). Since the mesh is stored pre-transformed, this seems inert, but I did not test whether the engine applies it to attachments or to child bones — if it does, winding/normals for those actors would need flipping.
- scaleOrient (the second node quaternion) and shear are carried but never exercised in shipped data (shear is exactly 0 everywhere; scale is uniform), so the composition order for a non-uniform scale case (EMotionFX applies scaleRot⁻¹·S·scaleRot) is untested here.
- Facial animation is out of scope of the files I scanned: eCVisualAnimation_PS.FacialAnimFilePath and the phoneme chunk id 12 exist in the enum, but no shipped .xact/.xmot in _compiledAnimation.pak contains chunk ids 5, 10, 11, 12 or 13.
- The 'gena' actor's nativeFileSize equals the FXA payload size in the files I checked, and resourceSize is always 0 with a junk priority — whether the engine ever relies on those numbers (streaming budget?) is unknown.
- How .xmot files are associated with an actor at runtime (name prefix? template list? both) was not established; motion-part names match node names, which is sufficient for a runtime that is told which pair to load.

## `.lrentdat` - the people, and everything else that is not scenery

A sector says what the world is built from: meshes, trees, lights. It says
nothing about who is in it. That is in 115 `.lrentdat` files beside them, and
their names say what they hold - `ardea_npc_01`, `faring_npc_02`,
`cpt_quest_npc_01` through 05, `story_xardas`, `zarkos_camp`.

`g3ent` reads them. Across the 97 that are wrapped: **35437 entities, 16604
wearing an actor, 10279 carrying `gCNPC_PS`**, 17259 `gCItem_PS`, 8073 with
dialogue and 7651 with a daily routine. `CPT_Goblin_01` sits at
(-399, -318, -163225) wearing `G3_Goblin_Body_01.FXA`.

The layout, which is a sector's with two differences:

    GENOMFLE wrapper and string table   - the same sniff a sector uses
    "GENOMEDL"                          - a sector does not name itself again
    u16 83                              - the same archive version
    eCEntityDynamicContext              - a sector goes straight to the count
    i32 entityCount
      each: u16 0x40, u16 0x53, a creator flag and 20 bytes if set,
            then the same 298-byte entity body a sector has
    pairs of (parent, child) indices, ended by -1

Two things cost time and are worth stating.

**A class header's size is a fallback, not an end.** `eCEntityDynamicContext`
declares 6957 bytes and occupies 124. Seeking to the declared end lands in the
string table at the back of the file and the entity count comes out as
2147483848. The class has to be walked: its properties, its version, then its
own tail of a flag, two culling factors and a box - thirty-three bytes.

**The entity body cannot be walked field by field.** The engine's file order is
not the member order in its headers, which is why the sector reader has fixed
offsets taken from the data - guid at 4, name at 41, matrix at 43, property set
count at 294, the whole body 298. Reading it in header order gives a render
alpha of zero and a scale of NaN. The same offsets work here unchanged; only the
prologue in front of the body differs.

The 18 files that do not read are the ones named for a guid rather than a place,
plus `g3_startup/sysdyn_*`. They are not wrapped at all - they open straight
into `eCEntityDynamicContext` with no string table - so they are a second
variant rather than a failure of this one.

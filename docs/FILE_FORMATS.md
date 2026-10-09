# Unigine 1.x File Format Specifications

Reverse-engineered and documented from the Unigine Heaven Benchmark (Engine version 1.x). Format details were derived from a combination of the [Unigine 1.0 SDK documentation](https://developer.unigine.com/en/docs/1.0/code/file_formats), forum posts, and direct binary analysis.

## Table of Contents

- [UNG Archive Format](#ung-archive-format)
- [mi08 Static Mesh Format](#mi08-static-mesh-format)
- [xml0 Binary XML Format](#xml0-binary-xml-format)
- [Transform Matrix Convention](#transform-matrix-convention)
- [Normal Compression](#normal-compression)

---

## UNG Archive Format

UNG files (`.ung`) are Unigine's encrypted and compressed asset archives. The extractor (`uniginex`) supports versions 3 and 4.

### Signature

| Offset | Type    | Description                          |
|--------|---------|--------------------------------------|
| 0      | char[2] | Magic: `"ar"`                        |
| 2      | char[2] | Version digits, e.g. `"03"` or `"04"` |

The version is decoded as: `(byte[2] - '0') * 10 + (byte[3] - '0')`.

### Version 3 (Zlib Compression)

After the 4-byte signature, the file contains a sequence of entries until EOF. Each entry is encrypted using a seed-based cipher with a game-specific key.

**Per-entry structure (encrypted reads):**

| Field         | Read Function     | Description                              |
|---------------|-------------------|------------------------------------------|
| name_crc      | `unigine_get32`   | CRC32 of the filename (for key validation) |
| filename      | `unigine_getss`   | Null-terminated encrypted string         |
| size          | `unigine_get32`   | Uncompressed file size                   |
| zsize         | `unigine_get32`   | Compressed data size                     |
| bsize         | `unigine_get32`   | Block size (≥ zsize)                     |
| crc           | `unigine_get32`   | Data checksum                            |
| data          | `unigine_getmm`   | Encrypted + zlib-compressed file data    |

### Version 4 (LZ4 or Zlib Compression)

| Offset | Type   | Description                                |
|--------|--------|--------------------------------------------|
| 0      | char[4]| Signature `"ar04"`                         |
| 4      | uint32 | Compression type: 0=none, 1=LZ4, 2=zlib   |
| EOF-8  | uint64 | Offset to the file index table             |

The index table at the end of the file contains the same per-entry structure as v3, but with 64-bit offsets for the data blocks. Data blocks are stored separately from the index.

### Encryption

The encryption uses a seeded PRNG with bitwise rotation (ROL/ROR) and XOR operations. The seed is initialized to `0xa13cdbde` and advanced via:

```
seed = seed * 0x6b982
seed = seed % 0x7fffffab
```

When a key is provided, the seed is further scrambled using key bytes. The tool automatically tries known keys for several games:

| Game          | Key                                |
|---------------|------------------------------------|
| Cradle        | `fS5g_2gDc*`                       |
| Sumoman       | `1EuCM90tADY6uxkJHXyCGj0PcJ1EWzvL`|
| Superposition | `ieNgeivaep9eima7ze8m`             |
| Oil Rush      | `CF29hity73GT640LP1dlfu55gzxyeuas` |

Heaven Benchmark archives use no encryption key (the default seed alone is sufficient).

### CRC32

Filenames are validated using a standard CRC32 (polynomial 0xEDB88320, initial value 0xFFFFFFFF, final XOR 0xFFFFFFFF). If the CRC doesn't match, the tool cycles through known keys until one works.

---

## mi08 Static Mesh Format

The `mi08` format is Unigine 1.x's binary mesh format for static (non-animated) geometry. All data is little-endian.

### Reference

[Unigine 1.0 SDK — Mesh File Formats](https://developer.unigine.com/en/docs/1.0/code/file_formats)

### Overview

```
┌─────────────────────────────────────┐
│ Header                              │
│   Magic (4 bytes)                   │
│   Mesh bounding volume (40 bytes)   │
│   Number of surfaces (4 bytes)      │
├─────────────────────────────────────┤
│ Surface Headers (per surface)       │
│   Name string                       │
│   Surface bounding volume (40 bytes)│
├─────────────────────────────────────┤
│ Surface Data (per surface)          │
│   Vertices + normals                │
│   UV set 0                          │
│   UV set 1                          │
│   Triangle indices                  │
└─────────────────────────────────────┘
```

### Detailed Layout

#### 1. File Header

| Offset | Type     | Description                                              |
|--------|----------|----------------------------------------------------------|
| 0      | int32    | Magic: `0x3830696D` = `'m' \| ('i'<<8) \| ('0'<<16) \| ('8'<<24)` |
| 4      | float[3] | Mesh bounding box minimum (x, y, z)                      |
| 16     | float[3] | Mesh bounding box maximum (x, y, z)                      |
| 28     | float[3] | Mesh bounding sphere center (x, y, z)                    |
| 40     | float    | Mesh bounding sphere radius                              |
| 44     | int32    | Number of surfaces                                       |

#### 2. Surface Headers (repeated for each surface)

| Type     | Description                                                |
|----------|------------------------------------------------------------|
| int32    | Name length (including null terminator, max 1024)          |
| char[]   | Surface name (null-terminated, `name_length` bytes)        |
| float[3] | Surface bounding box minimum                               |
| float[3] | Surface bounding box maximum                               |
| float[3] | Surface bounding sphere center                             |
| float    | Surface bounding sphere radius                             |

#### 3. Surface Data (repeated for each surface, immediately after all headers)

**Vertices:**

| Type     | Description                                    |
|----------|------------------------------------------------|
| int32    | Number of vertices (`num_verts`)               |

For each vertex (`num_verts` times):

| Type       | Description                                  |
|------------|----------------------------------------------|
| float[3]   | Vertex position (x, y, z)                    |
| uint16[3]  | Compressed normal (see [Normal Compression](#normal-compression)) |

Each vertex is 18 bytes (12 for position + 6 for normal).

**Texture Coordinates (UV Set 0):**

| Type     | Description                                    |
|----------|------------------------------------------------|
| int32    | Number of texture coordinates (`num_tc0`)      |

For each texcoord:

| Type     | Description                                    |
|----------|------------------------------------------------|
| float[2] | UV coordinates (u, v)                          |

**Texture Coordinates (UV Set 1):**

Same structure as UV Set 0. Often `num_tc1 = 0` (no lightmap UVs).

**Triangle Indices:**

| Type     | Description                                    |
|----------|------------------------------------------------|
| int32    | Number of triangles (`num_tris`)               |

For each triangle (3 indices per triangle):

| Condition              | Index Type    | Bytes per Triangle |
|------------------------|---------------|--------------------|
| `num_verts < 65536`    | uint16[3]     | 6                  |
| `num_verts >= 65536`   | int32[3]      | 12                 |

### Example: barrel.mesh

```
Magic: mi08
Mesh BBox: (-0.488, -0.479, -0.000) to (0.480, 0.483, 0.981)
Surfaces: 2
  [0] "powder_keg"      — 1201 verts, 1201 UVs, 1200 tris
  [1] "powder_keg_sign" —   12 verts,   12 UVs,    8 tris
Total: 1213 vertices, 1208 triangles, 38981 bytes
```

---

## xml0 Binary XML Format

Unigine `.node`, `.world`, `.mat`, and `.prop` files use a binary XML format identified by the `xml0` magic. This is the serialized form produced by `Xml::save(name, binary=true)`. The format encodes a full XML tree with tag names, attributes, text content, and child elements using three string pools and a recursive tree section.

### File Layout

```
┌──────────────────────────────────────────────┐
│ Magic: "xml0" (4 bytes)                      │
│ Header uint32 (4 bytes)                      │
├──────────────────────────────────────────────┤
│ Tag Name Pool (LPS sequence)                 │
│   Terminated when LPS read fails             │
├──────────────────────────────────────────────┤
│ uint32 num_attr_names                        │
│ Attribute Name Pool (LPS × num_attr_names)   │
├──────────────────────────────────────────────┤
│ uint32 num_values                            │
│ Value Pool (LPS × num_values)                │
├──────────────────────────────────────────────┤
│ Tree Section (recursive element encoding)    │
└──────────────────────────────────────────────┘
```

### LPS (Length-Prefixed String)

All strings in the header pools use LPS encoding:

| Type   | Description                                          |
|--------|------------------------------------------------------|
| uint32 | String length including null terminator               |
| char[] | String bytes (ASCII, null-terminated)                 |

Header pool strings are strict ASCII (bytes 32–126) with a maximum length of 65536. Data strings in the tree section (element text content) can be much larger (up to several MB for billboard/particle data).

### Variable-Width Index Encoding

**Critical detail**: indices into each string pool use either 1 or 2 bytes depending on the pool size:

| Pool Size | Index Width | Type   |
|-----------|-------------|--------|
| ≤ 255     | 1 byte      | uint8  |
| > 255     | 2 bytes     | uint16 (little-endian) |

Each pool (tags, attr_names, values) independently determines its index width. For example, a file with 9 tags, 11 attr names, and 333 values would use 1-byte tag indices, 1-byte attr name indices, and 2-byte value indices.

### Element Encoding (Tree Section)

Each element is encoded recursively:

```
tag_index       (W_tag bytes)     — index into tag name pool
num_attrs       (1 byte)          — number of attribute pairs
separator       (1 byte)          — always 0
[attr_name_idx  (W_attr bytes)    — index into attr name pool
 attr_val_idx   (W_val bytes)     — index into value pool
] × num_attrs
num_data_ch     (1 byte)          — number of text data children (0 or 1)
if num_data_ch > 0:
  [LPS(text)]   × num_data_ch    — inline text content (can be very large)
if num_data_ch == 0:
  num_elem_ch   (1 byte)          — number of element children
  [0x00, child_element, 0x00]     — each child bracketed by zero markers
    × num_elem_ch
```

Where `W_tag`, `W_attr`, `W_val` are the index widths (1 or 2 bytes) determined by pool sizes.

**Key rules:**
- Elements with text content (`num_data_ch > 0`) have NO element children
- Elements with element children (`num_data_ch == 0`) have NO text content
- The separator byte after `num_attrs` is always 0
- Child elements are bracketed by `0x00` start and end markers

### Unigine XML API Mapping

The binary format maps directly to the Unigine `Xml` C++ API:

| API Method          | Binary Field        |
|---------------------|---------------------|
| `getName()`         | tag_index → tag pool |
| `getNumArgs()`      | num_attrs           |
| `getArgName(i)`     | attr_name_idx → attr pool |
| `getArgValue(i)`    | attr_val_idx → value pool |
| `getData()`         | LPS text content    |
| `getNumChildren()`  | num_elem_ch         |
| `getChild(i)`       | child_element       |

### Example: cannon_balls.node (annotated hex)

```
xml0 header...
Tag pool: [nodes, materials, library, properties, node, mesh, surface, transform]
Attr pool: [version, type, id, name, material, property]
Value pool: [1.17, ObjectMesh, 1734, Sphere10, cannon_ball_tesselation_paint, surface_base]

Tree bytes (all 1-byte indices since all pools ≤ 255):
00          tag=0 (nodes)
01          num_attrs=1
00          separator
00 00       attr: version="1.17"
00          num_data_ch=0
03          num_elem_ch=3
  00        start marker
  01 00 00  tag=materials, na=0, sep=0
  00 01     ndc=0, nec=1
    00      start marker
    02 00 00  tag=library, na=0, sep=0
    01      ndc=1
    [LPS "heaven/materials/heaven_props.mat"]
    00      end marker
  00        end marker
  00        start marker
  03 00 00  tag=properties, na=0, sep=0
  00 01     ndc=0, nec=1
    ...
  00        end marker
  00        start marker
  04        tag=node
  02 00     na=2, sep=0
  01 01     attr: type="ObjectMesh"
  02 02     attr: id="1734"
  00 03     ndc=0, nec=3
    ...mesh, surface, transform children...
```

### Key Element Types

| Element Type      | Description                                    |
|-------------------|------------------------------------------------|
| `ObjectMesh`      | A mesh instance with `mesh` path and `transform` |
| `NodeDummy`       | A grouping node with `transform` (no geometry) |
| `NodeReference`   | References an external `.node` file            |
| `NodeLayer`       | A layer grouping node                          |
| `LightSpot`       | A spot light with position/color               |
| `LightWorld`      | A directional (sun) light                      |
| `ObjectSky`       | Sky sphere                                     |
| `ObjectGrass`     | Procedural grass                               |
| `ObjectBillboards`| Billboard particles (large text data blob)     |

### Material File Format (.mat)

Material files use the same xml0 format. The XML structure follows the [Unigine Material Format](https://developer.unigine.com/en/docs/1.0/code/material_format):

```xml
<materials version="1.17">
  <material name="fort_wall_t" parent="heaven_tessellation_base">
    <texture name="diffuse">heaven/textures/fort/fort_wall_d.dds</texture>
    <texture name="normal">heaven/textures/fort/fort_wall_n.dds</texture>
    <texture name="specular">heaven/textures/fort/fort_wall_s.dds</texture>
    <texture name="height">heaven/textures/fort/fort_wall_h.dds</texture>
    <parameter name="tessellation_distance">15</parameter>
  </material>
</materials>
```

Materials use inheritance: a material with `parent="heaven_base"` inherits all textures and parameters from the parent, overriding only what it explicitly defines. The inheritance chain can span across multiple `.mat` files.

---

## Transform Matrix Convention

Unigine 1.x stores 4x4 transform matrices as 16 space-separated floats in **row-major order** with the **row-vector × matrix** multiplication convention.

### Storage Layout

```
Index:  [0]  [1]  [2]  [3]    [4]  [5]  [6]  [7]    [8]  [9]  [10] [11]   [12] [13] [14] [15]
        ─────────────────────  ─────────────────────  ──────────────────────  ─────────────────────
        r0c0 r0c1 r0c2 r0c3   r1c0 r1c1 r1c2 r1c3   r2c0 r2c1 r2c2 r2c3   tx   ty   tz   1.0
        ───── rotation/scale ─────────────────────────────────────────────   ── translation ──
```

- **Indices [0..11]**: 3x3 rotation/scale matrix (rows 0-2, columns 0-2), plus zeros in column 3
- **Indices [12..14]**: Translation (tx, ty, tz)
- **Index [15]**: Homogeneous coordinate, typically `1.0` (or a uniform scale factor)

### Point Transformation

To transform a point `(x, y, z)`:

```
rx = x * mat[0]  + y * mat[4]  + z * mat[8]  + mat[12]
ry = x * mat[1]  + y * mat[5]  + z * mat[9]  + mat[13]
rz = x * mat[2]  + y * mat[6]  + z * mat[10] + mat[14]
```

### Normal Transformation

Normals use only the 3x3 rotation/scale part (no translation), followed by re-normalization:

```
rx = nx * mat[0]  + ny * mat[4]  + nz * mat[8]
ry = nx * mat[1]  + ny * mat[5]  + nz * mat[9]
rz = nx * mat[2]  + ny * mat[6]  + nz * mat[10]
(rx, ry, rz) = normalize(rx, ry, rz)
```

### Matrix Composition

When a child node has transform `C` and its parent has transform `P`, the combined world transform is:

```
world = P × C    (standard row-major matrix multiplication)
```

### Identity Matrix

```
1 0 0 0  0 1 0 0  0 0 1 0  0 0 0 1
```

### Example: Lamp Post on Airship Deck

```
-0.0000 -0.4000  0.0000  0.0000
 0.4000 -0.0000  0.0000  0.0000
 0.0000  0.0000  0.4000  0.0000
 0.6751 -1.5592  3.2249  1.0000
```

This represents a 0.4× uniform scale, a 90° rotation around Z, and a translation to position `(0.675, -1.559, 3.225)` on the airship deck.

---

## Normal Compression

Unigine 1.x (mi08 format) compresses normals from float to `uint16` using a bias-and-scale encoding.

### Compression (float → uint16)

```
compressed = (uint16)clamp((int)((normal * 0.5 + 0.5) * 65535.0), 0, 65535)
```

### Decompression (uint16 → float)

```
normal = (compressed / 65535.0) * 2.0 - 1.0
```

This maps the range `[-1.0, 1.0]` to `[0, 65535]` with uniform precision of approximately `3.05e-5` per step.

Note: newer Unigine versions (2.x+) use `short` (signed 16-bit) with a different encoding for tangent quaternions. The mi08 format uses `unsigned short` for 3-component normals only.

---

## World File (.world)

The `.world` file uses the same `xml0` binary format as `.node` files but represents the entire scene. It contains:

- **Render settings**: shadow distances, HDR parameters, scattering, DOF, etc.
- **Material library references**: paths to `.mat` files
- **Node hierarchy**: the full scene tree with `ObjectMesh`, `NodeReference`, `NodeDummy`, lights, sky, grass, physics forces, and player/camera nodes
- **Inline scripts**: UnigineScript expressions for dynamic behavior (e.g., floating island animation)

The Heaven benchmark world file (`heaven.world`, 72 KB) references meshes both directly (inline `ObjectMesh` nodes for terrain islands) and indirectly (via `NodeReference` pointing to `.node` files for buildings, props, and the airship).

### Scene Structure (Heaven Benchmark)

```
world
├── LightWorld (sun)
├── ObjectSky (sky sphere)
├── WorldExpression (camera binding script)
├── NodeReference → stars.node
├── ObjectMesh: fly_island1.mesh (main floating island)
├── ObjectMesh: fly_island1_cap.mesh
├── ObjectGrass (procedural grass)
├── NodeReference → dragon.node
├── LightSpot (blue accent light)
├── NodeDummy "buildings"
│   ├── NodeReference → house1.node (×2 instances, different transforms)
│   ├── NodeReference → house2.node (×9 instances)
│   ├── NodeReference → house3.node
│   ├── NodeReference → house4.node ... house6.node
│   ├── NodeReference → mill.node
│   ├── NodeReference → lab.node
│   ├── NodeReference → mansion.node
│   ├── NodeReference → flag_small.node (×3)
│   └── NodeReference → weathercocks.node
├── NodeDummy "vegetation"
│   ├── NodeReference → fir1.node (×9 instances)
│   ├── NodeReference → fir2.node (×8 instances)
│   └── NodeReference → fir3.node (×12 instances)
├── NodeDummy "ruins" (columns, walls, floors, pavements, steps)
├── NodeDummy "benches" (×7 bench instances)
├── NodeDummy "lamp_post" (×7 with associated lights)
├── NodeDummy "port" (port building + props)
├── NodeLayer "bridges"
│   ├── move_island_1 → fly_island2.node + bridge parts
│   ├── move_island_2 → fly_island2.node + bridge parts
│   ├── move_island_3 → fly_island3 + fort + props
│   └── move_island_4 → fly_island2.node
├── ObjectDummy "stone_noise" (scattered stones ×24)
├── PhysicalForce (wind)
└── PlayerDummy "airship_1" (camera + airship.node)
```

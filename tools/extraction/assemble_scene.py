#!/usr/bin/env python3
"""
Unigine scene assembler: reads .node/.world files (xml0 binary format),
extracts mesh references with their transforms, reads the mi08 .mesh files,
applies transforms, and writes a combined OBJ + MTL.

Usage:
    python3 assemble_scene.py <input.node> <output.obj> [--data-dir DIR]
    python3 assemble_scene.py extracted/heaven/heaven/nodes/airship.node airship_assembled.obj --data-dir extracted/heaven

The --data-dir is the base directory where mesh paths like "heaven/meshes/..." are resolved.
"""

import struct
import sys
import os
import math
import argparse


# ─── mi08 mesh reader ───────────────────────────────────────────

class Surface:
    def __init__(self):
        self.name = ""
        self.vertices = []   # list of (x,y,z)
        self.normals = []    # list of (nx,ny,nz)
        self.texcoords = []  # list of (u,v)
        self.triangles = []  # list of (a,b,c)


def read_mesh(filepath):
    """Read a mi08 .mesh file and return list of Surface objects."""
    with open(filepath, 'rb') as f:
        data = f.read()

    off = 0

    def take(size):
        nonlocal off
        if size < 0 or size > len(data) - off:
            raise ValueError(f"Truncated mi08 mesh: {filepath}")
        result = data[off:off + size]
        off += size
        return result

    def count(stride, maximum=10_000_000):
        value, = struct.unpack('<i', take(4))
        if value < 0 or value > maximum or value > (len(data) - off) // stride:
            raise ValueError(f"Invalid mi08 count {value}: {filepath}")
        return value

    magic = take(4)
    if magic != b'mi08':
        raise ValueError(f"Not a mi08 mesh: {filepath} (magic={magic})")
    # Skip bounding volume (10 floats = 40 bytes)
    take(40)
    num_surfaces = count(45, 32767)
    if num_surfaces == 0:
        raise ValueError(f"Mesh has no surfaces: {filepath}")

    surfaces = []
    # Read headers
    for s in range(num_surfaces):
        surf = Surface()
        name_len = count(1, 1024)
        name = take(name_len)
        if not name or name[-1] != 0:
            raise ValueError(f"Unterminated mi08 surface name: {filepath}")
        surf.name = name[:-1].decode('ascii', errors='replace')
        take(40)  # skip surface bounding volume
        surfaces.append(surf)

    # Read data
    for surf in surfaces:
        num_verts = count(18)

        for v in range(num_verts):
            x, y, z, nx_c, ny_c, nz_c = struct.unpack('<3f3H', take(18))
            nx = (nx_c / 65535.0) * 2.0 - 1.0
            ny = (ny_c / 65535.0) * 2.0 - 1.0
            nz = (nz_c / 65535.0) * 2.0 - 1.0
            surf.vertices.append((x, y, z))
            surf.normals.append((nx, ny, nz))

        # Texcoords 0
        num_tc0 = count(8)
        for t in range(num_tc0):
            u, v = struct.unpack('<2f', take(8))
            surf.texcoords.append((u, v))

        # Texcoords 1 (skip)
        num_tc1 = count(8)
        take(num_tc1 * 8)

        # Triangles
        triangle_size = 6 if num_verts < 65536 else 12
        triangle_format = '<3H' if num_verts < 65536 else '<3I'
        num_tris = count(triangle_size)
        for t in range(num_tris):
            triangle = struct.unpack(triangle_format, take(triangle_size))
            if any(index >= num_verts or (num_tc0 and index >= num_tc0) for index in triangle):
                raise ValueError(f"Invalid mi08 triangle index: {filepath}")
            surf.triangles.append(triangle)

    return surfaces


# ─── Matrix math ─────────────────────────────────────────────────

def mat4_multiply(a, b):
    """Multiply two 4x4 matrices in Unigine convention.
    Unigine uses row-vector * matrix, stored as 4 rows of 4:
      [r0c0 r0c1 r0c2 r0c3  r1c0 r1c1 r1c2 r1c3  r2c0 r2c1 r2c2 r2c3  tx ty tz 1]
    Combined transform: point * A * B  =>  combined = A * B (standard row-major multiply)
    """
    r = [0.0] * 16
    for row in range(4):
        for col in range(4):
            s = 0.0
            for k in range(4):
                s += a[row*4+k] * b[k*4+col]
            r[row*4+col] = s
    return r


def transform_point(mat, x, y, z):
    """Transform a point by a 4x4 Unigine matrix (row-vector * matrix convention).
    Translation is in the last row at indices [12],[13],[14].
    point' = point * matrix:
      rx = x*m[0] + y*m[4] + z*m[8]  + m[12]
      ry = x*m[1] + y*m[5] + z*m[9]  + m[13]
      rz = x*m[2] + y*m[6] + z*m[10] + m[14]
    """
    rx = x*mat[0] + y*mat[4] + z*mat[8]  + mat[12]
    ry = x*mat[1] + y*mat[5] + z*mat[9]  + mat[13]
    rz = x*mat[2] + y*mat[6] + z*mat[10] + mat[14]
    return (rx, ry, rz)


def transform_normal(mat, nx, ny, nz):
    """Use the inverse transpose so normals stay perpendicular after scaling."""
    a, b, c = mat[0:3]
    d, e, f = mat[4:7]
    g, h, i = mat[8:11]
    cofactors = (e*i-f*h, f*g-d*i, d*h-e*g,
                 c*h-b*i, a*i-c*g, b*g-a*h,
                 b*f-c*e, c*d-a*f, a*e-b*d)
    determinant = a*cofactors[0] + b*cofactors[1] + c*cofactors[2]
    if determinant == 0 or not math.isfinite(determinant):
        raise ValueError("Cannot transform normals with a singular/non-finite matrix")
    rx = (nx*cofactors[0] + ny*cofactors[3] + nz*cofactors[6]) / determinant
    ry = (nx*cofactors[1] + ny*cofactors[4] + nz*cofactors[7]) / determinant
    rz = (nx*cofactors[2] + ny*cofactors[5] + nz*cofactors[8]) / determinant
    length = math.sqrt(rx*rx + ry*ry + rz*rz)
    if length > 1e-8:
        rx /= length; ry /= length; rz /= length
    return (rx, ry, rz)


# ─── xml0 mesh+transform extractor ──────────────────────────────

def extract_mesh_transforms(filepath):
    """Extract (mesh_path, 4x4_matrix_row_major) pairs from a .node or .world file."""
    with open(filepath, 'rb') as f:
        data = f.read()

    if data[0:4] != b'xml0':
        raise ValueError(f"Not an xml0 file: {filepath}")

    # Extract all readable strings with positions
    strings_with_pos = []
    i = 0
    while i < len(data):
        if 32 <= data[i] < 127:
            start = i
            while i < len(data) and 32 <= data[i] < 127:
                i += 1
            text = data[start:i].decode('ascii')
            strings_with_pos.append((start, text))
        else:
            i += 1

    # Identify mesh paths and transform matrices
    mesh_indices = []
    transform_indices = []

    for idx, (pos, text) in enumerate(strings_with_pos):
        if text.endswith('.mesh') and '/' in text:
            mesh_indices.append(idx)

        parts = text.split()
        if len(parts) >= 16:
            try:
                nums = [float(p) for p in parts[:16]]
                if len(parts) <= 20:  # not some other long string
                    transform_indices.append(idx)
            except ValueError:
                pass

    results = []
    identity = [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]

    for mi in mesh_indices:
        mesh_path = strings_with_pos[mi][1]
        # Find the closest transform after this mesh reference
        best_ti = None
        for ti in transform_indices:
            if ti > mi:
                best_ti = ti
                break

        if best_ti is not None:
            text = strings_with_pos[best_ti][1]
            nums = [float(p) for p in text.split()[:16]]
            results.append((mesh_path, nums))
        else:
            results.append((mesh_path, list(identity)))

    return results


def extract_node_refs_with_transforms(filepath):
    """Extract (node_path, 4x4_matrix) pairs for NodeReference entries."""
    with open(filepath, 'rb') as f:
        data = f.read()

    if data[0:4] != b'xml0':
        return []

    strings_with_pos = []
    i = 0
    while i < len(data):
        if 32 <= data[i] < 127:
            start = i
            while i < len(data) and 32 <= data[i] < 127:
                i += 1
            text = data[start:i].decode('ascii')
            strings_with_pos.append((start, text))
        else:
            i += 1

    node_indices = []
    transform_indices = []

    for idx, (pos, text) in enumerate(strings_with_pos):
        if text.endswith('.node') and '/' in text:
            node_indices.append(idx)
        parts = text.split()
        if len(parts) >= 16 and len(parts) <= 20:
            try:
                nums = [float(p) for p in parts[:16]]
                transform_indices.append(idx)
            except ValueError:
                pass

    results = []
    identity = [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]

    for ni in node_indices:
        node_path = strings_with_pos[ni][1]
        best_ti = None
        for ti in transform_indices:
            if ti > ni:
                best_ti = ti
                break
        if best_ti is not None:
            text = strings_with_pos[best_ti][1]
            nums = [float(p) for p in text.split()[:16]]
            results.append((node_path, nums))
        else:
            results.append((node_path, list(identity)))

    return results


# ─── OBJ writer ──────────────────────────────────────────────────

def write_assembled_obj(mesh_entries, obj_path):
    """
    mesh_entries: list of (mesh_filepath, transform_matrix, group_name)
    Writes combined OBJ + MTL.
    """
    mtl_name = os.path.splitext(os.path.basename(obj_path))[0] + ".mtl"
    mtl_path = os.path.join(os.path.dirname(obj_path) or '.', mtl_name)

    all_materials = set()
    total_verts = 0
    total_tris = 0
    mesh_count = 0

    with open(obj_path, 'w') as fobj:
        fobj.write(f"# Assembled scene\n")
        fobj.write(f"mtllib {mtl_name}\n\n")

        v_offset = 0
        vn_offset = 0
        vt_offset = 0

        for mesh_path, transform, group_name in mesh_entries:
            if not os.path.exists(mesh_path):
                print(f"  WARNING: mesh not found: {mesh_path}")
                continue

            try:
                surfaces = read_mesh(mesh_path)
            except Exception as e:
                print(f"  WARNING: failed to read {mesh_path}: {e}")
                continue

            mesh_count += 1

            for surf in surfaces:
                mat_name = f"{group_name}_{surf.name}".replace(' ', '_')
                all_materials.add(mat_name)

                fobj.write(f"o {mat_name}\n")
                fobj.write(f"usemtl {mat_name}\n")

                # Transformed vertices
                for vx, vy, vz in surf.vertices:
                    tx, ty, tz = transform_point(transform, vx, vy, vz)
                    fobj.write(f"v {tx:.6f} {ty:.6f} {tz:.6f}\n")

                # Transformed normals
                for nx, ny, nz in surf.normals:
                    tnx, tny, tnz = transform_normal(transform, nx, ny, nz)
                    fobj.write(f"vn {tnx:.6f} {tny:.6f} {tnz:.6f}\n")

                # UVs (flip V)
                for u, v in surf.texcoords:
                    fobj.write(f"vt {u:.6f} {1.0 - v:.6f}\n")

                # Faces
                has_uv = len(surf.texcoords) > 0
                for a, b, c in surf.triangles:
                    a1 = a + 1 + v_offset
                    b1 = b + 1 + v_offset
                    c1 = c + 1 + v_offset
                    if has_uv:
                        at = a + 1 + vt_offset
                        bt = b + 1 + vt_offset
                        ct = c + 1 + vt_offset
                        an = a + 1 + vn_offset
                        bn = b + 1 + vn_offset
                        cn = c + 1 + vn_offset
                        fobj.write(f"f {a1}/{at}/{an} {b1}/{bt}/{bn} {c1}/{ct}/{cn}\n")
                    else:
                        an = a + 1 + vn_offset
                        bn = b + 1 + vn_offset
                        cn = c + 1 + vn_offset
                        fobj.write(f"f {a1}//{an} {b1}//{bn} {c1}//{cn}\n")

                fobj.write("\n")

                total_verts += len(surf.vertices)
                total_tris += len(surf.triangles)
                v_offset += len(surf.vertices)
                vn_offset += len(surf.normals)
                vt_offset += len(surf.texcoords)

    # Write MTL
    with open(mtl_path, 'w') as fmtl:
        fmtl.write(f"# Materials for assembled scene\n")
        for mat in sorted(all_materials):
            fmtl.write(f"\nnewmtl {mat}\n")
            fmtl.write(f"Ka 0.2 0.2 0.2\n")
            fmtl.write(f"Kd 0.8 0.8 0.8\n")
            fmtl.write(f"Ks 0.0 0.0 0.0\n")
            fmtl.write(f"d 1.0\n")

    return mesh_count, total_verts, total_tris


# ─── Main ────────────────────────────────────────────────────────

def resolve_path(relative_path, data_dirs):
    """Try to find a file by checking multiple data directories."""
    for d in data_dirs:
        full = os.path.join(d, relative_path)
        if os.path.exists(full):
            return full
    return None


def process_node_file(node_file, data_dirs, parent_transform=None, ancestors=frozenset()):
    """Recursively process a .node file, returning list of (mesh_path, combined_transform, group_name)."""
    if parent_transform is None:
        parent_transform = [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]
    canonical = os.path.realpath(node_file)
    if canonical in ancestors:
        raise ValueError(f"Cyclic node reference: {node_file}")
    ancestors = ancestors | {canonical}

    entries = []

    # Get direct mesh references
    mesh_pairs = extract_mesh_transforms(node_file)
    for mesh_rel, local_transform in mesh_pairs:
        combined = mat4_multiply(local_transform, parent_transform)
        mesh_full = resolve_path(mesh_rel, data_dirs)
        if mesh_full:
            group = os.path.splitext(os.path.basename(mesh_rel))[0]
            entries.append((mesh_full, combined, group))
        else:
            print(f"  WARNING: cannot resolve mesh: {mesh_rel}")

    # Get child node references
    node_pairs = extract_node_refs_with_transforms(node_file)
    for node_rel, local_transform in node_pairs:
        combined = mat4_multiply(local_transform, parent_transform)
        node_full = resolve_path(node_rel, data_dirs)
        if node_full:
            child_entries = process_node_file(node_full, data_dirs, combined, ancestors)
            entries.extend(child_entries)

    return entries


def main():
    parser = argparse.ArgumentParser(description='Assemble Unigine scene from .node/.world files into OBJ')
    parser.add_argument('input', help='Input .node or .world file')
    parser.add_argument('output', help='Output .obj file')
    parser.add_argument('--data-dir', action='append', default=[],
                        help='Data directory for resolving mesh/node paths (can specify multiple)')
    args = parser.parse_args()

    if not args.data_dir:
        # Default: try the directory containing the input file and its parents
        input_dir = os.path.dirname(os.path.abspath(args.input))
        args.data_dir = [input_dir]
        # Also try parent directories
        parent = os.path.dirname(input_dir)
        while parent and parent != os.path.dirname(parent):
            args.data_dir.append(parent)
            parent = os.path.dirname(parent)

    print(f"Assembling scene from: {args.input}")
    print(f"Data directories: {args.data_dir}")

    entries = process_node_file(args.input, args.data_dir)

    if not entries:
        print("No mesh entries found!")
        return 1

    print(f"\nFound {len(entries)} mesh instances:")
    for mesh_path, transform, group in entries:
        tx, ty, tz = transform[12], transform[13], transform[14]
        print(f"  {group}: translate=({tx:.3f}, {ty:.3f}, {tz:.3f})")

    mesh_count, total_verts, total_tris = write_assembled_obj(entries, args.output)
    print(f"\nWrote {args.output}: {mesh_count} meshes, {total_verts} vertices, {total_tris} triangles")
    return 0 if mesh_count == len(entries) else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"Error: {error}", file=sys.stderr)
        sys.exit(1)

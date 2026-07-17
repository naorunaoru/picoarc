"""Import and align exact STEP components in the PicoARC hero scene.

Run inside Blender after opening ``picoarc-hero.blend``.  Each replacement is
parented to the existing reference-named footprint empty, while the original
mesh is retained as a hidden rollback source.
"""

from __future__ import annotations

import json
import math
from pathlib import Path

import bpy
from mathutils import Matrix, Vector


COLLECTION_NAME = "Exact Component STEP"
MANIFEST_NAME = "component-sweep.json"


def _principled_material(name, base, metallic=0.0, roughness=0.3):
    material = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    material.use_nodes = True
    node = next(node for node in material.node_tree.nodes if node.type == "BSDF_PRINCIPLED")
    node.inputs["Base Color"].default_value = (*base, 1.0)
    node.inputs["Metallic"].default_value = metallic
    node.inputs["Roughness"].default_value = roughness
    material.diffuse_color = (*base, 1.0)
    return material


def _material_library():
    return {
        "epoxy": _principled_material("Exact IC Epoxy", (0.006, 0.008, 0.012), 0.0, 0.30),
        "mark": _principled_material("Exact Package Marking", (0.22, 0.25, 0.29), 0.0, 0.38),
        "mark_dark": _principled_material("Exact Dark Marking", (0.035, 0.042, 0.052), 0.0, 0.36),
        "tin": _principled_material("Exact Tin Lead", (0.42, 0.48, 0.55), 0.86, 0.22),
        "gold": _principled_material("Exact Gold Contact", (0.65, 0.38, 0.055), 0.82, 0.23),
        "can": _principled_material("Exact Crystal Can", (0.36, 0.40, 0.45), 0.88, 0.21),
        "ceramic": _principled_material("Exact LED Ceramic", (0.62, 0.64, 0.63), 0.0, 0.34),
        "red": _principled_material("Exact LED Red Lens", (0.32, 0.004, 0.002), 0.0, 0.18),
        "substrate": _principled_material("Exact Dark Substrate", (0.010, 0.012, 0.016), 0.0, 0.34),
    }


def _bounds(points):
    minimum = Vector((min(point.x for point in points), min(point.y for point in points), min(point.z for point in points)))
    maximum = Vector((max(point.x for point in points), max(point.y for point in points), max(point.z for point in points)))
    return minimum, maximum


def _find_source(parent, ref, source_kind):
    expected_name = f"{ref}_{source_kind}_Source"
    source = bpy.data.objects.get(expected_name)
    if source is not None:
        return source

    candidates = [
        child
        for child in parent.children
        if child.type == "MESH" and child.name != f"{ref}_STEP_Exact"
    ]
    if not candidates:
        raise RuntimeError(f"{ref}: source mesh not found")

    source = max(candidates, key=lambda obj: len(obj.data.polygons))
    source.name = expected_name
    return source


def _import_one(entry, models_root, collection, materials):
    ref = entry["ref"]
    parent = bpy.data.objects.get(ref)
    if parent is None:
        raise RuntimeError(f"{ref}: footprint parent not found")

    source = _find_source(parent, ref, entry["source_kind"])
    old_exact = bpy.data.objects.get(f"{ref}_STEP_Exact")
    if old_exact is not None:
        bpy.data.objects.remove(old_exact, do_unlink=True)

    model_path = models_root / entry["model"]
    if not model_path.is_file():
        raise FileNotFoundError(model_path)

    objects_before = set(bpy.data.objects)
    materials_before = set(bpy.data.materials)
    bpy.ops.import_scene.step(
        filepath=str(model_path),
        up_axis="Y",
        placement="ORIGIN",
        scale=1.0,
        merge_objects=True,
        use_assembly_collections=False,
    )
    imported = [obj for obj in bpy.data.objects if obj not in objects_before and obj.type == "MESH"]
    if len(imported) != 1:
        for obj in imported:
            bpy.data.objects.remove(obj, do_unlink=True)
        raise RuntimeError(f"{ref}: expected one imported mesh, got {len(imported)}")

    exact = imported[0]
    exact.name = f"{ref}_STEP_Exact"
    for owner in list(exact.users_collection):
        owner.objects.unlink(exact)
    collection.objects.link(exact)

    source_points = [source.matrix_local @ vertex.co for vertex in source.data.vertices]
    source_min, source_max = _bounds(source_points)
    source_center = (source_min + source_max) * 0.5

    rotation = Matrix.Rotation(math.radians(90 * entry["quarter_turns"]), 4, "Z")
    basis = rotation @ exact.matrix_world.copy()
    exact_points = [basis @ vertex.co for vertex in exact.data.vertices]
    exact_min, exact_max = _bounds(exact_points)
    exact_center = (exact_min + exact_max) * 0.5
    translation = Vector(
        (
            source_center.x - exact_center.x,
            source_center.y - exact_center.y,
            source_min.z - exact_min.z,
        )
    )

    exact.parent = parent
    exact.matrix_parent_inverse = Matrix.Identity(4)
    exact.matrix_basis = Matrix.Translation(translation) @ basis
    exact.hide_render = False
    exact.hide_set(False)

    roles = entry["material_roles"]
    if len(roles) != len(exact.material_slots):
        raise RuntimeError(
            f"{ref}: manifest has {len(roles)} material roles for {len(exact.material_slots)} slots"
        )
    for slot, role in zip(exact.material_slots, roles):
        slot.material = materials[role]

    source.hide_render = True
    source.hide_set(True)
    exact["picoarc_exact_ref"] = ref
    exact["lcsc_id"] = entry["lcsc"]
    exact["mpn"] = entry["mpn"]
    exact["source_model"] = entry["model"]
    exact["alignment_quarter_turns"] = entry["quarter_turns"]
    exact["material_roles"] = ",".join(roles)

    for material in set(bpy.data.materials) - materials_before:
        if material.users == 0:
            bpy.data.materials.remove(material)

    return {
        "ref": ref,
        "source": source.name,
        "exact": exact.name,
        "translation_mm": [round(value * 1000, 4) for value in translation],
        "polygons_before": len(source.data.polygons),
        "polygons_after": len(exact.data.polygons),
    }


def run(manifest_path=None):
    render_dir = Path(bpy.data.filepath).resolve().parent
    manifest_path = Path(manifest_path) if manifest_path else render_dir / MANIFEST_NAME
    manifest = json.loads(manifest_path.read_text())
    models_root = (manifest_path.parent / manifest["models_root"]).resolve()

    collection = bpy.data.collections.get(COLLECTION_NAME)
    if collection is None:
        collection = bpy.data.collections.new(COLLECTION_NAME)
        bpy.context.scene.collection.children.link(collection)

    materials = _material_library()
    reports = [
        _import_one(entry, models_root, collection, materials)
        for entry in manifest["components"]
    ]
    bpy.context.view_layer.update()
    return reports


if __name__ == "__main__":
    print(json.dumps(run(), indent=2))

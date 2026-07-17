"""Blender-side refresh and render pipeline for PicoARC.

This script is launched by ``render_pcb.py``.  It opens the art-directed blend
template, replaces its imported KiCad assembly with a fresh STEP export, moves
the calibrated exact component models to their current KiCad placements, and
renders the pinned camera set.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

import bpy
from mathutils import Matrix, Vector


HERO_OBJECTS = ("Hero Floor", "Hero Platform", "Hero Cyan Arc", "Hero Violet Arc")
HERO_LIGHTS = ("Hero Key", "Hero Fill", "Hero Rim", "Hero Softbox")
STUDY_LIGHTS = ("Study Key", "Study Fill", "Study Rim", "Study Accent")
PNG_METADATA_CHUNKS = {b"eXIf", b"iTXt", b"tEXt", b"tIME", b"zTXt"}


def _parse_args() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    parser = argparse.ArgumentParser()
    parser.add_argument("--step", type=Path, required=True)
    parser.add_argument("--positions", type=Path, required=True)
    parser.add_argument("--pcb", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--samples", type=int, required=True)
    parser.add_argument("--background", choices=("transparent", "charcoal"), required=True)
    parser.add_argument("--angles", default="all")
    parser.add_argument("--save-blend", type=Path)
    parser.add_argument("--success-file", type=Path, required=True)
    return parser.parse_args(argv)


def _require_object(name: str):
    obj = bpy.data.objects.get(name)
    if obj is None:
        raise RuntimeError(f"Template object is missing: {name}")
    return obj


def _require_material(name: str):
    material = bpy.data.materials.get(name)
    if material is None:
        raise RuntimeError(f"Template material is missing: {name}")
    return material


def _portable_source_path(path: Path, repo_root: Path) -> str:
    """Return a repository-relative path without exposing an external host path."""
    resolved = path.resolve()
    try:
        return resolved.relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        return resolved.name


def _disable_render_metadata(scene) -> None:
    """Keep Blender from embedding host paths and runtime details in PNG chunks."""
    for attribute in dir(scene.render):
        if attribute.startswith("use_stamp"):
            setattr(scene.render, attribute, False)


def _strip_png_metadata(path: Path) -> None:
    """Remove ancillary metadata chunks without decoding or changing image pixels."""
    data = path.read_bytes()
    signature = b"\x89PNG\r\n\x1a\n"
    if not data.startswith(signature):
        raise RuntimeError(f"Rendered file is not a PNG: {path}")

    cleaned = bytearray(signature)
    offset = len(signature)
    found_end = False
    while offset < len(data):
        if offset + 12 > len(data):
            raise RuntimeError(f"Truncated PNG chunk in {path}")
        length = int.from_bytes(data[offset : offset + 4], "big")
        chunk_type = data[offset + 4 : offset + 8]
        chunk_end = offset + 12 + length
        if chunk_end > len(data):
            raise RuntimeError(f"Invalid PNG chunk length in {path}")
        if chunk_type not in PNG_METADATA_CHUNKS:
            cleaned.extend(data[offset:chunk_end])
        offset = chunk_end
        if chunk_type == b"IEND":
            found_end = True
            break

    if not found_end:
        raise RuntimeError(f"PNG is missing its IEND chunk: {path}")
    path.write_bytes(cleaned)


def _scrub_saved_blend_metadata() -> None:
    """Remove importer provenance that contains temporary or host-local paths."""
    for obj in bpy.data.objects:
        for key in ("step_source_file", "source_step"):
            if key in obj:
                del obj[key]
    for scene in bpy.data.scenes:
        scene.render.filepath = "//"


def _read_positions(path: Path) -> dict[str, dict[str, float | str]]:
    positions = {}
    with path.open(newline="") as handle:
        for row in csv.DictReader(handle):
            positions[row["Ref"]] = {
                "x_mm": float(row["PosX"]),
                "y_mm": float(row["PosY"]),
                "rotation_deg": float(row["Rot"]),
                "side": row["Side"].lower(),
            }
    return positions


def _descendants(root):
    found = []
    pending = list(root.children)
    while pending:
        obj = pending.pop()
        found.append(obj)
        pending.extend(obj.children)
    return found


def _unlink_except(obj, collection) -> None:
    for owner in list(obj.users_collection):
        if owner != collection:
            owner.objects.unlink(obj)
    if obj.name not in collection.objects:
        collection.objects.link(obj)


def _world_z_center(obj) -> float:
    points = [obj.matrix_world @ Vector(corner) for corner in obj.bound_box]
    return (min(p.z for p in points) + max(p.z for p in points)) * 0.5


def _xy_bounds(obj) -> tuple[float, float, float, float]:
    points = [obj.matrix_world @ Vector(corner) for corner in obj.bound_box]
    return (
        min(p.x for p in points),
        max(p.x for p in points),
        min(p.y for p in points),
        max(p.y for p in points),
    )


def _delete_tree(root) -> None:
    for obj in reversed(_descendants(root)):
        if obj.name in bpy.data.objects:
            bpy.data.objects.remove(obj, do_unlink=True)
    if root.name in bpy.data.objects:
        bpy.data.objects.remove(root, do_unlink=True)


def _enable_step_importer() -> None:
    try:
        bpy.ops.preferences.addon_enable(module="bl_ext.blender_org.step_importer")
    except Exception:
        pass
    if not hasattr(bpy.ops.import_scene, "step"):
        raise RuntimeError(
            "Blender STEP Importer is not enabled. Install/enable the official "
            "STEP importer extension before running this command."
        )


def _assign_board_materials(new_objects, material_names: dict[str, str]) -> dict[str, str]:
    materials = {key: _require_material(value) for key, value in material_names.items()}
    meshes = [obj for obj in new_objects if obj.type == "MESH" and obj.name in bpy.data.objects]

    def matching(prefix: str):
        return [obj for obj in meshes if obj.name.startswith(prefix)]

    direct = {
        "layout_copper": materials["copper"],
        "layout_via": materials["copper"],
        "layout_pad": materials["pads"],
        "layout_PCB": materials["board_core"],
    }
    assignments = {}
    for prefix, material in direct.items():
        selected = matching(prefix)
        if len(selected) != 1:
            raise RuntimeError(f"Expected one {prefix} mesh, found {len(selected)}")
        selected[0].data.materials.clear()
        selected[0].data.materials.append(material)
        selected[0]["picoarc_render_layer"] = prefix
        assignments[selected[0].name] = material.name

    silks = sorted(matching("layout_silkscreen"), key=_world_z_center, reverse=True)
    masks = sorted(matching("layout_soldermask"), key=_world_z_center, reverse=True)
    if len(silks) != 2 or len(masks) != 2:
        raise RuntimeError(
            f"Expected two silkscreen and two soldermask meshes; got {len(silks)} and {len(masks)}"
        )
    paired = (
        (silks[0], materials["front_silkscreen"], "front_silkscreen"),
        (silks[1], materials["back_silkscreen"], "back_silkscreen"),
        (masks[0], materials["front_soldermask"], "front_soldermask"),
        (masks[1], materials["back_soldermask"], "back_soldermask"),
    )
    for obj, material, layer in paired:
        obj.data.materials.clear()
        obj.data.materials.append(material)
        obj["picoarc_render_layer"] = layer
        assignments[obj.name] = material.name
    return assignments


def _remap_imported_materials(new_objects) -> int:
    """Reuse the template's polished mat_N materials after STEP name suffixing."""
    remapped = 0
    for obj in new_objects:
        if obj.type != "MESH":
            continue
        for slot in obj.material_slots:
            material = slot.material
            if material is None:
                continue
            match = re.fullmatch(r"(mat_\d+)(?:\.\d+)?", material.name)
            if not match:
                continue
            canonical = bpy.data.materials.get(match.group(1))
            if canonical is not None and canonical != material:
                slot.material = canonical
                remapped += 1
    return remapped


def _find_old_root(assembly, rig):
    candidates = [
        obj
        for obj in assembly.objects
        if obj.type == "EMPTY" and obj.parent == rig and len(obj.children) >= 20
    ]
    if len(candidates) != 1:
        raise RuntimeError(f"Expected one template assembly root, found {len(candidates)}")
    return candidates[0]


def _capture_exact_components(config, old_root, exact_collection, assembly):
    old_root_inverse = old_root.matrix_world.inverted()
    captured = {}
    for ref in config["exact_components"]:
        exact = _require_object(f"{ref}_STEP_Exact")
        captured[ref] = {
            "object": exact,
            "board_matrix": old_root_inverse @ exact.matrix_world.copy(),
        }
        world = exact.matrix_world.copy()
        exact.parent = None
        exact.matrix_world = world
        if exact.name not in exact_collection.objects:
            exact_collection.objects.link(exact)
        if exact.name in assembly.objects:
            assembly.objects.unlink(exact)
    return captured


def _replace_assembly(step_path: Path, positions, config):
    presentation = config["presentation"]
    rig = _require_object(presentation["rig"])
    assembly = bpy.data.collections.get(presentation["assembly_collection"])
    exact_collection = bpy.data.collections.get(presentation["exact_collection"])
    if assembly is None or exact_collection is None:
        raise RuntimeError("The template assembly/exact collection is missing")

    old_root = _find_old_root(assembly, rig)
    root_basis = old_root.matrix_basis.copy()
    exact = _capture_exact_components(config, old_root, exact_collection, assembly)

    for obj in list(assembly.objects):
        if obj.name in bpy.data.objects:
            bpy.data.objects.remove(obj, do_unlink=True)

    before = set(bpy.data.objects)
    _enable_step_importer()
    bpy.ops.import_scene.step(
        filepath=str(step_path),
        up_axis="Y",
        placement="ORIGIN",
        scale=1.0,
        merge_objects=False,
        use_assembly_collections=False,
    )
    new_objects = [obj for obj in bpy.data.objects if obj not in before]
    new_object_names = [obj.name for obj in new_objects]
    roots = [obj for obj in new_objects if obj.parent is None and obj.type == "EMPTY"]
    if len(roots) != 1:
        raise RuntimeError(f"Expected one imported STEP root, found {len(roots)}")
    new_root = roots[0]

    component_empties = [
        obj
        for obj in new_objects
        if obj.type == "EMPTY" and obj.parent == new_root and len(obj.children) > 0
    ]
    if not component_empties:
        raise RuntimeError("The STEP export contains no component instances")
    component_plane = sorted(obj.matrix_world.translation.z for obj in component_empties)[
        len(component_empties) // 2
    ]

    removed_generic = []
    for ref, baseline in config["exact_components"].items():
        current = positions.get(ref)
        if current is None:
            raise RuntimeError(f"Exact component {ref} is missing from the KiCad position file")
        if current["side"] != baseline["side"]:
            raise RuntimeError(
                f"{ref} moved from {baseline['side']} to {current['side']}; "
                "recalibrate its exact STEP model in the template"
            )
        prefix = baseline.get("import_name_prefix")
        if not prefix:
            continue
        target = Vector((current["x_mm"] / 1000.0, current["y_mm"] / 1000.0))
        candidates = []
        for obj in component_empties:
            if obj.name not in bpy.data.objects or not obj.name.startswith(prefix):
                continue
            xy = obj.matrix_world.translation.xy
            candidates.append(((xy - target).length, obj))
        closest = min(candidates, default=(float("inf"), None), key=lambda item: item[0])
        if closest[0] > 0.00005:
            raise RuntimeError(
                f"Could not identify KiCad's generic {ref} model (closest distance: {closest[0] * 1000:.3f} mm)"
            )
        _, generic = closest
        removed_generic.append(generic.name)
        component_empties.remove(generic)
        _delete_tree(generic)

    # Re-resolve by name after removing generic component subtrees. Blender
    # invalidates deleted StructRNA handles immediately.
    new_objects = [bpy.data.objects[name] for name in new_object_names if name in bpy.data.objects]
    remapped_material_slots = _remap_imported_materials(new_objects)
    assignments = _assign_board_materials(new_objects, config["materials"])

    for obj in new_objects:
        if obj.name not in bpy.data.objects:
            continue
        _unlink_except(obj, assembly)

    pcb_meshes = [
        obj
        for obj in new_objects
        if obj.name in bpy.data.objects and obj.type == "MESH" and obj.name.startswith("layout_PCB")
    ]
    if len(pcb_meshes) != 1:
        raise RuntimeError(f"Expected one board-core mesh, found {len(pcb_meshes)}")
    min_x, max_x, min_y, max_y = _xy_bounds(pcb_meshes[0])
    board_center = Vector(((min_x + max_x) * 0.5, (min_y + max_y) * 0.5))
    root_basis.translation = Vector(
        (-board_center.x, -board_center.y, float(presentation["root_z_offset_m"]))
    )
    # Preserve the STEP importer's Y-up -> Z-up transform on its own root.
    # A separate wrapper receives the art-directed presentation transform.
    wrapper = bpy.data.objects.new("PicoARC Layout Assembly", None)
    assembly.objects.link(wrapper)
    wrapper.parent = rig
    wrapper.matrix_parent_inverse = Matrix.Identity(4)
    wrapper.matrix_basis = root_basis
    new_root.name = "KiCad STEP Assembly"
    new_root.parent = wrapper
    new_root.matrix_parent_inverse = Matrix.Identity(4)

    z_delta = component_plane - float(presentation["baseline_component_plane_z_m"])
    moved = {}
    for ref, captured in exact.items():
        baseline = config["exact_components"][ref]
        current = positions[ref]
        old_xy = Vector((baseline["x_mm"] / 1000.0, baseline["y_mm"] / 1000.0, 0.0))
        new_xy = Vector((current["x_mm"] / 1000.0, current["y_mm"] / 1000.0, z_delta))
        rotation = Matrix.Rotation(
            math.radians(current["rotation_deg"] - baseline["rotation_deg"]), 4, "Z"
        )
        delta = Matrix.Translation(new_xy) @ rotation @ Matrix.Translation(-old_xy)
        obj = captured["object"]
        obj.parent = wrapper
        obj.matrix_parent_inverse = Matrix.Identity(4)
        obj.matrix_basis = delta @ captured["board_matrix"]
        obj.hide_render = False
        obj.hide_set(False)
        moved[ref] = {
            "x_mm": current["x_mm"],
            "y_mm": current["y_mm"],
            "rotation_deg": current["rotation_deg"],
        }

    bpy.context.view_layer.update()
    return {
        "root": wrapper.name,
        "step_root": new_root.name,
        "board_center_m": list(board_center),
        "component_plane_m": component_plane,
        "removed_generic": removed_generic,
        "remapped_material_slots": remapped_material_slots,
        "moved_exact": moved,
        "material_assignments": assignments,
    }


def _aim(obj, target: Vector) -> None:
    obj.rotation_euler = (target - obj.location).to_track_quat("-Z", "Y").to_euler()


def _apply_lights(camera, target: Vector, scheme: str) -> None:
    if scheme == "overhead":
        specs = {
            "Study Key": (target + Vector((0.050, -0.038, 0.082)), 0.52, 0.145),
            "Study Fill": (target + Vector((-0.042, 0.032, 0.058)), 0.17, 0.125),
            "Study Rim": (target + Vector((0.060, 0.042, 0.030)), 0.20, 0.070),
            "Study Accent": (target + Vector((-0.058, -0.044, 0.030)), 0.18, 0.065),
        }
    else:
        front = Vector((camera.location.x - target.x, camera.location.y - target.y, 0.0))
        if front.length < 1e-9:
            front = Vector((0.0, -1.0, 0.0))
        front.normalize()
        right = Vector((front.y, -front.x, 0.0))
        if scheme == "underside":
            specs = {
                "Study Key": (target + front * 0.055 + right * 0.043 + Vector((0, 0, -0.045)), 0.62, 0.120),
                "Study Fill": (target + front * 0.039 - right * 0.040 + Vector((0, 0, -0.035)), 0.18, 0.110),
                "Study Rim": (target - front * 0.010 + right * 0.070 + Vector((0, 0, -0.015)), 0.24, 0.065),
                "Study Accent": (target + front * 0.010 - right * 0.070 + Vector((0, 0, -0.015)), 0.22, 0.060),
            }
        else:
            specs = {
                "Study Key": (target + front * 0.055 + right * 0.043 + Vector((0, 0, 0.071)), 0.68, 0.115),
                "Study Fill": (target + front * 0.039 - right * 0.040 + Vector((0, 0, 0.051)), 0.20, 0.105),
                "Study Rim": (target - front * 0.010 + right * 0.070 + Vector((0, 0, 0.023)), 0.27, 0.060),
                "Study Accent": (target + front * 0.010 - right * 0.070 + Vector((0, 0, 0.023)), 0.27, 0.060),
            }
    for name, (location, energy, size) in specs.items():
        light = _require_object(name)
        light.location = location
        light.data.energy = energy
        light.data.shape = "DISK"
        light.data.size = size
        light.data.color = (1.0, 0.965, 0.92)
        _aim(light, target)


def _select_cameras(config, selection: str):
    cameras = config["cameras"]
    if selection == "all":
        return cameras
    ids = {item.strip() for item in selection.split(",") if item.strip()}
    known = {camera["id"] for camera in cameras}
    unknown = ids - known
    if unknown:
        raise RuntimeError(f"Unknown camera ids: {', '.join(sorted(unknown))}")
    return [camera for camera in cameras if camera["id"] in ids]


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _render(args, config, refresh_report) -> list[dict[str, str]]:
    scene = bpy.context.scene
    render = config["render"]
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    _disable_render_metadata(scene)

    for name in HERO_OBJECTS + HERO_LIGHTS:
        obj = bpy.data.objects.get(name)
        if obj:
            obj.hide_render = True
    for name in STUDY_LIGHTS:
        _require_object(name).hide_render = False

    scene.render.engine = "CYCLES"
    scene.cycles.samples = args.samples
    scene.cycles.use_denoising = bool(render["denoise"])
    scene.render.resolution_x = int(render["resolution"][0])
    scene.render.resolution_y = int(render["resolution"][1])
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = render["file_format"]
    scene.render.image_settings.color_mode = render["color_mode"]
    scene.render.film_transparent = args.background == "transparent"
    scene.view_settings.exposure = float(render["exposure"])
    scene.render.dither_intensity = float(render["dither"])
    try:
        scene.view_settings.look = render["look"]
    except TypeError:
        pass

    target = _require_object(config["presentation"]["focus"]).matrix_world.translation.copy()
    outputs = []
    for camera_config in _select_cameras(config, args.angles):
        camera = _require_object(camera_config["object"])
        scene.camera = camera
        _apply_lights(camera, target, camera_config["lighting"])
        path = output_dir / camera_config["filename"]
        scene.render.filepath = str(path)
        print(f"Rendering {camera_config['id']}: {camera.name} -> {path}", flush=True)
        bpy.ops.render.render(write_still=True)
        if not path.is_file() or path.stat().st_size == 0:
            raise RuntimeError(f"Blender did not create {path}")
        _strip_png_metadata(path)
        outputs.append(
            {
                "id": camera_config["id"],
                "camera": camera.name,
                "path": path.relative_to(output_dir).as_posix(),
                "sha256": _sha256(path),
            }
        )

    manifest = {
        "version": 1,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "source_pcb": _portable_source_path(args.pcb, args.repo_root),
        "source_pcb_sha256": _sha256(args.pcb),
        "template_blend": _portable_source_path(Path(bpy.data.filepath), args.repo_root),
        "config": _portable_source_path(args.config, args.repo_root),
        "config_sha256": _sha256(args.config),
        "background": args.background,
        "engine": "CYCLES",
        "samples": args.samples,
        "resolution": render["resolution"],
        "material_preset": "Neutral Shadow",
        "refresh": refresh_report,
        "outputs": outputs,
    }
    (output_dir / "render-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return outputs


def main() -> None:
    args = _parse_args()
    config = json.loads(args.config.read_text())
    positions = _read_positions(args.positions)
    refresh = _replace_assembly(args.step, positions, config)
    outputs = _render(args, config, refresh)
    if args.save_blend:
        _scrub_saved_blend_metadata()
        args.save_blend.parent.mkdir(parents=True, exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=str(args.save_blend))
    args.success_file.write_text(json.dumps({"status": "ok", "outputs": outputs}, indent=2) + "\n")
    print(json.dumps({"status": "ok", "outputs": outputs}, indent=2), flush=True)


if __name__ == "__main__":
    main()

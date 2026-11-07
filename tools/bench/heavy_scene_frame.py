"""How much of the viewport a world-space bound covers, from the viewer's free camera.

The camera is the seven numbers the viewport publishes as natronSceneFreeCamera (pivot x, y, z,
distance, azimuth, elevation, fovY) and its view mode; the maths mirrors SceneViewportRenderer's
eyeDirection and viewBasis. A bound is {"min": [x, y, z], "max": [x, y, z]} in stage units.
"""
import math

ORTHOGRAPHIC_MODES = ("front", "back", "right", "left", "top", "bottom")


def _sub(a, b):
    return [a[i] - b[i] for i in range(3)]


def _dot(a, b):
    return sum(a[i] * b[i] for i in range(3))


def _cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def _neg(a):
    return [-x for x in a]


def view_basis(view_mode, azimuth, elevation, z_up):
    """Unit vectors from the pivot to the eye (back) and to the top of the view (up)."""
    world_up = [0.0, 0.0, 1.0] if z_up else [0.0, 1.0, 0.0]
    front = [0.0, -1.0, 0.0] if z_up else [0.0, 0.0, 1.0]
    right_axis = [1.0, 0.0, 0.0]
    up = world_up
    if view_mode == "perspective":
        az, el = math.radians(azimuth), math.radians(elevation)
        ce = math.cos(el)
        if z_up:
            back = [ce * math.sin(az), -ce * math.cos(az), math.sin(el)]
        else:
            back = [ce * math.sin(az), math.sin(el), ce * math.cos(az)]
    elif view_mode == "front":
        back = front
    elif view_mode == "back":
        back = _neg(front)
    elif view_mode == "right":
        back = right_axis
    elif view_mode == "left":
        back = _neg(right_axis)
    elif view_mode == "top":
        back, up = world_up, _neg(front)
    elif view_mode == "bottom":
        back, up = _neg(world_up), front
    else:
        raise ValueError("unknown view mode %r" % (view_mode,))
    return back, up


def frame_coverage(bound, camera, view_mode, z_up, aspect):
    """The share of the frame the bound's projected box covers, in [0, 1], and the reason it is None.

    The projected box is the 2D bounding box of the bound's eight corners, so the share is an upper
    bound on the pixels the geometry can touch. None, with a reason, when the bound crosses the plane
    of the eye (a perspective projection is then unbounded) or the inputs are missing.
    """
    if bound is None:
        return None, "no world bound for this row"
    if camera is None or len(camera) < 7:
        return None, "the viewport has not published a free camera"
    if view_mode not in ("perspective",) + ORTHOGRAPHIC_MODES:
        return None, "view mode %r" % (view_mode,)
    if aspect <= 0:
        return None, "the viewport has no size"
    pivot = [float(camera[0]), float(camera[1]), float(camera[2])]
    distance = max(float(camera[3]), 1e-9)
    fov = float(camera[6])
    back, up = view_basis(view_mode, float(camera[4]), float(camera[5]), z_up)
    right = _cross(up, back)
    eye = [pivot[i] + back[i] * distance for i in range(3)]
    half_height = math.tan(math.radians(fov) / 2.0)
    xs, ys = [], []
    for ix in (0, 1):
        for iy in (0, 1):
            for iz in (0, 1):
                corner = [bound["max" if flag else "min"][axis] for axis, flag in enumerate((ix, iy, iz))]
                offset = _sub(corner, eye)
                depth = -_dot(offset, back)
                if view_mode == "perspective":
                    if depth <= 1e-9:
                        return None, "the bound crosses the plane of the eye"
                    scale = depth * half_height
                else:
                    scale = distance * half_height
                xs.append(_dot(offset, right) / (scale * aspect))
                ys.append(_dot(offset, up) / scale)
    width = max(0.0, min(1.0, max(xs)) - max(-1.0, min(xs)))
    height = max(0.0, min(1.0, max(ys)) - max(-1.0, min(ys)))
    return width * height / 4.0, None

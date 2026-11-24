# ColorWarp

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| ColorWarp | render | 1e-5 absolute | the node's render against `ColorWarpField` built from the same pins (`NativeColorWarp_Test.cpp`) |
| ColorWarp | gpu | `ParityTolerance::make(1e-5, 2e-4)` Gaussian, `make(1e-5, 5e-4)` RBF | the kernel against the CPU row, the hue, skin and look pin sets in every warp space and kernel, three and four components, with and without unprocessed channels (`NativeColorWarpGpu_Test.cpp`) |

ColorWarp moves colours with pins. A pin maps a source colour to a target colour. Colours near
the source move with it, and the move fades with their distance from the source, measured in the
warp space. With no pins, or with every pin disabled, the node passes its input through unchanged.

## Pins

Each pin holds:

- **Source:** the colour the pin is attached to, picked from the input.
- **Target:** where the source moves to.
- **Strength:** how much of the move from source to target the pin applies. At 1 the source
  lands on the target; at 0 the pin holds its colour in place.
- **Falloff:** how far from its source the pin reaches.
- **Enabled:** a disabled pin is ignored.
- **Name**, and the viewer position and frame the source was picked at.

Target, Strength and Falloff can be keyed. Source, position and frame belong to the pick and are
not animated.

A **hold pin** keeps the colours around it where they are, so a nearby pin's move stops at it.
A pin becomes a hold pin when its strength is 0 or its target equals its source. A new pin starts
as a hold pin because its target is its source until it is pushed.

Pins are stored in the node's single Pins parameter. Every pin edit changes the node's hash, so
the viewer and the cache follow it.

## Warp space, kernel and lightness falloff

- **Warp Space:** the space distances are measured in and the move is made in. OKLab Log (the
  default) is OKLab with a log2 encoding in place of the cube root, so it takes even steps per stop
  of exposure. OKLab, XYZ, Log RGB and Linear RGB are also available. Each space's falloff is
  normalised, so a falloff value reaches about as far in every space.
- **Kernel:** the Gaussian (the default) moves the colours within each pin's falloff and lands
  each source exactly on its target. RBF Linear and RBF Cubic interpolate smoothly through every
  pin and ignore Falloff. Their move is full everywhere within the sphere around the pins and fades
  smoothly to none beyond it, over the larger of that sphere's radius and twice the largest pin
  move, so far-away colours, HDR values among them, pass through unchanged.
- **Lightness Falloff:** how far a pin reaches along lightness compared with hue and chroma. At 1
  (the default) the reach is the same in every direction; at 2 it reaches twice as far in
  lightness.

The node also has the shared (Un)premult By, mask and mix controls of the colour nodes. (Un)premult
By is off by default.

## In the viewer

With the node's settings panel open:

- **Click** on the image to add a pin on the colour under the pointer. The new pin is selected.
- **Drag** a pin to pick another source colour. The pin keeps its move: the target shifts by the
  same amount as the source.
- **Delete** or **Backspace** removes the selected pin.

Each pin is drawn as a square, with the source on its left half and the target on its right half.
The selected pin has a white outline, and disabled pins are drawn faded.

## In the settings panel

The pin list shows each pin's Enabled box, its name (double-click to rename it), and its source
and target swatches. Selecting a pin in the list selects it in the viewer too, and the other way
round. **Add Pin** adds a pin on mid grey. **Delete Pin** removes the selected pin.

Below the list are the selected pin's Target, Strength and Falloff. Each has a **Key** button that
sets a key at the current frame, or removes the key there.

## Pushing a target with the global mode

The selected pin's target is a colour the colour global mode can drive. Press **Ctrl+E** with the
pointer over the pin list, or after selecting a pin, to engage the global mode on it. Then use the
C, S, T and V strokes, as on a colour knob. The list is outlined while the global mode drives it.
Selecting another pin moves the global mode to that pin's target.

## Undo

Adding, moving, deleting, renaming, enabling and disabling a pin, keying it, and each global mode
stroke on a target are each one undo step.

## Python

`node.getParam("pins")` returns a `ColorWarpPinsParam`, which addresses pins by their index in the
list:

- `getPinCount()`, `addPin(r, g, b, x, y, frame)` (returns the new pin's index) and
  `removePin(index)`.
- `getPinName` and `setPinName`, `getPinEnabled` and `setPinEnabled`.
- `getPinSource` and `setPinSource` (keeps the pin's move), `getPinPosition` and `getPinFrame`.
- `getPinTarget(index, frame)` and `setPinTarget(index, r, g, b, frame)`, with `setPinTargetKey`
  to set a key.
- `getPinStrength`, `setPinStrength` and `setPinStrengthKey`; `getPinFalloff`, `setPinFalloff` and
  `setPinFalloffKey`.
- `getPinHasKey(index, frame)` and `removePinKeys(index, frame)`.
- `getSelectedPin()` (-1 when none is selected) and `setSelectedPin(index)`.

A bad index raises `IndexError`.

## GPU

The GPU kernel takes three- and four-component images; one- and two-component images stay on the
CPU, where the node passes them through. The device evaluates the field in float and the CPU in
double. The log spaces' exp2 and log2, and the RBF weights carried through a few matrix products,
round in float, which the GPU tolerance above allows for.

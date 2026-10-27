# RFVSwitch

Tolerance: none recorded.

RFVSwitch passes one of its inputs through unchanged, chosen by the value of a Reverse-Flow Variable on the path being rendered. It passes images, deep images and any other data kind alike.

## Parameters

- **Variable**: the name of the variable to switch on, such as `shot`.
- **Input 0**, **Input 1**, ...: one row per pattern, up to 16. The pattern in row Input N chooses input N. The **+** button after a row inserts an empty row below it, and **-** removes its row; the rows below, and the inputs connected to them, move with them, so each pattern keeps choosing the node it chose. Removing the only row empties it instead.
- **If No Match**: what to output when the variable is unset or matches no pattern.
  - **Error** fails the render, and the node shows the variable and its value.
  - **First Input** passes input 0 through.

## Matching

The first row that matches the variable's whole value wins.

- A pattern starting with `re:` is an ECMAScript regular expression, written after the prefix. `re:sh0[2-9]0` matches `sh030` but not `sh0300`.
- Any other pattern is a glob. `*` matches any characters, `?` matches one, `[abc]` matches one of those, `[!abc]` matches one not among them, and `[a-z]` matches a range. There is no escape character, and matching is case-sensitive.
- An empty pattern never matches, but it still holds its input's place.
- A regular expression that does not compile never matches. The node reports the first such row, whichever input is chosen.

Every variable is matched as text, and the error message shows the same text:

| Variable | Value | Matched as |
| --- | --- | --- |
| String | `sh010` | `sh010` |
| Boolean | on | `True` |
| Integer | `12` | `12` |
| Choice | second option | `1` (its index) |
| Floating point | `5.0` | `5` |
| Floating point | `0.1` | `0.1` (the shortest text that reads back as the same number) |
| 2D/3D/Color | `1920, 1080` | `1920, 1080` (the items' text joined by `, `, without brackets, since `[` opens a glob class) |

For example, with `shot` set to `sh010` and the patterns

```
Input 0: sh01*
Input 1: re:sh0[2-9]0
```

the switch passes input 0 through. With `shot` set to `sh030` it passes input 1.

## Which value is read

The variable is read exactly as `rfv.<name>` would read it in an expression of the switch. Under an RFVSet node that sets it, the value is that RFVSet node's. An animated or expression-driven RFVSet variable is evaluated at the time the switch is rendered. Elsewhere the value is the project's.

One RFVSwitch under two RFVSet nodes therefore chooses separately on each path. In a graph like

```
A   B
 \ /
RFVSwitch  (Variable: var, Input 0: a, Input 1: b)
 /     \
RFVSet   RFVSet
var=a    var=b
```

the first RFVSet's branch shows A and the second's shows B. Only A and B render, each once, and the other inputs are never asked for.

## Inputs

Input 0 is always shown. Input i is also shown while row Input i is, or while it is connected. A chosen input that is not connected outputs black.

## Errors

With If No Match set to Error, a render that finds no match fails. The node's error message names the variable and the text it was matched as, or says that it is unset.

Only renders post or clear the message, previews aside: a value read outside a render, under a context no render reached the switch with, never shows. The message is what the latest render reaching the switch decided. When several paths in it reach the switch, the message lists the failure of each path that failed, and it is cleared once a render reaches the switch and every path through it succeeds. Editing a project variable the switch reads, or the value an RFVSet below it sets, renders it again at once.

# RFVSwitch

Tolerance: none recorded.

RFVSwitch passes one of its inputs through unchanged, chosen by the value of a Reverse-Flow Variable on the path being rendered. It passes images, deep images and any other data kind alike.

## Parameters

- **Variable**: the name of the variable to switch on, such as `shot`.
- **Patterns**: one pattern per line. Line 1 chooses input 0, line 2 input 1, and so on, up to 16 lines. Lines past the 16th are ignored.
- **If No Match**: what to output when the variable is unset, is a list, or matches no line.
  - **Error** fails the render, and the node shows the variable and its value.
  - **First Input** passes input 0 through.

## Matching

The first line that matches the variable's whole value wins.

- A line starting with `re:` is an ECMAScript regular expression, written after the prefix. `re:sh0[2-9]0` matches `sh030` but not `sh0300`.
- Any other line is a glob. `*` matches any characters, `?` matches one, `[abc]` matches one of those, `[!abc]` matches one not among them, and `[a-z]` matches a range. There is no escape character, and matching is case-sensitive.
- An empty line never matches, but it still holds its input's place.
- A regular expression that does not compile never matches. The node reports the first such line, whichever input is chosen.
- A value that is not a string is matched as Python's `str()` gives it, so an int variable `3` matches the line `3`.

For example, with `shot` set to `sh010` and the patterns

```
sh01*
re:sh0[2-9]0
```

the switch passes input 0 through. With `shot` set to `sh030` it passes input 1.

## Which value is read

The variable is read exactly as `rfv.<name>` would read it in an expression of the switch. Under a Set node that sets it, the value is that Set node's. An animated or expression-driven Set variable is evaluated at the time the switch is rendered. Elsewhere the value is the project's.

One RFVSwitch under two Set nodes therefore chooses separately on each path. In a graph like

```
A   B
 \ /
RFVSwitch  (Variable: var, Patterns: a / b)
 /     \
Set      Set
var=a    var=b
```

the first Set's branch shows A and the second's shows B. Only A and B render, each once, and the other inputs are never asked for.

## Inputs

Input 0 is always shown. Input i is also shown while Patterns has a line i, or while it is connected. A chosen input that is not connected outputs black.

## Errors

With If No Match set to Error, a render that finds no match fails. The node's error message names the variable and its value, or says that it is unset. The message belongs to the node, so when two paths through the switch disagree, it shows the path rendered last.

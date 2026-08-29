# Architecture

```
src/core/     the language -- GLib + libm, no GTK
  m42-value     what an expression evaluates to: number (exact when it
                can be), complex number, string, list, symbolic
                expression, function, graph, error
  m42-lexer     source text to tokens
  m42-parser    tokens to a syntax tree (recursive descent)
  m42-symbolic  trees as mathematics: printing, simplifying,
                differentiating, integrating, expanding, substituting
  m42-matrix    linear algebra: multiply, determinant, inverse, solve,
                eigenvalues by Jacobi or QR
  m42-eval      the session: variables, scopes, builtins, control flow
  m42-help      every function with a line about it
src/ui/       the window -- GTK 4
  m42-application  GtkApplication, accelerators, --screenshot,
                   --activate, --export-pdf, --size
  m42-window       menu bar, scrolling notebook, input line with its
                   history, status bar, the function reference
  m42-typeset      a value laid out as boxes and drawn: fractions,
                   powers, radicals, matrices, graphs in two dimensions
                   and surfaces in three
  m42-notebook     the cells themselves, and the PDF of the page
src/calc-main.c  math42-calc, the engine from a terminal
data/         menus.ui, style.css, icon, desktop entry -- compiled into
              the binary as a GResource
```

The core is a static library that the `math42` and `math42-calc`
executables both link. Only the core sees `M42Session`; the UI talks
to it through `m42_session_eval`, which never returns NULL — a failed
parse or evaluation is an `M42_VALUE_ERROR` the notebook paints in
red, and a line ending in `;` is `M42_VALUE_NULL`, which it does not
paint at all.

## How a line is answered

`m42_session_eval` parses the line into one `M42_NODE_SEQ` and walks
it. Most calls have their arguments evaluated first and are then
looked up among the builtins; the forms that must not have theirs
evaluated — `D`, `Integrate`, `Plot`, `Table`, `Sum`, `For`, `While`,
`Solve` and the rest — are picked out by name before that happens, and
read their own trees.

A name with no value evaluates to itself as an `M42_VALUE_EXPR`, which
is what makes symbolic algebra fall out of ordinary evaluation: `2 x`
multiplies a number by an expression, `map2` sees that one side is an
expression and builds a tree, and `m42_node_simplify` tidies it. The
same trees are what `D` differentiates and `Integrate` integrates.

Numbers are doubles, but a whole one — and a quotient of two whole
ones — carries the fraction it really is in a `gint64` pair beside the
double, so `1/3 + 1/6` is exactly `1/2`. `exact_op` does that
arithmetic in `__int128` and hands back to the doubles when a result
will not fit.

## Grammar

Loosest to tightest:

```
program := stmt (';' stmt)* [';']
stmt    := IDENT ('=' | ':=') stmt | IDENT '[' params ']' ':=' stmt
         | replace
replace := rule ('/.' rule)*
rule    := or ('->' or)?
or      := and ('||' and)*
and     := not ('&&' not)*
not     := '!' not | cmp
cmp     := range (('==' | '!=' | '<' | '<=' | '>' | '>=') range)?
range   := sum (':' sum (':' sum)?)?
sum     := product (('+' | '-') product)*
product := unary (('*' | '/' | '%' | '.' | '\') unary | juxtaposed)*
unary   := '-' unary | '+' unary | power
power   := postfix ('^' unary)?             right associative
postfix := primary ('!' | '\'' | '[[' args ']]')*
primary := NUMBER | STRING | '%' | IDENT | IDENT '[' args ']'
         | IDENT '(' args ')' | '(' stmt ')' | '{' args '}'
         | '[' rows ']' | '@' '(' params ')' stmt | '?' IDENT
```

`Sin[x]` and `sin(x)` both call a function; a number or name followed
by another multiplies, as in `2x` and `x y` — except inside `[ ]`,
where a space separates the elements of a MATLAB matrix. `f[[i]]` is
a part, unless what is inside looks like a matrix (`Eigenvalues[[2 1;
1 2]]`), which a semicolon or two values side by side give away.

## Drawing

`m42-typeset` turns a value into a tree of boxes, each of which knows
its width, its ascent above the baseline and its descent below it. A
row lines its boxes up on one baseline; a fraction sits on the maths
axis with its bar drawn between; a matrix is a grid inside brackets
that grow with it; a graph is a box of fixed size that paints itself
with Cairo when the notebook asks. Because every box measures itself
at the size it was built for, the whole layout scales with the View
menu's zoom, and the same drawing goes to the screen and to the PDF.

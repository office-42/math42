# The Math42 User Guide

Math42 is a mathematics notebook that speaks two languages: the one
Mathematica speaks and the one MATLAB speaks. `Sin[Pi/2]` and
`sin(pi/2)` both work, `{1, 2, 3}` and `[1 2 3]` are both lists, and
`f[x_] := x^2` and `f = @(x) x^2` both define a function.

This guide is in three parts: [using the notebook](#1-the-notebook),
[the language](#2-the-language), and
[what is in it](#3-what-is-in-it) — every group of functions with what
it does. Then comes a [course by course map](#4-course-by-course) of the
mathematics an NTNU engineering degree teaches, the two compatibility
tables, one for [Mathematica](#5-coming-from-mathematica) and one for
[MATLAB](#6-coming-from-matlab), and a note on
[what is not there](#7-what-math42-does-not-do).

---

## 1. The notebook

### Starting it

```sh
math42                  # an empty notebook
math42 notebook.m42     # a notebook, played back line by line
math42-calc             # the same engine in a terminal
```

Type an expression on the line at the bottom and press **Enter**. The
answer is added to the page as an `In[n]:=` / `Out[n]=` pair, set as
mathematics: fractions stacked, powers raised, roots under a radical,
matrices inside brackets, graphs drawn.

### Getting around

| | |
|---|---|
| **Enter** | evaluate the line |
| **↑** and **↓** | walk back and forth through what you have typed |
| **click a cell** | put its input back on the line, to run again with a change |
| **Ctrl +**, **Ctrl −**, **Ctrl 0** | make the mathematics bigger, smaller, ordinary |
| **F1** | the function reference, with a box that narrows the list as you type |
| **Ctrl S**, **Ctrl O** | save and open a `.m42` file — the inputs, which are played back |
| **Ctrl P** | print, through whatever printers the machine has |
| **File ▸ Export as PDF** | the whole page as vector graphics |
| **Ctrl L** | clear the page; **Evaluation ▸ Clear Variables** forgets what is defined |
| **Ctrl Q** | quit |

A `.m42` file is a plain text file of inputs, one per line. Opening
one runs every line from the top, so a notebook is reproducible by
construction.

### On paper

Printing and Export as PDF lay the notebook out the same way: the
cells down the page, a cell that will not fit moved to the next page,
and one taller than a whole page drawn smaller so that all of it is
there. Every page carries the file's name at the top and its number at
the foot, and the PDF carries the name in its properties, so a printed
page can be filed and a PDF found by searching. A result wider than the
page — a graph — is drawn to fit it, as it is in a narrow window.

Everything is vector: the type is type and the graphs are lines, so a
page stands up to being enlarged or printed at any size.

From a terminal, `math42 --export-pdf out.pdf notebook.m42` writes the
PDF and `math42 --print-to out.pdf notebook.m42` sends the notebook
through the printing machinery itself and puts what comes out in a
file, which is how the printed pages can be looked at where there is
no printer.

### Asking what something does

```
?Sin                       Sin[x] -- the sine, in radians (MATLAB: sin)
Information["Integrate"]   the same, spelled out
Names[]                    every function math42 knows
```

### Data in and out

A table of numbers on disk is read by `Import` and written by `Export`,
with MATLAB's `csvread`, `csvwrite`, `readmatrix` and `writematrix`
alongside. A `.csv`, `.tsv` or `.dat` file comes back as a list of
rows — or as a plain list when it has one column, which is what a
column of numbers is usually wanted as. Anything else comes back as a
string, which `ReadString` also does.

```
d = Import["data.csv"]        {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}}
Total[d]                      {12, 15, 18}
Export["out.csv", [1 2; 3 4]]
```

`Export` takes the name first, as Mathematica does, and
`writematrix(table, "out.csv")` takes it last, as MATLAB does;
whichever argument is the string is the name.

### Files from other programs

File ▸ Open reads four kinds of file and File ▸ Save As writes them,
chosen by the ending you give:

| | |
|---|---|
| `.m42` | a math42 notebook: the inputs, one to a line |
| `.m` | a MATLAB script |
| `.wl`, `.wls` | a Wolfram Language script |
| `.nb` | a Mathematica notebook |

A MATLAB script needs a little translating, and gets it: `%` comments
become `(* … *)` on the way in and `%` again on the way out, since `%`
means the previous output here; a block fenced by `%{` and `%}` on
lines of their own is passed over entirely; a line ending in `...` is
joined to the next; and `'single quotes'` become `"double"` ones, while `A'` stays
the transpose it is — and so does the second `'` of `y''`, which is how
a second derivative is written. An apostrophe inside a string is kept:
MATLAB's `'it''s'` is read as `"it's"`, and a `'` inside a
double-quoted string is left alone. What math42 cannot do — a `function` file, a cell
array — is left as it was written, and says so on the line it is on.

A Mathematica notebook is itself a Wolfram expression, and math42 reads
the input cells out of one and writes one made of them — with the
results beside them, as `Cell[…, "Output"]`, so that somebody opening
the file in Mathematica sees what math42 answered. Reading one back
takes the input cells and passes over the rest, so a round trip gives
the notebook it started as. Input typed in Mathematica's own
two-dimensional way is read as what it means: `SuperscriptBox["x", "2"]`
is `x^2`, a `FractionBox` is a fraction, a `SqrtBox` a root, and an
input and its output inside one `CellGroupData` are two cells, not one.
The characters Mathematica writes by name are read as what they mean —
`\[Rule]` is `->`, `\[Equal]` is `==`, `\[LessEqual]` is `<=` — and a
name math42 has no spelling for, a Greek letter among them, is left as
it stands, so that it is complained about rather than quietly misread.

**MATLAB's own `.mat` files.** `Import["run.mat"]` reads the variables
of a MATLAB session and `Export["run.mat", m]` writes them, under
MATLAB's own names — `load` and `save` are the same two functions
under MATLAB's spelling:

```
In[m1]:= Export["run.mat", {"A" -> {{1, 2}, {3, 4}}, "v" -> {1, 2, 3}}]
In[m2]:= load("run.mat")     Out[m2]= {"A" -> {{1, 2}, {3, 4}}, "v" -> {1, 2, 3}}
```

A file of one variable comes back as that variable; several come back
as the rules `name -> value`. math42 reads the level 5 files every
MATLAB since 1996 writes, compressed (version 7) or not, and the older
level 4 files as well; it writes uncompressed level 5 ones, which
MATLAB, Octave and SciPy all read. It does not read the HDF5 files
`-v7.3` writes, and holds matrices of numbers and strings but not
cells, structures or objects.

`Import` of a notebook or a script — `.m42`, `.m`, `.wl`, `.wls`,
`.nb` — gives the lines it holds, a MATLAB script's comments and
continuations put right and a Mathematica notebook's cells read out of
the boxes they are written in. `ToExpression` runs one of them. An expression
spread over several lines is put back together as the file is read.

From a terminal, `math42-calc script.m` runs any of them, and
`math42 --convert out.nb notebook.m42` turns one into another without
opening a window.

A line that is only a comment becomes a line of text on the page, with
no `In[n]` beside it and nothing worked out — which is how the comments
in a script survive being read.

### Four notebooks to start from

`examples/showcase.m42`, `examples/tour.m42`, `examples/calculus.m42`
and `examples/matlab.m42` come with math42. Open one with File ▸ Open,
or run `math42 examples/showcase.m42` from a terminal. The first is
the page in the picture at the top of the README. A line beginning `(*` is a
comment and is passed over — and since a notebook reads a line at a
time, a comment has to fit on one.

### Looking at a notebook from a script

```sh
math42 --size 880x1000 --screenshot page.png notebook.m42
math42 --export-pdf page.pdf notebook.m42
math42 --activate reference --screenshot ref.png    # with a dialog open
```

---

## 2. The language

### Numbers

Whole numbers and their fractions are kept **exactly**, and a whole
number that outgrows the machine keeps going on its own digits:

```
In[1]:= 1/3 + 1/6
Out[1]= 1/2
In[2]:= 2^-3
Out[2]= 1/8
In[3]:= N[1/3]
Out[3]= 0.333333333333333
In[4]:= Rationalize[0.75]
Out[4]= 3/4
In[5]:= 2^100
Out[5]= 1267650600228229401496703205376
In[6]:= 100!/99!
Out[6]= 100
```

A number written with a decimal point is inexact and stays that way.
`N[x]` asks for the decimal, `Rationalize[x]` asks for the fraction.

Complex numbers use `I`:

```
In[5]:= Sqrt[-9]
Out[5]= 3 I
In[6]:= (1 + 2I)(3 - I)
Out[6]= 5 + 5 I
In[7]:= Abs[3 + 4I]
Out[7]= 5
```

`Re`, `Im`, `Conjugate` and `Arg` take one apart.

### Exactness, and which spelling you use

`Pi`, `E`, `Degree`, `GoldenRatio` and `EulerGamma` — the names
Mathematica uses — stay as themselves until a number is actually
needed, and a function asked for by its **capitalised** name gives an
exact answer where mathematics has one:

```
In[8]:=  Sin[Pi]        Out[8]=  0
In[9]:=  Sin[Pi/3]      Out[9]=  Sqrt[3]/2
In[10]:= Sqrt[8]        Out[10]= 2 Sqrt[2]
In[11]:= Log[E]         Out[11]= 1
In[11b]:= ArcSin[1/2]   Out[11b]= Pi/6
In[11c]:= ArcTan[1]     Out[11c]= Pi/4
```

The **lower-case MATLAB spellings** always hand back a decimal, which
is what a MATLAB user expects:

```
In[12]:= sin(pi)        Out[12]= 1.22460635382238e-16
In[13]:= sqrt(8)        Out[13]= 2.82842712474619
```

So the spelling picks the dialect. Wherever a number is genuinely
needed — a plot's bounds, an integral, a table — a constant expression
is worked out on the spot, so `Plot[Sin[x], {x, 0, 2 Pi}]` is fine.

### Variables, and symbols

`=` gives a name a value. A name with no value is a **symbol**, and
arithmetic on a symbol builds an expression rather than an error:

```
In[14]:= expr = a q^2 + b q + c
Out[14]= a q^2 + b q + c
In[15]:= expr /. {a -> 1, b -> 2, c -> 3}
Out[15]= q^2 + 2 q + 3
In[16]:= D[expr, q]
Out[16]= 2 a q + b
```

`expr /. rule` substitutes; a rule is written `name -> value`, and a
list of rules substitutes all of them. `%` is the previous answer.

`x == 2` and `y >= 3` are **equations**, kept as they are written;
`SameQ[a, b]` (MATLAB's `isequal`) is the one that answers yes or no.

### Lists and matrices

```
{1, 2, 3}          a list, the Mathematica way
[1 2 3]            the same list, the MATLAB way
[1 2; 3 4]         a matrix: two rows of two
Range[5]           {1, 2, 3, 4, 5}
1:2:9              {1, 3, 5, 7, 9}
Range[0, 1, 0.25]  {0, 0.25, 0.5, 0.75, 1}
```

Arithmetic works element by element, and a number spreads over a list:

```
In[17]:= {1, 2, 3} + {10, 20, 30}     Out[17]= {11, 22, 33}
In[18]:= {1, 2, 3}^2                  Out[18]= {1, 4, 9}
In[19]:= [1 2; 3 4] * [1 2; 3 4]      Out[19]= {{1, 4}, {9, 16}}
In[20]:= [1 2; 3 4] . [1 2; 3 4]      Out[20]= {{7, 10}, {15, 22}}
```

`*` multiplies element by element and `.` is matrix multiplication, as
in Mathematica; MATLAB's `.*` and `*` are accepted for the same two.

### Taking things out of a list

| written | means |
|---|---|
| `x[[3]]`, `x(3)` | the third |
| `x[[-1]]`, `x(end)` | the last |
| `x[[2 ;; 4]]`, `x(2:4)` | the second to the fourth |
| `x[[;;3]]`, `x[[8;;]]` | up to the third, from the eighth on |
| `x[[{1, 3, 5}]]` | those three |
| `A[[2, 3]]`, `A(2, 3)` | row two, column three |
| `A[[All, 2]]`, `A(:, 2)` | the whole second column |
| `A[[2]]`, `A(2, :)` | the whole second row |

### Functions

Four ways to write one, all of which mean the same thing:

```
f[x_] := x^2 + 1        Mathematica's definition
f(x) = x^2 + 1          the same, with round brackets
g = #^2 + 1 &           a pure function; # is its argument
h = @(a, b) a + b       MATLAB's anonymous function
```

`#1`, `#2` are the first and second arguments of a pure function, and
`#` is short for `#1`. A function value can be called straight away:
`(#^2 &)[4]` is 16.

### Applying a function

```
Map[f, {1, 2, 3}]     f /@ {1, 2, 3}      {f[1], f[2], f[3]}
Apply[Plus, {1, 2}]   Plus @@ {1, 2}      Plus[1, 2]
f[x]                  f @ x               x // f
```

All three shorthands mean what they mean in Mathematica: `/@` maps,
`@@` applies, `@` calls, and `//` hands what is on its left to what is
on its right.

### Several things on one line

`;` separates statements, and a trailing `;` hides the answer:

```
In[21]:= a = 2; b = 3; a b
Out[21]= 6
In[22]:= x = 10;                (nothing is shown)
```

### Deciding and repeating

Mathematica's forms are functions:

```
If[test, then, else]
Which[test1, value1, test2, value2]
For[i = 1, i <= 5, i = i + 1, body]
While[test, body]
Do[body, {i, 1, 10}]
Module[{t = 5}, t^2]              names local to the body
```

MATLAB's blocks are written as blocks, with `end`:

```
total = 0; for i = 1:5, total = total + i; end; total
k = 1; while k < 100, k = k * 2; end; k
if x > 5, disp("big"); elseif x > 2, disp("middling"); else, disp("small"); end
```

A block written on one line wants a comma or a semicolon after its
header, exactly as MATLAB wants one. `for i = list` walks a list; a
single value runs the body once.

### Patterns

A pattern is an expression with holes in it.

| written | stands for |
|---|---|
| `_` | any one thing |
| `x_` | any one thing, and calls it `x` |
| `_Integer` | any one thing with that head — also `_Real`, `_Rational`, `_String`, `_List`, `_Symbol`, `_Plus`, `_Times`, `_Power`, or the name of a function |
| `x__` | one thing or more, side by side |
| `x___` | none, or as many as you like |
| `p /; test` | only when the test comes out true |
| `p ? f` | only when `f` says so of what filled it: `x_?OddQ` |
| `p \| q` | one shape or the other: `_Integer \| _String` |

A name that turns up twice has to stand for the same thing both times,
which is what makes `Sin[u_]^2 + Cos[u_]^2` mean what it says.

```
In[p1]:= Cases[{1, 2.5, "a", {3}}, _Integer]         Out[p1]= {1}
In[p2]:= MatchQ[3, _Integer]                         Out[p2]= 1
In[p3]:= Count[{1, 2, 3, 4}, n_ /; n > 2]            Out[p3]= 2
In[p4]:= DeleteCases[{1, 2.5, "a", 3}, _Integer]     Out[p4]= {2.5, "a"}
In[p5]:= FreeQ[x^2 + 1, Sin[_]]                      Out[p5]= 1
In[p6]:= Cases[x^2 + Sin[y]^2 + 3, p_^2, All]        Out[p6]= {x^2, Sin[y]^2}
```

**Replacing.** A rule whose left side is a shape replaces wherever that
shape is found, outermost first. `//.` goes round again as long as
something changed, and `:>` keeps its right side as written rather than
working it out when the rule is made.

```
In[p7]:= x^2 + y^3 /. x_^2 -> Q                      Out[p7]= Q + y^3
In[p8]:= f[a, b, c] /. f[x__] -> g[x]                Out[p8]= g[a, b, c]
In[p9]:= Sin[a + b] /. Sin[u_ + v_] -> Sin[u] Cos[v] + Cos[u] Sin[v]
         Out[p9]= Sin[a] Cos[b] + Cos[a] Sin[b]
In[p10]:= {5, 3, 4, 1} //. {a___, m_, n_, b___} /; m > n -> {a, n, m, b}
          Out[p10]= {1, 3, 4, 5}
```

The last of those is the whole of a sort: whenever two neighbours are
the wrong way round, swap them, until none are. `ReplaceAll` and
`ReplaceRepeated` are the same two operators under their own names.

Sums and products are held here as trees of two branches rather than
the flat sums Mathematica matches against, so `a_ + b_` finds `a + b`
in `a + b + c` and leaves `c` over. Both ways round are tried, which is
what lets `c_ x_` find the `2` in `2 x`.

**Definitions that dispatch on a shape.** A name may be defined more
than once, and the definition whose shape fits is the one used. The
ones with no hole in them are tried first, so a base case is found
whichever order it was written in.

```
In[p11]:= fib[0] = 0; fib[1] = 1; fib[n_] := fib[n - 1] + fib[n - 2]
In[p12]:= fib[30]                                    Out[p12]= 832040
In[p13]:= area[r_] := Pi r^2 ; area[2]               Out[p13]= 4 Pi
```

`Clear[f]` forgets a name: what it held, and everything defined for it.
Bare `clear`, the MATLAB way, forgets the lot.

**One more thing about names.** A name with an underscore in the middle
is a pattern only when what follows it looks like a head, which is to
say it begins with a capital. So `my_var` stays the ordinary MATLAB
name it looks like, while `x_Integer` is the pattern Mathematica means
by it.

### Changing one place in a list

`v[[2]] = 9` changes the one place and leaves the rest, and
`A[[1, 2]] = 7` reaches into a matrix. MATLAB writes the same thing
`v(2) = 9`, and in that spelling a place past the end makes the list
longer, filling out with zeros.

```
In[q1]:= v = {1, 2, 3}; v[[2]] = 9; v          Out[q1]= {1, 9, 3}
In[q2]:= A = [1 2; 3 4]; A[[1, 2]] = 7; A      Out[q2]= {{1, 7}, {3, 4}}
In[q3]:= u = {1, 2}; u(5) = 3; u               Out[q3]= {1, 2, 0, 0, 3}
```

### Writing text

```
"quoted text"                      a string
Print["x is ", x]                  written above the result
sprintf("%5.2f apples", 3.14159)   "  3.14 apples"
fprintf("%d of %d\n", 3, 10)       the same, written out
StringJoin["a", "b"]               "ab"
```

`%d`, `%i`, `%f`, `%e`, `%g` and `%s` are understood with their widths
and precisions, and `\n` and `\t` do what they usually do.

---

## 3. What is in it

Every function below is in the F1 reference too, with its MATLAB
spelling. Where two names are given, either will do.

### Calculus

| | |
|---|---|
| `D[f, x]`, `D[f, {x, n}]` | the derivative, to any order |
| `D[f, x, y]` | the mixed partial, one variable after the other |
| `Integrate[f, x]` | the antiderivative. A letter inside a function is no obstacle: `Integrate[Sin[a x], x]` is `-(Cos[a x]/a)` and `Integrate[Sqrt[a x + b], x]` is `2 (a x + b)^(3/2)/(3 a)` |
| `Integrate[f, {x, a, b}]` | between bounds: exact if it can be, Simpson's rule if not |
| `NIntegrate[f, {x, a, b}]` | always numeric |
| `Limit[f, x -> a]` | including `x -> Infinity`; found numerically, and written as the constant when it is one — `Limit[(1 + 1/n)^n, n -> Infinity]` is `E` |
| `Residue[f, {x, a}]` | what is left of the function at a pole, up to order four: `Residue[1/(x^2 - 1), {x, 1}]` is `1/2` |
| `Series[f, {x, a, n}]` | the Taylor polynomial |
| `NDSolve[f, {x, a, b}, y0]`, `ode45` | solves y′ = f(x, y) by Runge–Kutta |
| `DSolve[eqn, y, x]` | the linear equations, exactly: a first order one by its integrating factor, a second order one with constant coefficients by variation of parameters |
| `DSolve[{x' == a x + b y, y' == c x + d y}, {x, y}, t]` | a system of two, by the eigenvalues of its matrix: two real rates give exponentials, a turning pair gives a wave inside one, and a repeated rate gives the extra `t`. Starting values may follow the equations, as `x[0] == 1, y[0] == 0` |
| `RSolve[eqn, a, n]` | a linear recurrence, in closed form |

The integration rules are the ones a first course teaches:

- powers, sums and constant multiples;
- the standard functions of a linear argument, `Sin[2x + 1]` and its kind;
- a rational function by **partial fractions** — two real roots into two
  logarithms, a repeated root into a logarithm and a reciprocal, complex
  roots into a logarithm and an inverse tangent, with the whole part
  divided out first;
- `1/Sqrt[a - x^2]` and `1/Sqrt[a + x^2]`, which give the inverse sine
  and its hyperbolic cousin;
- an exponential times a sine or cosine, the pair that comes back to
  itself after two integrations by parts;
- `Sin[a x]^2` and `Cos[a x]^2`, through the double angle;
- **by parts**, with the polynomial differentiated — or the logarithm or
  inverse tangent, when there is one, since its derivative is algebraic;
- **substitution**: `f'/f` into a logarithm, `f^n f'` into the next
  power, and `f(u) u'` by integrating the outer function on its own.

An integral outside them is kept as it was written, and drawn under its
sign.

```
In[a]:= Integrate[x Log[x], x]         Out[a]= x^2 Log[x]/2 - x^2/4
In[b]:= Integrate[1/(x^2 + 2x + 2), x] Out[b]= ArcTan[x + 1]
In[c]:= Integrate[Log[x]/x, x]         Out[c]= Log[x]^2/2
In[d]:= Integrate[x Exp[x^2], x]       Out[d]= Exp[x^2]/2
```

### What Integrate knows

The power rule, sums and constant multiples; `1/(a x + b)`; the
standard functions of a linear argument; a polynomial times one of
those, by parts; a rational function by partial fractions, for a
denominator of any degree; the two square roots

```
In[i1]:= Integrate[Sqrt[1 - x^2], x]   Out[i1]= x Sqrt[1 - x^2]/2 + ArcSin[x]/2
In[i2]:= Integrate[Sqrt[x^2 + 1], x]   Out[i2]= x Sqrt[x^2 + 1]/2 + Log[Abs[x + Sqrt[x^2 + 1]]]/2
```

and their reciprocals, which give the inverse sine and the inverse
hyperbolic sine; an exponential times a sine or a cosine; any whole
power of a sine, cosine or tangent, by the reduction that takes two
off the power at a time

```
In[i3]:= Integrate[Sin[x]^3, x]        Out[i3]= -(Sin[x]^2 Cos[x]/3) - 2 Cos[x]/3
In[i4]:= Integrate[Tan[x]^2, x]        Out[i4]= Tan[x] - x
In[i5]:= Integrate[1/Cos[x]^2, x]      Out[i5]= Tan[x]
```

the Gaussian, through the error function; and the two substitutions
that carry most of the rest, `f'/f` and `f^n f'`, either of which may
be out by a number or a minus sign:

```
In[i6]:= Integrate[Cos[x]^2 Sin[x], x] Out[i6]= -(Cos[x]^3/3)
In[i7]:= Integrate[1/(x Log[x]), x]    Out[i7]= Log[Abs[Log[x]]]
In[i8]:= Integrate[Abs[x], x]          Out[i8]= x Abs[x]/2
```

An end at infinity is allowed. When there is an antiderivative it is
asked further and further out until it settles; when there is not, the
range is pulled back into a finite one by `x = a + t/(1 - t)` and
Simpson does the rest.

```
In[i8]:= Integrate[Exp[-x] Sin[x], {x, 0, Infinity}]   Out[i8]= 0.5
In[i9]:= NIntegrate[Exp[-x^2], {x, -Infinity, Infinity}]
         Out[i9]= 1.77245385090552
```

That substitution only works for a function that dies away. When what
it hands Simpson grows towards the far end instead — `Sin[x]/x`, whose
tail oscillates for ever — the integral is left as it was written
rather than answered wrongly.

What it does not find at all, it works out with Simpson's rule when
there are limits, and leaves as it was written when there are not.
Where Simpson cannot look — a function with no value at one of the
ends, as `1/Sqrt[x]` has none at nothing — a rule that crowds its
points towards the ends and lets its weights die away there takes
over, so `NIntegrate[1/Sqrt[x], {x, 0, 1}]` is 2 and
`NIntegrate[Log[x]/Sqrt[x], {x, 0, 1}]` is -4.

Simpson's rule is used on two thousand panels, and checks itself by
adding the same samples up again on half as many: where the two
disagree there is something in the function narrower than a panel, and
the range is halved and done again until they agree. So a spike a
thousandth of the range wide, which the panels would otherwise walk
past, is found. `Exp[x^2]`
and `Sin[x]/x` are the usual examples: their antiderivatives are not
elementary, and math42 has no `Erfi` or `SinIntegral` to name them
with.

### Differential equations and recurrences

`DSolve` solves the linear equations with constant coefficients, and a
first order equation whose coefficients are anything it can integrate,
by the integrating factor `Exp[Integrate[p, x]]`:

```
In[d1]:= DSolve[y' == x y, y, x]              Out[d1]= C1 Exp[x^2/2]
In[d2]:= DSolve[y' + y/x == 1, y, x]          Out[d2]= (x^2/2 + C1)/x
In[d3]:= DSolve[{y' == x y, y[0] == 5}, y, x] Out[d3]= 5 Exp[x^2/2]
```

The bars of the `Abs` that comes out of `Integrate[1/x, x]` are dropped
at that step, as every textbook drops them: the constant in front takes
care of the sign.

A first order equation that is not linear but separates into a power of
`y` is solved by separating it:

```
In[d4]:= DSolve[y' == y^2, y, x]                  Out[d4]= -(1/(x + C1))
In[d5]:= DSolve[{y' == y^2, y[0] == 1}, y, x]     Out[d5]= -(1/(x - 1))
In[d6]:= DSolve[y' == Sqrt[y], y, x]              Out[d6]= (x/2 + C1/2)^2
```

What is left over — `y' == y^2 + x` above all — is not solved, and
says so.

A second order equation with constant coefficients is solved whatever
stands on its right, by variation of parameters: the two solutions of
the homogeneous equation give a Wronskian W, and

    yp = -y1 Integrate[(y2 R)/(a W), x] + y2 Integrate[(y1 R)/(a W), x]

solves the whole of it, so long as math42 can do the two integrals.

```
In[v1]:= DSolve[y'' + y == Exp[x], y, x]
Out[v1]= C1 Cos[x] + C2 Sin[x] + Exp[x]/2
In[v2]:= DSolve[y'' + 4 y == Sin[x], y, x]
Out[v2]= C1 Cos[2 x] + C2 Sin[2 x] + Sin[x]/3
In[v3]:= DSolve[y'' + 2 y' + y == Exp[-x], y, x]
Out[v3]= C1 Exp[-x] + C2 x Exp[-x] + Exp[-x] x^2/2
In[v4]:= DSolve[{x' == -y, y' == x, x[0] == 1, y[0] == 0}, {x, y}, t]
Out[v4]= {x -> Cos[t], y -> Sin[t]}
```

The second of those comes back from the integrals as four products of
waves; it is `TrigReduce` that makes `Sin[x]/3` of them.

The primes are written as Mathematica writes them, and conditions may
follow in the list:

```
In[e]:= DSolve[y'' + y == 0, y, x]
Out[e]= C1 Cos[x] + C2 Sin[x]
In[f]:= DSolve[{y'' - 3y' + 2y == 0, y[0] == 1, y'[0] == 0}, y, x]
Out[f]= -Exp[2 x] + 2 Exp[x]
In[g]:= DSolve[{y' + 2y == 6, y[0] == 0}, y, x]
Out[g]= -3 Exp[-2 x] + 3
```

`RSolve` does the same for a recurrence, through its characteristic
roots:

```
In[h]:= RSolve[{a[n] == a[n-1] + a[n-2], a[0] == 0, a[1] == 1}, a, n]
Out[h]= 0.447213595499958 1.61803398874989^n - 0.447213595499958 (-0.618033988749895)^n
```

which is Binet's formula for the Fibonacci numbers.

```
In[23]:= Integrate[x Exp[x], x]        Out[23]= x Exp[x] - Exp[x]
In[24]:= Integrate[1/(2u + 1), u]      Out[24]= Log[Abs[2 u + 1]]/2
In[25]:= Series[Exp[u], {u, 0, 4}]     Out[25]= 1 + u + 1/2 u^2 + 1/6 u^3 + 1/24 u^4
```

### Algebra

| | |
|---|---|
| `Expand[e]` | multiplies out and gathers like terms |
| `Factor[p]` | a polynomial as a product of its factors |
| `Simplify[e]`, `FullSimplify[e]` | the identities, the cancelling and the multiplying out, whichever comes out shorter |
| `Simplify[e, x > 0]`, `Refine[e, x > 0]` | with what may be assumed about a letter: `Simplify[Sqrt[x^2], x > 0]` is `x`, and `x < 0` makes it `-x`. The assumptions understood are the inequalities against nothing, singly, in a list, or joined with `&&` |
| `Assuming[x > 0, body]` | the same assumption handed to every `Simplify`, `Refine` and `PowerExpand` inside the body |
| `Element[x, Reals]`, `Element[n, Integers]` | what a letter is, rather than which side of nothing: a real number is its own conjugate and has nothing imaginary about it, and a whole number leaves `Floor` and `Round` where they were and makes `Sin[n Pi]` nothing and `Cos[n Pi]` `(-1)^n` |
| `Cancel[e]` | a fraction with whatever divides both halves taken off |
| `TrigReduce[e]` | products and powers of waves as a sum of plain waves: `Sin[x] Cos[x]` is `Sin[2 x]/2` |
| `TrigExpand[e]` | the other way: `Sin[x + y]` is `Sin[x] Cos[y] + Cos[x] Sin[y]`, and `Sin[2 x]` is `2 Sin[x] Cos[x]` |
| `TrigToExp[e]`, `ExpToTrig[e]` | waves as exponentials of an imaginary angle, and back |
| `PowerExpand[e]` | `Log[a b]` as `Log[a] + Log[b]`, taking the letters for positive numbers |
| `Solve[lhs == rhs, x]` | every root of a polynomial, complex ones included |
| `Solve[a x^3 + b x^2 + c x + d == 0, x]` | a cubic with letters for coefficients, by Cardano: three roots, written with the cube roots of one |
| `Solve[eq, x, Modulus -> n]` | the equation where the arithmetic wraps round: `Solve[3 x == 1, x, Modulus -> 7]` is `x -> 5` |
| `Reduce[eq, x]` | the whole answer with its conditions: `a x + b == 0` is `a != 0 && x == -(b/a) \|\| a == 0 && b == 0`. An inequality gives the stretches of the line where it holds — `Reduce[x^2 - 4 > 0, x]` is `x < -2 \|\| x > 2` |
| `Solve[Sin[x] == 1/2, x]` | a function of the unknown that has an inverse is turned round instead of scanned: `{{x -> Pi/6}, {x -> 5 Pi/6}}`. The waves repeat for ever, and these are the two the principal inverse gives; the rest are these plus whole turns |
| `Roots[lhs == rhs, x]` | the same, written `x == a || x == b` |
| `Variables[e]` | the letters in an expression |
| `PolynomialQ[e, x]` | whether it is a polynomial in x |
| `FindRoot[f, {x, x0}]`, `fzero` | one root, from a starting point |
| `Sum[f, {i, a, b}]`, `Product[…]` | a sum to a named end gets its closed form |
| `Sum[f, {i, a, Infinity}]` | a sum with no end: exact for a geometric series and for 1/n^2, 1/n^4, 1/n^6, added up when the terms die away fast, and left alone when they do not |
| `Solve[{eqs}, {vars}]` | a system, when the equations are linear |
| `Solve[a x + b == 0, x]` | with letters for coefficients: a line and a quadratic have a closed form |
| `FindMinimum[f, {x, x0}]` | the bottom of a curve, from a starting point; `FindMaximum` the top |
| `Minimize[f, x]`, `Maximize[f, x]` | the lowest or highest the function goes anywhere, exactly where it can: `Minimize[x^2 - x, x]` is `{-1/4, {x -> 1/2}}`, and a function that never turns round answers `-Infinity` |
| `NMinimize[f, x]`, `NMaximize[f, x]` | the same, numerically from the start |
| `ArgMin[f, x]`, `ArgMax[f, x]` | the place alone; over a list, which one of them it is |
| `fminbnd(f, a, b)` | the place alone, MATLAB's way |
| `Coefficient`, `CoefficientList`, `Exponent`, `Collect` | a polynomial taken apart |
| `Together`, `Apart`, `Cancel` | the two ways of writing a rational function |
| `PolynomialQuotient`, `PolynomialRemainder`, `PolynomialGCD` | division, with a remainder |

```
In[26]:= Solve[u^3 - u == 0, u]        Out[26]= {{u -> -1}, {u -> 0}, {u -> 1}}
In[27]:= Factor[x^3 - 6x^2 + 11x - 6]  Out[27]= (x - 1) (x - 2) (x - 3)
In[28]:= Solve[{x + y == 3, x - y == 1}, {x, y}]   Out[28]= {{x -> 2, y -> 1}}
In[28b]:= Solve[a x + b == 0, x]       Out[28b]= {{x -> -(b/a)}}
In[28c]:= Solve[x^2 == a, x]           Out[28c]= {{x -> -Sqrt[a]}, {x -> Sqrt[a]}}
In[29]:= Sum[i^2, {i, 1, n}]           Out[29]= n/6 + n^2/2 + n^3/3
In[30]:= Apart[(x + 3)/(x^2 + 3x + 2), x]          Out[30]= -1/(x + 2) + 2/(1 + x)
In[31]:= FindMinimum[(x - 3)^2 + 1, {x, 0}]        Out[31]= {1, {x -> 3}}
In[31b]:= Minimize[x^2 - x, x]         Out[31b]= {-1/4, {x -> 1/2}}
In[31c]:= Maximize[Sin[x], x]          Out[31c]= {1, {x -> 1.5707963267949}}
In[32]:= Sum[1/2^n, {n, 0, Infinity}]  Out[32]= 2
In[33]:= Sum[1/n^2, {n, 1, Infinity}]  Out[33]= Pi^2/6
In[34]:= Limit[(1 + 1/n)^n, n -> Infinity]         Out[34]= E
```

### Several variables

`D` differentiates with respect to whichever name it is given, so
partial derivatives need nothing new; the rest of a second course is
built on that.

| | |
|---|---|
| `Grad[f, {x, y}]` | the vector of first derivatives |
| `Div[{p, q}, {x, y}]` | the divergence of a field |
| `Curl[{p, q, r}, {x, y, z}]` | the curl, in three dimensions |
| `Laplacian[f, {x, y}]` | the sum of the second derivatives |
| `Hessian[f, {x, y}]` | the matrix of second derivatives |
| `Jacobian[{f, g}, {x, y}]` | the matrix of first derivatives of a map |
| `Integrate[f, {x, a, b}, {y, c, d}]` | one variable at a time, inner first |

```
In[i]:= Curl[{y, -x, 0}, {x, y, z}]        Out[i]= {0, 0, -2}
In[j]:= Integrate[x y^2, {x, 0, 1}, {y, 0, 3}]  Out[j]= 9/2
```

### Transforms

`ZTransform[f, n, z]` takes a sequence to its one sided Z transform,
from the table a course hands out — `1`, `n`, `n^2`, `a^n`, `n a^n`,
`Sin[b n]`, `Cos[b n]`, and sums and multiples of those — and
`InverseZTransform[X, z, n]` brings it back by splitting `X(z)/z` into
partial fractions, which handles a repeated pole as well as a simple
one.

`FourierTransform[f, t, w]` and `InverseFourierTransform` read a table
of their own, in Mathematica's convention — `F(w) = 1/Sqrt[2 Pi]` times
the integral, which is where the `Sqrt[2 Pi]` in every answer comes
from. `Exp[-a Abs[t]]`, `Exp[-a t^2]`, `1/(a^2 + t^2)`, `Sin[a t]`,
`Cos[a t]` and `DiracDelta` are what is in it; the last three answer
with `DiracDelta`, which math42 carries about as a name and does not
try to work out.

```
In[f1]:= FourierTransform[Exp[-3 Abs[t]], t, w]  Out[f1]= 3 Sqrt[2/Pi]/(9 + w^2)
In[f2]:= FourierTransform[Exp[-2 t^2], t, w]     Out[f2]= Exp[-(w^2/8)]/2
In[f3]:= FourierTransform[Cos[3 t], t, w]
         Out[f3]= Sqrt[Pi/2] (DiracDelta[w - 3] + DiracDelta[w + 3])
```

```
In[z1]:= ZTransform[n 2^n, n, z]              Out[z1]= 2 z/(z - 2)^2
In[z2]:= InverseZTransform[z/(z^2 - 3z + 2), z, n]   Out[z2]= -1 + 2^n
In[z3]:= InverseZTransform[z/(z - 1)^2, z, n] Out[z3]= n
```

| | |
|---|---|
| `LaplaceTransform[f, t, s]` | by the table: powers, exponentials, sines, with the shift |
| `InverseLaplaceTransform[F, s, t]` | back again, by partial fractions |
| `FourierSeries[f, {x, a, b}, n]` | the series to n terms, as an expression |
| `FourierCoefficient[f, {x, a, b}, n]` | a0 and the pairs {an, bn} |
| `Fourier`, `InverseFourier` | the discrete transform, Mathematica's convention |
| `fft`, `ifft` | the same, MATLAB's convention |

```
In[k]:= LaplaceTransform[t Exp[3t], t, s]     Out[k]= 1/(s - 3)^2
In[l]:= InverseLaplaceTransform[s/(s^2+4), s, t]  Out[l]= Cos[2 t]
In[m]:= FourierSeries[x, {x, -Pi, Pi}, 4]
Out[m]= 2 Sin[x] - Sin[2 x] + 2 Sin[3 x]/3 - Sin[4 x]/2
In[n]:= fft({1, 2, 3, 4})                     Out[n]= {10, -2 + 2 I, -2, -2 - 2 I}
```

### Matrices

| | |
|---|---|
| `Det`, `Inverse`, `Transpose` (`A'`) | the usual three; `Transpose` turns any list of lists, names and strings included |
| `ConjugateTranspose`/`ctranspose` | rows for columns with every number conjugated |
| `Dot[a, b]`, `a . b` | matrix multiplication |
| `LinearSolve[a, b]`, `linsolve`, `a \ b` | solves a x = b |
| `Eigenvalues`, `Eigenvectors`, `Eigensystem` | Jacobi for a symmetric matrix, QR otherwise. The vectors are what each eigenvalue sends to nothing, for any matrix whose eigenvalues are real; where there are fewer vectors than values, the gap is filled with a vector of nothing, as Mathematica fills it |
| `MatrixPower`, `Rank`/`MatrixRank`, `Tr`, `Norm`, `Cross` | |
| `Diagonal[m]`, `Diagonal[m, k]` | the diagonal, and the one k steps off it |
| `CholeskyDecomposition[m]`, `chol` | the upper R with `R'.R` the matrix, for a positive definite one |
| `sqrtm(m)` | the matrix whose square it is, by Denman and Beavers |
| `squeeze`, `vertcat`, `horzcat`, `cat(dim, a, b)` | MATLAB's ways of stacking and unstacking |
| `IdentityMatrix`/`eye`, `zeros`, `ones` | |
| `DiagonalMatrix`/`diag`, `ArrayReshape`/`reshape` | |
| `Dimensions`/`size`, `size(A, 1)` | |
| `RowReduce`/`rref` | the reduced row echelon form |
| `NullSpace`/`null` | a basis for what the matrix sends to nothing |
| `Orthogonalize`/`orth` | Gram–Schmidt: the same space, orthonormal |
| `LeastSquares` | the closest answer when there is no exact one |
| `CharacteristicPolynomial[m, x]` | det(A − x I) |
| `MatrixExp`/`expm` | the exponential of a matrix |
| `Projection[u, v]` | the part of one vector along another |
| `LUDecomposition`/`lu`, `QRDecomposition`/`qr` | the two standard factorings |
| `SingularValueDecomposition`, `SingularValueList`/`svd` | and the values alone |
| `PseudoInverse`/`pinv`, `Cond`/`cond` | when there is no inverse, and how ill-conditioned |
| `Eigensystem` | the eigenvalues and their vectors together |

```
In[28]:= LinearSolve[[2 1; 1 3], {3, 5}]   Out[28]= {0.8, 1.4}
In[29]:= Eigenvalues[[4 1; 2 3]]           Out[29]= {5, 2}
In[30]:= MatrixPower[[1 1; 0 1], 5]        Out[30]= {{1, 5}, {0, 1}}
```

A matrix with complex eigenvalues gives them: `Eigenvalues[[0 -1; 1 0]]`
is `{I, -I}`.

### Lists

`Length`, `Total`/`sum`, `Mean`, `Median`, `Variance`,
`StandardDeviation`, `Max`, `Min`, `Sort`, `Reverse`, `Flatten`,
`Join`, `First`, `Last`, `Rest`, `Append`, `Take`, `Drop`,
`Partition`, `Count`, `Position`/`find`, `Tally`, `Counts`, `Ratios`,
`Thread`, `Inner[f, u, v, g]`, `Union`,
`Intersection`, `Complement`, `Accumulate`/`cumsum`,
`Differences`/`diff`, `AnyTrue`/`any`, `AllTrue`/`all`, `repmat`,
`fliplr`, `flipud`, `cumprod`, `logspace`, `RandomChoice`,
`RandomSample`.

`Total` of a matrix is the vector of its column sums, as both languages
mean it; `sum(A, 2)` adds along the rows instead, and `mean`, `max`,
`min` and `prod` take a dimension the same way.

### Statistics and chance

`Mean`, `Median`, `Variance`, `StandardDeviation`, `Quantile`, `Mode`,
`Skewness`, `Kurtosis`, `RootMeanSquare`, `Correlation`/`corrcoef` and
`Covariance`/`cov`.

Written the Mathematica way, of exact numbers, `Mean`, `Median` and
`Variance` answer exactly; `mean`, `median` and `var` are numeric, as
everything in MATLAB's spelling is. Of a matrix they all work down the
columns, which is what both languages mean by it.

```
In[t0]:= Mean[{1, 2, 3, 4}]                     Out[t0]= 5/2
In[t0b]:= mean({1, 2, 3, 4})                    Out[t0b]= 2.5
In[t0c]:= Variance[{1, 2, 3, 4}]                Out[t0c]= 5/3
In[t0d]:= Mean[[1 2; 3 4]]                      Out[t0d]= {2, 3}
```

A distribution is written the Mathematica way and read by `PDF`,
`CDF`, `RandomVariate`, `Mean` and `Variance`:

```
In[t1]:= CDF[NormalDistribution[0, 1], 1.96]    Out[t1]= 0.97500210485178
In[t2]:= Mean[NormalDistribution[3, 2]]         Out[t2]= 3
In[t3]:= RandomVariate[PoissonDistribution[4], 5]
```

`NormalDistribution`, `UniformDistribution`, `ExponentialDistribution`,
`PoissonDistribution` and `BinomialDistribution` are the five it knows;
`normpdf`, `normcdf` and `norminv` are MATLAB's names for the first.

From a list rather than a function: `trapz`, `cumtrapz`, `gradient`,
and `polyder` and `polyint` for a polynomial kept as coefficients.

### Applying and repeating

`Map`/`arrayfun`, `Select`, `Fold`, `FoldList`, `Nest`, `NestList`,
`NestWhile`, `NestWhileList`, `FixedPoint`, `FixedPointList`, `Apply`,
`Through`, `Gather`, `GatherBy`, `SplitBy` and `GroupBy` — each of
which takes either a pure function or the name of one, and `cellfun`
is MATLAB's other spelling of `Map`:

```
In[31]:= Select[Range[10], PrimeQ]      Out[31]= {2, 3, 5, 7}
In[32]:= Nest[# + 1 &, 0, 5]            Out[32]= 5
In[33]:= Fold[Times, 1, Range[5]]       Out[33]= 120
In[34]:= FoldList[Plus, 0, {1, 2, 3}]   Out[34]= {0, 1, 3, 6}
In[35]:= FixedPoint[Cos, 1.0]           Out[35]= 0.739085133215161
In[36]:= NestWhile[#/2 &, 100, # > 1 &] Out[36]= 25/32
In[37]:= GroupBy[{1, 2, 3, 4}, EvenQ]   Out[37]= {0 -> {1, 3}, 1 -> {2, 4}}
In[38]:= Through[{Sin, Cos}, 0]         Out[38]= {0, 1}
```

`GroupBy` hands back a list of `key -> the ones with it` rather than an
association, which math42 does not have. `Through` is written with the
functions and the argument side by side: Mathematica's
`Through[{Sin, Cos}[0]]` cannot be written here, because `[0]` after a
list is a MATLAB matrix and the two spellings collide.

### Numbers and whole numbers

`N`, `Rationalize`, `Numerator`, `Denominator`, `Abs`, `Sign`,
`Round` (also `Round[x, dx]`), `Floor`, `Ceiling`, `IntegerPart`/`fix`,
`FractionalPart`, `Mod`, `rem`, `Quotient`, `GCD`, `LCM`, `Binomial`,
`Factorial` (also `n!`), `Gamma`, `LogGamma`/`gammaln`, `PrimeQ`,
`Prime`, `Fibonacci`, `RandomReal`/`rand`, `RandomInteger`/`randi`,
`randn`, `Chop`.

The functions an integral runs into are here too, since
`Integrate[Exp[x^2], x]` has no elementary answer and something has to
be said: `Erfi` (the error function of an imaginary argument, made
real), `SinIntegral` and `CosIntegral`, `ExpIntegralEi`,
`LogIntegral`, `FresnelS` and `FresnelC`, and the Airy pair `AiryAi`
and `AiryBi` with their derivatives `AiryAiPrime` and `AiryBiPrime`.
Each is a number to whatever a double holds, and each is what
something differentiates back into:

```
In[i1]:= Integrate[Exp[x^2], x]    Out[i1]= Sqrt[Pi] Erfi[x]/2
In[i2]:= Integrate[Sin[x]/x, x]    Out[i2]= SinIntegral[x]
In[i3]:= Integrate[Sin[x^2], x]    Out[i3]= Sqrt[Pi/2] FresnelS[Sqrt[2/Pi] x]
In[i4]:= Integrate[Exp[x]/x, x]    Out[i4]= ExpIntegralEi[x]
In[i5]:= D[AiryAi[x], x, x]        Out[i5]= x AiryAi[x]
```

The functions of one number are `Sin`, `Cos`, `Tan`, `Cot`, `Sec`,
`Csc`, `ArcSin`, `ArcCos`, `ArcTan`, `Sinh`, `Cosh`, `Tanh`, `Exp`,
`Log`, `Log10`, `Log2`, `Sqrt` and `Abs`, each of them under its MATLAB
spelling too, and the ones a physics course wants: `Erf`, `Erfc`,
`InverseErf`/`erfinv`, `InverseErfc`/`erfcinv`, `Zeta`. With an order
and an argument there are `BesselJ`, `BesselY`, `BesselI`, `BesselK`
(all four under `besselj` and its kin), and the orthogonal polynomials
`LegendreP`, `ChebyshevT`, `ChebyshevU`, `HermiteH`, `LaguerreL`.

### Discrete mathematics

**Whole numbers.** `PowerMod`, `ExtendedGCD`, `ModularInverse`,
`ChineseRemainder`, `FactorInteger`, `Divisors`, `EulerPhi`,
`MoebiusMu`, `JacobiSymbol`, `NextPrime`, `PrimePi` — with `GCD`,
`LCM`, `Mod` and `PrimeQ` from the numbers section.

`HarmonicNumber[n]` is 1 + 1/2 + ... + 1/n and comes out exactly
(`HarmonicNumber[5]` is `137/60`); `PartitionsP[n]` counts the ways n
is a sum of whole numbers; `PolyGamma[x]` and `PolyGamma[n, x]` are the
derivatives of `Log[Gamma[x]]`; and `ContinuedFraction[Pi, 5]` is
`{3, 7, 15, 1, 292}`, which `FromContinuedFraction` turns back into
`103993/33102`.

**Counting.** `Subsets` (all of them, or those of one size),
`Permutations` in dictionary order, `Binomial`, `Multinomial`,
`CatalanNumber`, `StirlingS2`, `BellB`.

**Logic.** `And`, `Or`, `Not` as `&&`, `||` and `!`, with `Xor`,
`Implies` and `Nand`.

**Graphs**, held as the matrix of what joins what: `VertexDegrees`,
`ConnectedComponents`, `TransitiveClosure`, `GraphDistance` (Dijkstra,
using the numbers in the matrix as lengths), and `MatrixPower` for the
number of walks of a given length.

```
In[o]:= PowerMod[7, 100, 13]                Out[o]= 9
In[p]:= ExtendedGCD[240, 46]                Out[p]= {2, {-9, 47}}
In[q]:= FactorInteger[360]                  Out[q]= {{2, 3}, {3, 2}, {5, 1}}
In[r]:= StirlingS2[4, 2]                    Out[r]= 7
In[s]:= ConnectedComponents[[0 1 0; 1 0 0; 0 0 0]]   Out[s]= {{1, 2}, {3}}
```

### Asking what something is

`Head`, `NumberQ`, `IntegerQ`, `ListQ`, `MatrixQ`, `StringQ`, `EvenQ`,
`OddQ`, `SameQ`/`isequal`, `isempty`, `ndims`.

### Polynomials, the MATLAB way

A polynomial is a list of coefficients, highest power first.

| | |
|---|---|
| `polyval(p, x)` | the polynomial at x |
| `polyfit(x, y, n)` | least squares through the points |
| `Fit[data, {1, x, x^2}, x]` | the combination of those functions closest to the points, by least squares |
| `FindFit[data, model, {a, b}, x]` | the parameters of any model at all that fit the points best, by Nelder and Mead's simplex: `FindFit[data, a Exp[b x], {a, b}, x]` |
| `Interpolation[data]` | a function of one argument, drawing a straight line between the points |
| `roots(p)` | every root, complex ones included |
| `conv(a, b)` | two polynomials multiplied |
| `interp1(x, y, at)` | a straight line between the two nearest points |

### Text

`StringLength`/`strlength`, `StringJoin`/`strcat`, `StringTake`,
`StringDrop`, `StringReverse`, `StringSplit`/`strsplit`,
`StringRiffle`/`strjoin`, `StringTrim`/`strtrim`,
`StringPosition`/`strfind`, `StringCount`, `StringContainsQ`/`contains`,
`StringStartsQ`, `StringEndsQ`, `StringPadLeft`, `StringPadRight`,
`Characters`, `ToUpperCase`/`upper`, `ToLowerCase`/`lower`,
`ToString`/`num2str`, `ToExpression`/`str2num`, `strcmp`, and
`sprintf`/`fprintf` with MATLAB's formats.

`StringReplace` takes either spelling — `StringReplace[s, "a" -> "b"]`
with one rule or a list of them, or MATLAB's
`strrep(s, "a", "b")` — and `IntegerString[255, 16]` and
`FromDigits["ff", 16]` change base either way.

`StringMatchQ[s, "a*"]` asks whether the whole of the text matches a
pattern, with `*` for anything and `?` for one letter, and
`StringCases[s, p]` gives every piece of it that does. Where a real
regular expression is wanted there is MATLAB's
`regexprep(s, pattern, to)`.

```
In[s1]:= StringReplace["hello", "l" -> "L"]        Out[s1]= "heLLo"
In[s2]:= StringRiffle[{"a", "b", "c"}, "-"]        Out[s2]= "a-b-c"
In[s3]:= StringCount["banana", "an"]               Out[s3]= 2
In[s4a]:= StringCases["banana", "an"]              Out[s4a]= {"an", "an"}
In[s4b]:= regexprep("2026-08-29", "-", "/")        Out[s4b]= "2026/08/29"
In[s4]:= IntegerString[255, 16]                    Out[s4]= "ff"
```

### Graphs

| | |
|---|---|
| `Plot[f, {x, a, b}]` | one curve, or several from a list of functions |
| `Plot3D[f, {x, a, b}, {y, c, d}]`, `surf` | a surface, drawn in projection |
| `ParametricPlot[{x, y}, {t, a, b}]` | the curve a moving point traces |
| `PolarPlot[r, {t, a, b}]` | in polar coordinates |
| `ListPlot`/`scatter`, `ListLinePlot`, `plot` | points, or points joined up |
| `BarChart`/`bar`, `Histogram`/`hist` | bars, and data in bins |
| `ContourPlot`/`contour` | the curves along which a function keeps its value |
| `ParametricPlot3D[{x, y, z}, {t, a, b}]` | a curve through space |
| `VectorPlot[{p, q}, {x, a, b}, {y, c, d}]`, `quiver` | which way a field points at each place — the direction field of `y' = f(x, y)` is `VectorPlot[{1, f}, …]` |
| `StreamPlot[{p, q}, {x, a, b}, {y, c, d}]` | the same field drawn as the lines a speck of dust would follow through it |
| `RegionPlot[cond, {x, a, b}, {y, c, d}]` | the part of the plane where a condition holds, shaded: `RegionPlot[y > x^2 && y < x + 2, …]` |
| `DensityPlot[f, {x, a, b}, {y, c, d}]` | the same surface looked straight down on, painted by height |
| `ListPlot3D[grid]`, `ListContourPlot[grid]`, `ListDensityPlot[grid]` | the same three from a grid of heights rather than from a function |
| `LogPlot`/`semilogy`, `LogLogPlot`/`loglog` | an axis in powers of ten |
| `StemPlot`/`stem`, `StairsPlot`/`stairs` | two more ways of drawing a list |
| `Show[p, q]` | graphs laid over one another |

Each takes options after its arguments:

```
Plot[Sin[x]/x, {x, -20, 20},
     PlotLabel -> "the sinc function",
     AxesLabel -> {"x", "y"},
     PlotRange -> {-0.5, 1.2}]
```

A solved differential equation is a list of points, so it draws with
`ListLinePlot`:

```
ListLinePlot[NDSolve[y - x^2, {x, 0, 4}, 1], PlotLabel -> "y' = y - x^2"]
```

---

## 4. Course by course

What follows is the mathematics of an NTNU engineering degree, and
where math42 meets it. Everything named here works; what it cannot do
is in [section 7](#7-what-math42-does-not-do).

### Matte 1 — one variable

| the course | in math42 |
|---|---|
| limits | `Limit[f, x -> a]`, also at `Infinity` |
| derivatives | `D[f, x]`, `D[f, {x, n}]` |
| integrals | `Integrate`, by the rules listed above |
| definite integrals | `Integrate[f, {x, a, b}]`, exact where it can be |
| Taylor series | `Series[f, {x, a, n}]` |
| sequences and sums | `Sum`, `Product`, `Table` |
| Newton's method | `FindRoot`, `fzero` |
| separable and linear first-order equations | `DSolve` for the constant-coefficient ones, `NDSolve` for the rest |

### Matte 2 — several variables

| the course | in math42 |
|---|---|
| partial derivatives | `D[f, x]`, `D[f, y]` |
| gradient, divergence, curl | `Grad`, `Div`, `Curl` |
| the Laplacian | `Laplacian` |
| second-derivative test | `Hessian`, then `Eigenvalues` of it |
| change of variables | `Jacobian` |
| double and triple integrals | `Integrate[f, {x, a, b}, {y, c, d}]` |
| surfaces | `Plot3D`, `ParametricPlot` |

### Matte 3 — linear algebra

| the course | in math42 |
|---|---|
| Gaussian elimination | `RowReduce`, `LinearSolve`, `A \ b` |
| determinants and inverses | `Det`, `Inverse` |
| the four subspaces | `NullSpace`, `Rank`, `Transpose` |
| eigenvalues and eigenvectors | `Eigenvalues`, `Eigenvectors` |
| the characteristic polynomial | `CharacteristicPolynomial` |
| orthogonality and projection | `Orthogonalize`, `Projection`, `Norm` |
| least squares | `LeastSquares`, `polyfit` |
| systems of differential equations | `MatrixExp` |
| complex numbers | `I`, `Abs`, `Arg`, `Conjugate`, `Solve` |

### Matte 4 — transforms and equations

| the course | in math42 |
|---|---|
| the Laplace transform | `LaplaceTransform`, `InverseLaplaceTransform` |
| solving equations with it | transform, solve for the image, transform back |
| Fourier series | `FourierSeries`, `FourierCoefficient` |
| the discrete transform | `Fourier`, `fft` |
| second-order equations | `DSolve`, including initial values |
| numerical solutions | `NDSolve`, `ode45` |
| numerical integration | `NIntegrate`, `quad` |

An equation solved through the Laplace transform, the way it is done on
paper — transform, solve for the image, and transform back:

```
In[t]:= LaplaceTransform[Sin[2t], t, s]      Out[t]= 2/(s^2 + 4)
In[u]:= InverseLaplaceTransform[2/((s^2+4)(s+1)), s, t]
```

### Diskret matematikk

| the course | in math42 |
|---|---|
| modular arithmetic | `Mod`, `PowerMod`, `ModularInverse` |
| the Euclidean algorithm | `GCD`, `ExtendedGCD` |
| the Chinese remainder theorem | `ChineseRemainder` |
| primes and factorisation | `PrimeQ`, `FactorInteger`, `Divisors`, `EulerPhi` |
| counting | `Binomial`, `Multinomial`, `Subsets`, `Permutations` |
| set partitions | `StirlingS2`, `BellB`, `CatalanNumber` |
| logic | `&&`, `\|\|`, `!`, `Xor`, `Implies`, `Nand` |
| sets | `Union`, `Intersection`, `Complement`, `Subsets` |
| relations | a matrix, with `TransitiveClosure` |
| graphs | `VertexDegrees`, `ConnectedComponents`, `GraphDistance` |
| recurrences | `RSolve` |

## 5. Coming from Mathematica

Nearly everything you would type in a first session works. What is
there:

- `Sin[x]` bracket syntax throughout, and the capitalised names.
- Exact numbers: `1/3`, `Sqrt[2]`, `Sin[Pi/3]`, `Pi` and `E` symbolic.
- Symbols, `expr /. x -> 2`, `f[x_] := …`, pure functions `#^2 &`.
- `/@`, `@@`, `@`, `//`, `%`, `;`, `(* comments *)`.
- `Part` in full: `[[i]]`, `[[-1]]`, `[[2 ;; 4]]`, `[[{1,3}]]`, `All`.
- `D`, `Integrate`, `Sum`, `Product`, `Limit`, `Series`, `Solve`,
  `Expand`, `Factor`, `Simplify`, `Table`, `Map`, `Select`, `Fold`,
  `Nest`, `Module`, `If`, `Which`, `For`, `While`, `Do`, `Print`,
  `Names`, `Information` (`?Sin`), `Timing`.
- Output set as mathematics, and `MatrixForm` for a matrix.

What is different:

| | |
|---|---|
| `Sin[2]` | a decimal, not `Sin[2]` — only constants and the exact angles stay exact |
| `2^100` | a decimal: exact arithmetic is 64 bits wide |
| `Solve` | numeric roots, found by iteration, not closed forms |
| `Integrate` | the rules of a first course; anything else is kept as written or done numerically |
| patterns | `f[x_]` names an argument; there is no pattern matching beyond that |
| `Simplify` | knows the identities that need no assumptions — `Sin[x]^2 + Cos[x]^2` is 1 — but cannot be told that a letter is positive or real, so it will not prove the ones that need it |

## 6. Coming from MATLAB

What is there:

- `sin(x)` round-bracket calls, and the lower-case names, always numeric.
- `[1 2; 3 4]` matrices, `A(2, :)`, `A(i, j)`, `x(end)`, `x(2:4)`.
- `a:b`, `a:step:b`, `linspace`, `logspace`, `zeros`, `ones`, `eye`.
- `*`, `.*`, `.^`, `'`, `\`, `.` — with `*` element-wise and `.` the
  matrix product, as Mathematica has it.
- `for … end`, `while … end`, `if … elseif … else … end`.
- `@(x) …` anonymous functions, `arrayfun`.
- `sprintf`, `fprintf`, `disp`, `num2str`, `str2num`, `strcat`,
  `strrep`, `strsplit`, `strcmp`.
- `det`, `inv`, `rank`, `trace`, `eig`, `norm`, `cross`, `reshape`,
  `diag`, `size`, `numel`, `length`, `sum`, `mean`, `std`, `var`,
  `median`, `sort`, `cumsum`, `cumprod`, `diff`, `find`, `any`, `all`,
  `fliplr`, `flipud`, `rot90`, `repmat`, `sortrows`, `triu`, `tril`,
  `kron`, `nnz`, `prod`, `polyval`, `polyfit`, `roots`, `conv`,
  `interp1`, `fzero`, `fminbnd`, `quad`, `ode45`, `rand`, `randi`,
  `randn`, `hypot`, `nthroot`, `deg2rad`, `rad2deg`, `primes`,
  `strjoin`, `strtrim`, `contains`, `dec2base`, `base2dec`, `subs`,
  `squeeze`, `vertcat`, `horzcat`, `cat`, `cellfun`, `histcounts`,
  `chol`, `sqrtm`, `linsolve`, `regexprep`, `erf`, `erfc`, `erfinv`,
  `gammaln`, `zeta`, `besselj`, `log2`, `log10`, `sec`, `csc`, `cot`.
- `plot`, `bar`, `hist`, `scatter`, `surf`, `fplot`, `polarplot`,
  `contour`, `semilogy`, `loglog`, `stem`, `stairs`.
- `v(2) = 9` to change one place, growing the list with zeros when the
  place is past the end; `clear` to forget everything.

What is different:

| | |
|---|---|
| `*` | element-wise; write `.` or `Dot` for the matrix product |
| indices | start at 1 as they do in MATLAB, but a list is a list, not an array with a shape |
| `function` files | not there; write `f(x) = …` or `f = @(x) …` |
| a space before `(` | means multiplication, as it does in Mathematica: `2 x (x + 1)` is a product. Write a call with no space — `f(3)`, `v(2)` — unless the name is one math42 knows, where `Sin (x)` is still `Sin[x]` |
| several return values | `[q, r] = size(A)` hands out the pieces of the list `size(A)` returns, and `{q, r} = …` does the same the Mathematica way; a function still returns one thing, which is that list |
| `end` in a block | needed after `for`, `while` and `if`, as usual — and a one-line block needs a comma after its header |
| strings | double quotes only |

## 7. What math42 does not do

Said plainly, so nothing surprises you:

- **Arbitrary precision beyond whole numbers.** A whole number grows as
  large as it needs to — `2^100`, `100!` and `Fibonacci[200]` come out
  to the last digit — but the *denominator* of an exact fraction is
  still 64 bits wide, and a decimal is always the double the machine
  gives, never fifty places of it.
- **Patterns, at the edges.** `_`, `x_`, `_Head`, `__`, `___`, `/;`,
  `?f` and `|` all work, in rules and in definitions. What is missing
  is the rest of Mathematica's vocabulary: no `Repeated`, no `Optional`
  or default values, no `HoldPattern`, and no named upvalues. Sums and
  products match as trees of two branches rather than flat, so a
  pattern for a sum of three terms has to be written as one. `|` is
  also MATLAB's or, and means one shape or the other only where a
  pattern is what was wanted.
- **Symbolic solving.** `Solve` gives a closed form for a line, a
  quadratic and a cubic with letters for coefficients, turns round a
  function of the unknown that has an inverse, solves an equation to a
  modulus, and finds the rest of the roots numerically — a quartic
  with letters in it is beyond it. `Reduce` gives the whole answer
  with its conditions for an equation in one letter, and the stretches
  of the line for an inequality with numbers for coefficients; a
  system of them, or one in several letters, is beyond it, and so is
  an inequality whose coefficients are letters. `DSolve` handles the
  linear equations: a first order one by its integrating factor
  whatever its coefficients are, a second order one with constant
  coefficients and anything on the right it can integrate twice, and a
  first order one that separates into a power of `y`, and a system of
  two first order ones with numbers for coefficients. `RSolve` handles
  a linear recurrence with constant coefficients. There are no partial
  differential equations, and a system of more than two, or one whose
  coefficients are letters, is beyond it.
- **Symbolic integration beyond the rules listed above.** The special
  functions are numeric, but the ones an integral runs into are
  integrated and differentiated as well: `Exp[x^2]` gives `Erfi`,
  `Sin[x]/x` gives `SinIntegral`, `Cos[x]/x` `CosIntegral`, `Exp[x]/x`
  `ExpIntegralEi`, `1/Log[x]` `LogIntegral`, and `Sin[x^2]` and
  `Cos[x^2]` the two of Fresnel, and the Airy pair solves `y'' == x y`.
  What is missing is everything else: an integral outside the rules
  listed above is left standing rather than answered with a function
  nobody has heard of, and there is no Meijer G to fall back on.
- **Symbolic transforms beyond the rules.** The Laplace transform
  works from a small table with the rules of the trade on top of it —
  a constant multiple, a sum, the shift `L[Exp[a t] f] = F(s - a)` and
  the power `L[t^n f] = (-1)^n F^(n)(s)`, applied together, so that
  `t^2 Exp[t] Sin[t]` goes through though no table lists it. What is
  not there is the transform of a derivative of an unknown function,
  which is what would let `DSolve` be done by transform, and the
  convolution rule. The Fourier series is worked out symbolically
  wherever the coefficient integral has a closed form —
  `FourierSeries[x^2, {x, -Pi, Pi}, 3]` is `Pi^2/3 - 4 Cos[x] +
  Cos[2 x] - 4 Cos[3 x]/9` — and falls back on quadrature where it has
  none, as it must for `Abs[x]`.
- **`function` files**, and a function that returns several things.
  `{a, b} = {1, 2}` and `[q, r] = size(A)` hand out the pieces of one
  list, which is as near as it comes.
- **Units, dates, images, sound, networks** — none of that.

Everything it does do, it does honestly: if math42 cannot work
something out, it says so or hands the expression back as you wrote
it. It never guesses.

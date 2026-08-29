Math42 ("math42", one word, is the project and binary name) is a mathematics
notebook with GPL license, built on the same principles as office42: a small,
honest C codebase on GTK 4, Pango and Cairo, in the shape of Mathematica and
MATLAB.

Layout:
- `src/core/` — the language: values, lexer, parser, evaluator. Links
  against GLib and libm only. It must never include GTK.
- `src/ui/` — the GTK application, window and the notebook canvas, which
  paints In[n]/Out[n] cells with Pango and Cairo.
- `src/calc-main.c` — `math42-calc`, the engine from a terminal; CI's
  smoke test runs it, so keep it working.
- `src/core/m42-help.c` — the table of every function with a line about
  it. Adding a function to the evaluator means adding it here too: the
  `?Sin` answer, `Names[]` and the window's F1 reference all read this
  table, so a missing row is a function nobody can find. Worse, a name
  missing from the table and called the MATLAB way with one argument is
  read as a multiplication instead: `zeros(3)` came out as `3 zeros`,
  and went on doing so through several rounds of work because nothing
  said so out loud. To find any that have slipped through, pull every
  name out of the evaluator and look for the ones the table lacks:

  ```sh
  python - <<'EOF'
  import io, re
  ev = io.open('src/core/m42-eval.c', encoding='utf-8').read()
  hp = io.open('src/core/m42-help.c', encoding='utf-8').read()
  names = set()
  for m in re.finditer(r'name_is \(name, "([^"]+)", (?:"([^"]+)"|NULL)\)', ev):
      names.add(m.group(1))
      if m.group(2): names.add(m.group(2))
  have = set()
  for a, b in re.findall(r'\{ "([^"]+)", (?:"([^"]+)"|NULL)', hp):
      have.add(a)
      if b: have.add(b)
  print(sorted(n for n in names if n not in have))
  EOF
  ```

  New rows go above the `{ NULL, NULL, NULL, NULL, NULL }` that ends the
  array, not below it.

Instructions for AI agents:
- Do not add any unit tests.
- Exercise changes through the running program or the math42-calc
  terminal front-end, and say in the commit message what you did to check.
- Build with `meson setup builddir && meson compile -C builddir`. On
  Windows use MSYS2 MinGW64 (`C:\msys64\mingw64\bin` on PATH).
- `src/core/m42-format.c` — reading and writing MATLAB `.m`, Wolfram
  `.wl` and Mathematica `.nb` files. A round trip must come back
  unchanged: `math42 --convert out.nb a.m42 && math42 --convert
  back.m42 out.nb && diff a.m42 back.m42`.
- To look at a change without a person at the keyboard:
  `math42 --size 800x900 --screenshot out.png notebook.m42`, adding
  `--activate reference` to picture a dialog, and
  `--export-pdf out.pdf` to check the PDF (pdftoppm renders it).
- Function names are accepted in both spellings: Mathematica's `Sin[x]`
  and MATLAB's `sin(x)`.

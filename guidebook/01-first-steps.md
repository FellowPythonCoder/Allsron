# First steps

SPRFST is a small language. You can read all of it in an afternoon, and
the compiler is quick enough that running a program feels like pressing
a key rather than starting a build.

Here is a whole program.

```sprfst
use std.io

fn main() {
    io.say("hello, world")
}
```

Three things are happening.

- `use std.io` brings in the input and output module. Nothing is
  imported for you behind your back, so you can always tell where a name
  came from.
- `fn main()` declares the function the runner starts with.
- `io.say` writes a line.

Save that as `hello.spf` and run it:

```
sprfst run hello.spf
```

There is no build step to remember. `run` compiles and executes in one
go; `build` is there for when you want the artefacts.

## Making a project

For anything bigger than one file, start a project:

```
sprfst new my-app
cd my-app
sprfst run .
```

That gives you `project.sprfst`, a `src/` folder with `main.spf`, a
`tests/` folder and an `assets/` folder. The dot in `sprfst run .` means
"the project here" — the runner reads the entry from `project.sprfst`.

## Try it

Change the message, then make the program say two lines instead of one.

```sprfst
use std.io

fn main() {
    io.say("my name is ...")
    io.say("and this is my first SPRFST program")
}
```

## Exercise

Write a program that prints your name, then prints a line of dashes
under it. You only need `io.say` and one piece of text.

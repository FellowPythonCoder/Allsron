# The standard library

Everything in this chapter arrives with the language. Nothing has to be
installed, and nothing reaches the network to work.

## The modules

**Core**

| Module | For |
| --- | --- |
| `std.io` | `say`, `print`, `warn`, `ask`, `read_line`, `flush` |
| `std.math` | the usual functions, `pi`, `clamp`, `hypot`, `is_nan` |
| `std.time` | `now`, `clock`, `ms`, `sleep`, `format`, `parts` |
| `std.rand` | `seed`, `int`, `num`, `choice`, `shuffle`, `normal` |
| `std.sys` | `args`, `env`, `platform`, `arch`, `cpu_count`, `run`, `memory`, `exit` |

**Data**

| Module | For |
| --- | --- |
| `std.json` | `parse`, `write`, `pretty` |
| `std.csv` | `parse`, `write` |
| `std.hash` | `sha256`, `hmac_sha256`, `crc32`, `fnv64`, `base64` |
| `std.db` | the Ember database — chapter 19 |
| `std.tensor` | numeric arrays — chapter 23 |

**System**

| Module | For |
| --- | --- |
| `std.fs` | `read`, `write`, `append`, `exists`, `list`, `mkdir`, `size`, `copy` |
| `std.path` | `join`, `dirname`, `basename`, `ext`, `abs` |
| `std.thread` | threads, locks, atomics, channels — chapter 17 |
| `std.task` | cooperative tasks |

**Networking, interface, pictures**

| Module | For |
| --- | --- |
| `std.net` | TCP and UDP sockets |
| `std.http` | `get`, `post`, `serve`, `url_encode` — chapter 22 |
| `std.ui` | windows and controls — chapter 21 |
| `std.draw` | canvases and PNGs — chapter 20 |

**Written in SPRFST, in `std/`**

| Module | For |
| --- | --- |
| `std.core` | small helpers: `between`, `sign`, `blend`, `round_to`, `times`, `human_size`, `human_time` |
| `std.collections` | `zip`, `unique`, `chunks`, `tally`, `group_by`, `span`, `mean` |
| `std.testing` | `same`, `different`, `yes`, `no`, `close`, `holds`, `failed` |
| `std.ai` | layers, networks, tokenising — chapter 23 |

Open any of them: they are ordinary source files you can read.

## A tour in one program

```sprfst
use std.io
use std.core
use std.collections
use std.math
use std.hash
use std.json

fn main() {
    io.say("— core —")
    io.say("between      {core.between(7.0, 1.0, 10.0)}")
    io.say("round_to     {core.round_to(3.14159, 2)}")
    io.say("blend        {core.blend(0.0, 100.0, 0.25)}")
    io.say("human_size   {core.human_size(1536000)}")
    io.say("human_time   {core.human_time(3725.0)}")

    io.say("")
    io.say("— collections —")
    let names = ["ada", "alan", "grace", "edsger", "grace"]
    io.say("unique       {collections.unique(names)}")
    io.say("chunks       {collections.chunks([1, 2, 3, 4, 5], 2)}")
    io.say("zip          {collections.zip([1, 2, 3], ["a", "b", "c"])}")
    io.say("group_by     {collections.group_by(names, fn(n) => n.slice(0, 1))}")
    io.say("mean         {collections.mean([2.0, 4.0, 9.0])}")

    io.say("")
    io.say("— maths and hashing —")
    io.say("hypot        {math.hypot(3.0, 4.0)}")
    io.say("clamp        {math.clamp(42.0, 0.0, 10.0)}")
    io.say("sha256       {hash.sha256("sprfst").slice(0, 16)}…")
    io.say("base64       {hash.base64("sprfst")}")

    io.say("")
    io.say("— json —")
    let record = ["name": "notes", "version": 1, "tags": ["small", "fast"]]
    io.say(json.write(record))
}
```

## Files and paths

```sprfst
use std.io
use std.fs
use std.path

fn main() {
    let file = "tour.txt"
    fs.write(file, "one\ntwo\nthree\n")
    fs.append(file, "four\n")

    let text = fs.read(file) ?? ""
    io.say("{text.trim().lines().len()} lines, {fs.size(file)} bytes")
    io.say("name {path.basename(file)}  extension {path.ext(file)}")
    io.say("joined {path.join("notes", "today.md")}")

    fs.remove(file)
    io.say("gone: {not fs.exists(file)}")
}
```

## Time

```sprfst
use std.io
use std.time

fn main() {
    let now = time.now()
    io.say("formatted {time.format(now, "%Y-%m-%d")}")
    let parts = time.parts(now)
    io.say("year {parts.get("year") ?? 0}, month {parts.get("month") ?? 0}")

    let started = time.clock()
    var total = 0
    for i in 0..50000 { total = total + i }
    io.say("counted to {total} in {(time.clock() - started) * 1000.0} ms")
}
```

`time.now()` is wall clock seconds since 1970 — the one to store.
`time.clock()` is a monotonic clock — the one to measure with.
`time.ms()` is milliseconds since the program started.

## Finding out what exists

Three ways, none of which involve a web page:

```
sprfst docs .            # writes docs/ for your project and the library
```

In Studio, hover any name for its signature and its documentation line.
And from any editor:

```
echo '{"id":1,"op":"natives"}' | sprfst studio
```

gives the whole native table — module, name, signature and summary —
as JSON.

## Exercise

Write a `report` program: read a CSV with `std.csv`, group the rows by
their first column with `collections.group_by`, print a count per group
and the total, and save the summary as JSON next to the input.

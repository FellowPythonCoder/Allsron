# Files and data

## Reading and writing

```sprfst
use std.io
use std.fs
use std.path

fn main() {
    let folder = "/tmp/sprfst-chapter-18"
    fs.mkdir(folder)

    let note = path.join(folder, "note.txt")
    fs.write(note, "first line\n")
    fs.append(note, "second line\n")

    io.say("{fs.size(note)} bytes")
    io.say(fs.read(note) ?? "")
    io.say("exists: {fs.exists(note)}")
    io.say("here: {fs.list(folder)}")

    fs.remove(note)
}
```

`fs.read` gives `Text?` because the file may not be there. The compiler
makes you decide what happens then.

## JSON

```sprfst
use std.io
use std.json

fn main() {
    let record = ["name": "Ada", "age": 36, "tags": ["maths", "engines"]]

    let compact = json.write(record)
    io.say(compact)
    io.say(json.pretty(record))

    let back = json.parse(compact)
    io.say("name is {back?.get("name") ?? "?"}")
}
```

## CSV

```sprfst
use std.io
use std.csv

fn main() {
    let table = "city,people\nhouston,2300000\naustin,960000"
    for row in csv.parse(table) {
        io.say("{row}")
    }

    let rows = [["name", "score"], ["ada", "99"], ["alan", "95"]]
    io.say(csv.write(rows))
}
```

## Paths

```sprfst
use std.io
use std.path

fn main() {
    let p = "/home/user/project/src/main.spf"
    io.say(path.dirname(p))
    io.say(path.basename(p))
    io.say(path.ext(p))
    io.say(path.join("src", "main.spf"))
}
```

## Hashing and encoding

```sprfst
use std.io
use std.hash

fn main() {
    io.say(hash.sha256("hello"))
    io.say("{hash.crc32("hello")}")
    let packed = hash.base64("SPRFST")
    io.say(packed)
    io.say(hash.unbase64(packed) ?? "")
}
```

## Exercise

Write a program that reads a CSV file of names and scores, works out the
average, and writes a JSON summary next to it.

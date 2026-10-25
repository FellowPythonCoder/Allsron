# Text

Text in SPRFST is a sequence of bytes holding UTF-8. The methods you
reach for most are all on the value itself.

```sprfst
use std.io

fn main() {
    let line = "  SPRFST is small and quick  "
    io.say("[{line.trim()}]")
    io.say("{line.trim().upper()}")
    io.say("{line.trim().len()} bytes")
    io.say("{line.contains("quick")}")
    io.say("{line.trim().replace("quick", "fast")}")
}
```

## Splitting and joining

```sprfst
use std.io

fn main() {
    let csv = "ada,grace,alan"
    let names = csv.split(",")
    io.say("{names}")
    io.say("{names.join(" and ")}")
    io.say("first is {names.first() ?? "nobody"}")
}
```

## Slicing and padding

```sprfst
use std.io

fn main() {
    let word = "guidebook"
    io.say(word.slice(0, 5))
    io.say(word.reverse())
    io.say("[{"42".pad_left(6, "0")}]")
    io.say("[{"name".pad_right(10)}]")
}
```

## Characters

`chars()` gives a list of one character pieces.

```sprfst
use std.io

fn main() {
    var vowels = 0
    for ch in "programming language".chars() {
        if "aeiou".contains(ch) { vowels = vowels + 1 }
    }
    io.say("{vowels} vowels")
}
```

## Turning text into numbers

`to_int` and `to_num` give back nothing when the text is not a number,
so you have to say what should happen instead.

```sprfst
use std.io

fn main() {
    io.say("{"42".to_int() ?? 0}")
    io.say("{"not a number".to_int() ?? 0}")
}
```

## Exercise

Write `is_palindrome(text)` that ignores spaces and capital letters.
`"Never odd or even"` should come back `true`.

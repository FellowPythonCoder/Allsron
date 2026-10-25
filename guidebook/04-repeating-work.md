# Repeating work

Three loops, each with a clear job.

## `while` — carry on until something changes

```sprfst
use std.io

fn main() {
    var countdown = 3
    while countdown > 0 {
        io.say("{countdown}...")
        countdown = countdown - 1
    }
    io.say("go")
}
```

## `for` — walk through a collection or a range

```sprfst
use std.io

fn main() {
    for n in 1..4 {
        io.say("number {n}")
    }

    for colour in ["red", "green", "blue"] {
        io.say(colour)
    }
}
```

`1..4` counts 1, 2, 3. Use `1..=4` when you want to include the end.

## `loop` — go round until you break out

```sprfst
use std.io

fn main() {
    var total = 0
    var n = 1
    loop {
        total = total + n
        if total > 20 { break }
        n = n + 1
    }
    io.say("stopped at n = {n}, total = {total}")
}
```

## `skip` and `break`

`skip` jumps to the next turn of the loop; `break` leaves it.

```sprfst
use std.io

fn main() {
    for n in 1..11 {
        if n % 2 == 0 { skip }
        if n > 7 { break }
        io.say("odd: {n}")
    }
}
```

## Walking a map

```sprfst
use std.io

fn main() {
    let prices = ["bread": 2, "milk": 1, "jam": 4]
    for item in prices.keys() {
        io.say("{item} costs {prices.get(item) ?? 0}")
    }
}
```

## Exercise

Print the first fifteen numbers of the Fibonacci sequence, one per line,
using a `while` loop and two `var` values.

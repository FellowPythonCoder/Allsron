# Making decisions

`if` chooses between paths. There are no brackets around the condition.

```sprfst
use std.io

fn main() {
    let temperature = 31

    if temperature > 30 {
        io.say("hot")
    } else if temperature > 18 {
        io.say("pleasant")
    } else {
        io.say("cold")
    }
}
```

## `if` gives a value

A block ends in a value, so `if` can sit on the right of `let`:

```sprfst
use std.io

fn main() {
    let hour = 15
    let greeting = if hour < 12 { "good morning" } else { "good afternoon" }
    io.say(greeting)
}
```

## Joining conditions

SPRFST spells the logical operators out: `and`, `or`, `not`. They read
aloud the way they behave.

```sprfst
use std.io

fn main() {
    let age = 24
    let member = true

    if age >= 18 and member {
        io.say("welcome in")
    }
    if not member or age < 18 {
        io.say("sign up at the desk")
    }
}
```

`and` and `or` stop early: if the left side already decides the answer,
the right side is never worked out.

## Comparing

`==` `!=` `<` `<=` `>` `>=` work on numbers and text. Text compares
alphabetically.

```sprfst
use std.io

fn main() {
    io.say("{"apple" < "banana"}")
    io.say("{3 != 4}")
}
```

## Exercise

Write a function that takes a year and says whether it is a leap year. A
year is a leap year when it divides by 4, except years that divide by
100, unless they also divide by 400.

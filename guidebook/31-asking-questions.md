# Asking questions

Until now a program has only talked. This chapter is about listening:
reading what a person types, one line at a time, and doing something
sensible when they type something unexpected.

## One line in

`io.ask` prints a prompt, waits for a line and gives it back as `Text`.

```sprfst
use std.io

fn main() {
    let name = io.ask("What is your name? ")
    io.say("Nice to meet you, {name}.")
}
```

In a terminal you simply type the answer. In **Studio**, the console has an
**Input** box above the output. Whatever you type there is what the program
reads: one answer per line, in the order the program asks for them.

```text
Ada
```

If the box is empty, `io.ask` gives back an empty `Text` and the program
carries on. A program can never hang waiting for a keyboard that is not there.

## Numbers and yes or no

Text that should be a number has to be checked, because people type anything.
These three do the checking for you and give back `nil` when the answer is not
what was asked for.

| Function | Gives back | When |
| --- | --- | --- |
| `io.ask_int(prompt)` | `Int?` | the line is a whole number such as `42` |
| `io.ask_num(prompt)` | `Num?` | the line is any number such as `3.5` |
| `io.ask_yes(prompt)` | `Bool` | `y`, `yes`, `true` or `1` mean true; anything else is false |

An optional is opened with `??`, which supplies a fallback, or compared with
`nil`.

```sprfst
use std.io

fn main() {
    let age = io.ask_int("How old are you? ")
    if age == nil {
        io.say("That was not a whole number.")
        give
    }
    let years = age ?? 0
    io.say("Next year you will be {years + 1}.")
    if io.ask_yes("Shall I count to {years}? ") {
        for n in 1..(years + 1) { io.say("{n}") }
    }
}
```

```text
36
yes
```

## Keep asking until it is right

A loop turns a single question into a conversation. Here `while` allows five
tries, and `give` leaves the function as soon as the answer is good.

```sprfst
use std.io

fn main() {
    var tries = 0
    while tries < 5 {
        tries = tries + 1
        let n = io.ask_num("A number between 1 and 10: ")
        if n == nil {
            io.say("Numbers only, please.")
            skip
        }
        let value = n ?? 0.0
        if value >= 1.0 and value <= 10.0 {
            io.say("{value} it is.")
            give
        }
        io.say("Out of range.")
    }
    io.say("Let's try again another time.")
}
```

When the input runs out, `ask_num` gives `nil` every time, so a loop with no
limit would never finish. Count the attempts, as above, or use `io.read_line`,
which tells you when the input has ended.

## Until the input ends

`io.read_line` reads a line without printing a prompt. At the end of the
input it gives back `nil`, which is how a program knows to stop.

```sprfst
use std.io

fn main() {
    var total = 0.0
    var count = 0
    loop {
        let line = io.read_line()
        if line == nil { break }
        let n = (line ?? "").trim().to_num()
        if n == nil { skip }
        total = total + (n ?? 0.0)
        count = count + 1
    }
    io.say("{count} numbers, total {total}")
}
```

```text
4
8.5
fifteen
2
```

`io.read_all()` takes the rest of the input in one piece, handy for text you
want to split and process yourself.

```sprfst
use std.io

fn main() {
    let text = io.read_all()
    io.say("{text.lines().len()} lines, {text.len()} characters")
}
```

## Quick reference

| Function | Prompts | Ends with |
| --- | --- | --- |
| `io.ask(p)` | yes | empty `Text` |
| `io.ask_int(p)` / `io.ask_num(p)` | yes | `nil` |
| `io.ask_yes(p)` | yes | `false` |
| `io.read_line()` | no | `nil` |
| `io.read_all()` | no | empty `Text` |

## Try it

- Ask for two numbers and print their sum, repeating the question until both are valid.
- Write a guessing game: pick a number, then keep calling `io.ask_int` until it is found.

When you want buttons and fields instead of lines, the next step is a window.
That is the next chapter.

# Making an app

A program that opens a window is an **app**. In SPRFST an app is a few lines:
some state, and a window that is drawn from it. Change the state and the
window follows.

## Your first app, in Studio

1. Open Studio and, in the **Your programs** panel, press the app button
   next to **+**. A small starter app appears in the editor.
2. Press **Run**. Instead of text, the console shows a **live preview**: a
   window you can click and type in.
3. Edit the code and run it again.

The preview is real. Every click or edit runs your program again with the
same events, so what you see is what your code does.

## The shape of an app

```sprfst
use std.io

app "Counter" {
    var count = 0

    window {
        title: "Counter"
        width: 320
        height: 200

        title "Clicked {count} times"
        button "Add one" {
            on click {
                count = count + 1
            }
        }
        button "Start again" {
            on click {
                count = 0
            }
        }
    }
}
```

- `app "Name" { }` holds everything.
- `var` lines are the app's **state**. They are the only things that change.
- `window { }` describes what to draw. Text containing `{holes}` is redrawn
  whenever the state it mentions changes.
- `on click { }` is a handler. It changes state, and nothing else is needed.

You never say "update the label". You change `count` and the title follows.

## Taking input

A `field` shows a value and reports what was typed. Bind it with `value:`
going in and `on change(text)` coming out.

```sprfst
use std.io

app "Greeter" {
    var name = ""

    window {
        title: "Greeter"
        width: 400
        height: 220

        title "Hello, {name}"
        field "Your name" {
            value: name
            on change(text) {
                name = text
            }
        }
        button "Forget me" {
            on click {
                name = ""
            }
        }
    }
}
```

The same pattern covers `toggle` (a tick box that reports `ticked`) and
`list` (which shows an array through `items:`). Chapter 21 lists every widget.

## A small real app

A to-do list: one field, one button, one list. State holds the draft and the
items.

```sprfst
use std.io

app "Todo" {
    var draft = ""
    var todos: [Text] = []

    window {
        title: "Todo"
        width: 460
        height: 480

        title "{todos.len()} things to do"
        field "What needs doing?" {
            value: draft
            on change(text) {
                draft = text
            }
        }
        button "Add" {
            on click {
                if draft.trim().len() > 0 {
                    todos.push(draft.trim())
                    draft = ""
                }
            }
        }
        divider
        list {
            items: todos
        }
        button "Clear all" {
            on click {
                todos = []
            }
        }
    }
}
```

Notice that "Add" empties `draft`. The field is bound to it, so the box clears
by itself.

## From the terminal

To start a project that is an app from the beginning:

```
sprfst new app notes
cd notes
sprfst run .
```

On a Mac the window opens for real. Without a screen, set `SPRFST_UI`:

| Value | What you get |
| --- | --- |
| `term` | the window drawn in the terminal, with numbered controls |
| `json` | the window as JSON, then events read from standard input |
| `none` | build the window, print its description, exit |

## Testing an app without a screen

The `json` backend prints the window after every event. An event is one line:
`event <id> <name> [payload]`. The window itself is id 0 and each widget
numbers up from there, in the order written.

```
printf 'event 2 change Ada\nevent 3 click\nquit\n' | SPRFST_UI=json sprfst run app.spf
```

That is exactly what Studio's preview does, which is why the preview and your
program can never disagree.

## Keeping your apps

Save a program in Studio and it lands in **Documents / Sreon Studio**. Any
saved program that contains an `app` block is added to the **dock** on the
new tab page, so your own apps sit next to Studio, Shield and Settings. Click
one and it opens in Studio, ready to run.

## Try it

- Add a "Remove last" button to the to-do app using `todos.pop()`.
- Make a tip calculator with two fields and a title that shows the total.
- Make an app with a `toggle` that changes what the title says.

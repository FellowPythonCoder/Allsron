/* ========================================================================
   SPRFST — native standard library surface

   One declarative table drives three things at once:
     * the names/types the compiler knows about  (sema)
     * the dispatch ids the runtime switches on  (vm)
     * `sprfst docs` output                      (docs)

   Signature grammar (parsed by sema_parse_signature):
       (T, T, ...) -> T
   where T is  Int Num Text Bool Byte Nil Any Self
               [T]  [K:T]  T?  List<T> Map<K,T> Set<T> Chan<T> Future<T>
               Result<T,E>  fn(T,...)->T   and single capital letters = generic.
   A module name beginning with '@' declares methods on a builtin type.
   ======================================================================== */
#ifndef SPRFST_NATIVES_H
#define SPRFST_NATIVES_H

#include "sprfst/common.h"

#define SPRFST_NATIVE_LIST(X)                                                                        \
/* ------------------------------------------------------------------ io */                          \
X(IO_SAY,        "io",   "say",        "(Any) -> Nil",            "Write a value and a line break to standard output") \
X(IO_PRINT,      "io",   "print",      "(Any) -> Nil",            "Write a value with no trailing line break")          \
X(IO_WARN,       "io",   "warn",       "(Any) -> Nil",            "Write a value to standard error")                    \
X(IO_ASK,        "io",   "ask",        "(Text) -> Text",          "Print a prompt and read one line from standard input")\
X(IO_READ_LINE,  "io",   "read_line",  "() -> Text?",             "Read one line of standard input, or nil at end")      \
X(IO_ASK_NUM,    "io",   "ask_num",    "(Text) -> Num?",          "Print a prompt, read a line and give it as a number (nil if it is not one)") \
X(IO_ASK_INT,    "io",   "ask_int",    "(Text) -> Int?",          "Print a prompt, read a line and give it as a whole number (nil if it is not one)") \
X(IO_ASK_YES,    "io",   "ask_yes",    "(Text) -> Bool",          "Print a prompt and give true for y, yes, true or 1")  \
X(IO_READ_ALL,   "io",   "read_all",   "() -> Text",              "Read everything left on standard input")              \
X(IO_FLUSH,      "io",   "flush",      "() -> Nil",               "Flush buffered output")                               \
/* ---------------------------------------------------------------- math */                          \
X(MATH_SQRT,     "math", "sqrt",       "(Num) -> Num",            "Square root")                                         \
X(MATH_ABS,      "math", "abs",        "(Num) -> Num",            "Absolute value")                                      \
X(MATH_FLOOR,    "math", "floor",      "(Num) -> Int",            "Round down to an Int")                                \
X(MATH_CEIL,     "math", "ceil",       "(Num) -> Int",            "Round up to an Int")                                  \
X(MATH_ROUND,    "math", "round",      "(Num) -> Int",            "Round to the nearest Int")                            \
X(MATH_POW,      "math", "pow",        "(Num, Num) -> Num",       "Raise to a power")                                    \
X(MATH_SIN,      "math", "sin",        "(Num) -> Num",            "Sine of an angle in radians")                         \
X(MATH_COS,      "math", "cos",        "(Num) -> Num",            "Cosine of an angle in radians")                       \
X(MATH_TAN,      "math", "tan",        "(Num) -> Num",            "Tangent of an angle in radians")                      \
X(MATH_ATAN2,    "math", "atan2",      "(Num, Num) -> Num",       "Angle of the vector (x, y)")                          \
X(MATH_LOG,      "math", "log",        "(Num) -> Num",            "Natural logarithm")                                   \
X(MATH_LOG2,     "math", "log2",       "(Num) -> Num",            "Base 2 logarithm")                                    \
X(MATH_LOG10,    "math", "log10",      "(Num) -> Num",            "Base 10 logarithm")                                   \
X(MATH_EXP,      "math", "exp",        "(Num) -> Num",            "e raised to a power")                                 \
X(MATH_MIN,      "math", "min",        "(Num, Num) -> Num",       "Smaller of two numbers")                              \
X(MATH_MAX,      "math", "max",        "(Num, Num) -> Num",       "Larger of two numbers")                               \
X(MATH_CLAMP,    "math", "clamp",      "(Num, Num, Num) -> Num",  "Clamp a value into a range")                          \
X(MATH_PI,       "math", "pi",         "() -> Num",               "The constant pi")                                     \
X(MATH_E,        "math", "e",          "() -> Num",               "Euler's number")                                      \
X(MATH_INF,      "math", "inf",        "() -> Num",               "Positive infinity")                                   \
X(MATH_IS_NAN,   "math", "is_nan",     "(Num) -> Bool",           "True when the value is not a number")                 \
X(MATH_HYPOT,    "math", "hypot",      "(Num, Num) -> Num",       "Length of the vector (x, y)")                         \
/* ---------------------------------------------------------------- text */                          \
X(TEXT_LEN,      "@Text","len",        "() -> Int",               "Number of bytes in the text")                         \
X(TEXT_CHARS,    "@Text","chars",      "() -> [Text]",            "Split into a list of characters")                     \
X(TEXT_UPPER,    "@Text","upper",      "() -> Text",              "Uppercase copy")                                      \
X(TEXT_LOWER,    "@Text","lower",      "() -> Text",              "Lowercase copy")                                      \
X(TEXT_TRIM,     "@Text","trim",       "() -> Text",              "Remove leading and trailing whitespace")              \
X(TEXT_SPLIT,    "@Text","split",      "(Text) -> [Text]",        "Split on a separator")                                \
X(TEXT_LINES,    "@Text","lines",      "() -> [Text]",            "Split into lines")                                    \
X(TEXT_CONTAINS, "@Text","contains",   "(Text) -> Bool",          "True when the text contains a part")                  \
X(TEXT_STARTS,   "@Text","starts_with","(Text) -> Bool",          "True when the text starts with a prefix")             \
X(TEXT_ENDS,     "@Text","ends_with",  "(Text) -> Bool",          "True when the text ends with a suffix")               \
X(TEXT_REPLACE,  "@Text","replace",    "(Text, Text) -> Text",    "Replace every occurrence")                            \
X(TEXT_SLICE,    "@Text","slice",      "(Int, Int) -> Text",      "Substring between two byte offsets")                  \
X(TEXT_INDEX_OF, "@Text","index_of",   "(Text) -> Int",           "Byte offset of a part, or -1")                        \
X(TEXT_REPEAT,   "@Text","repeat",     "(Int) -> Text",           "Repeat the text n times")                             \
X(TEXT_PAD_LEFT, "@Text","pad_left",   "(Int, Text?) -> Text",     "Pad on the left to a width")                          \
X(TEXT_PAD_RIGHT,"@Text","pad_right",  "(Int, Text?) -> Text",     "Pad on the right to a width")                         \
X(TEXT_TO_INT,   "@Text","to_int",     "() -> Int?",              "Parse an Int, nil when it is not a number")           \
X(TEXT_TO_NUM,   "@Text","to_num",     "() -> Num?",              "Parse a Num, nil when it is not a number")            \
X(TEXT_REVERSE,  "@Text","reverse",    "() -> Text",              "Reversed copy")                                       \
X(TEXT_CODE_AT,  "@Text","code_at",    "(Int) -> Int",            "Byte value at an offset")                             \
X(TEXT_CHAR_AT,  "@Text","char_at",    "(Int) -> Text",           "One character at an offset")                          \
X(TEXT_IS_EMPTY, "@Text","is_empty",   "() -> Bool",              "True when the text has no bytes")                     \
X(TEXT_BYTES,    "@Text","bytes",      "() -> [Int]",             "Bytes of the text as a list")                         \
X(TEXT_TO_TEXT,  "@Text","to_text",    "() -> Text",              "The text itself")                                     \
/* ---------------------------------------------------------------- list */                          \
X(LIST_LEN,      "@List","len",        "() -> Int",               "Number of items")                                     \
X(LIST_PUSH,     "@List","push",       "(T) -> Nil",              "Append an item")                                      \
X(LIST_POP,      "@List","pop",        "() -> T?",                "Remove and return the last item")                     \
X(LIST_INSERT,   "@List","insert",     "(Int, T) -> Nil",         "Insert an item at an index")                          \
X(LIST_REMOVE,   "@List","remove",     "(Int) -> T?",             "Remove the item at an index")                         \
X(LIST_CLEAR,    "@List","clear",      "() -> Nil",               "Remove every item")                                   \
X(LIST_CONTAINS, "@List","contains",   "(T) -> Bool",             "True when an equal item is present")                  \
X(LIST_INDEX_OF, "@List","index_of",   "(T) -> Int",              "Index of an item, or -1")                             \
X(LIST_FIRST,    "@List","first",      "() -> T?",                "First item, or nil")                                  \
X(LIST_LAST,     "@List","last",       "() -> T?",                "Last item, or nil")                                   \
X(LIST_SLICE,    "@List","slice",      "(Int, Int) -> [T]",       "Items between two indexes")                           \
X(LIST_REVERSE,  "@List","reverse",    "() -> [T]",               "Reversed copy")                                       \
X(LIST_SORT,     "@List","sort",       "() -> [T]",               "Sorted copy, ascending")                              \
X(LIST_SORT_BY,  "@List","sort_by",    "(fn(T, T) -> Bool) -> [T]","Sorted copy; the test answers \"does a come first\"")  \
X(LIST_MAP,      "@List","map",        "(fn(T) -> U) -> [U]",     "Apply a function to every item")                      \
X(LIST_FILTER,   "@List","filter",     "(fn(T) -> Bool) -> [T]",  "Keep the items a test accepts")                       \
X(LIST_REDUCE,   "@List","reduce",     "(U, fn(U, T) -> U) -> U", "Fold the list into one value")                        \
X(LIST_EACH,     "@List","each",       "(fn(T) -> Nil) -> Nil",   "Run a function for every item")                       \
X(LIST_ANY,      "@List","any",        "(fn(T) -> Bool) -> Bool", "True when a test accepts any item")                   \
X(LIST_ALL,      "@List","all",        "(fn(T) -> Bool) -> Bool", "True when a test accepts every item")                 \
X(LIST_COUNT,    "@List","count",      "(fn(T) -> Bool) -> Int",  "How many items a test accepts")                       \
X(LIST_FIND,     "@List","find",       "(fn(T) -> Bool) -> T?",   "First item a test accepts")                           \
X(LIST_JOIN,     "@List","join",       "(Text) -> Text",          "Join the items into text")                            \
X(LIST_SUM,      "@List","sum",        "() -> Num",               "Sum of numeric items")                                \
X(LIST_MIN,      "@List","min",        "() -> T?",                "Smallest item")                                       \
X(LIST_MAX,      "@List","max",        "() -> T?",                "Largest item")                                        \
X(LIST_COPY,     "@List","copy",       "() -> [T]",               "Shallow copy")                                        \
X(LIST_CONCAT,   "@List","concat",     "([T]) -> [T]",            "Copy with another list appended")                     \
X(LIST_TAKE,     "@List","take",       "(Int) -> [T]",            "First n items")                                       \
X(LIST_DROP,     "@List","drop",       "(Int) -> [T]",            "All but the first n items")                           \
X(LIST_IS_EMPTY, "@List","is_empty",   "() -> Bool",              "True when the list has no items")                     \
X(LIST_FLATTEN,  "@List","flatten",    "() -> [Any]",             "Flatten one level of nesting")                        \
/* ----------------------------------------------------------------- map */                          \
X(MAP_LEN,       "@Map", "len",        "() -> Int",               "Number of entries")                                   \
X(MAP_GET,       "@Map", "get",        "(K) -> V?",               "Value for a key, or nil")                             \
X(MAP_SET,       "@Map", "set",        "(K, V) -> Nil",           "Store a value for a key")                             \
X(MAP_HAS,       "@Map", "has",        "(K) -> Bool",             "True when the key is present")                        \
X(MAP_REMOVE,    "@Map", "remove",     "(K) -> V?",               "Remove a key and return its value")                   \
X(MAP_KEYS,      "@Map", "keys",       "() -> [K]",               "Every key")                                           \
X(MAP_VALUES,    "@Map", "values",     "() -> [V]",               "Every value")                                         \
X(MAP_CLEAR,     "@Map", "clear",      "() -> Nil",               "Remove every entry")                                  \
X(MAP_IS_EMPTY,  "@Map", "is_empty",   "() -> Bool",              "True when the map has no entries")                    \
/* ----------------------------------------------------------------- set */                          \
X(SET_ADD,       "@Set", "add",        "(T) -> Nil",              "Add an item")                                         \
X(SET_HAS,       "@Set", "has",        "(T) -> Bool",             "True when the item is present")                       \
X(SET_REMOVE,    "@Set", "remove",     "(T) -> Bool",             "Remove an item")                                      \
X(SET_LEN,       "@Set", "len",        "() -> Int",               "Number of items")                                     \
X(SET_ITEMS,     "@Set", "items",      "() -> [T]",               "Items as a list")                                     \
/* -------------------------------------------------------------- future */                          \
X(FUT_AWAIT,     "@Future","wait",     "() -> T",                 "Block until the task finishes")                       \
X(FUT_DONE,      "@Future","is_done",  "() -> Bool",              "True when the task has finished")                     \
/* ----------------------------------------------------------------- chan */                         \
X(CHAN_SEND,     "@Chan","send",       "(T) -> Nil",              "Send a value (blocks when full)")                     \
X(CHAN_RECV,     "@Chan","recv",       "() -> T?",                "Receive a value (blocks when empty)")                 \
X(CHAN_TRY_RECV, "@Chan","try_recv",   "() -> T?",                "Receive without blocking")                            \
X(CHAN_CLOSE,    "@Chan","close",      "() -> Nil",               "Close the channel")                                   \
X(CHAN_LEN,      "@Chan","len",        "() -> Int",               "Number of queued values")                             \
/* ------------------------------------------------------------- builtins */                         \
X(B_LEN,         "",     "len",        "(Any) -> Int",            "Length of text, list, map or set")                    \
X(B_TO_TEXT,     "",     "to_text",    "(Any) -> Text",           "Convert any value to readable text")                  \
X(B_TO_INT,      "",     "to_int",     "(Any) -> Int",            "Convert a number or text to Int")                     \
X(B_TO_NUM,      "",     "to_num",     "(Any) -> Num",            "Convert a number or text to Num")                     \
X(B_TYPE_OF,     "",     "type_of",    "(Any) -> Text",           "Name of a value's runtime type")                      \
X(B_ASSERT,      "",     "assert",     "(Bool, Text) -> Nil",     "Fail the program when a condition is false")          \
X(B_PANIC,       "",     "panic",      "(Text) -> Nil",           "Stop the program with an error")                      \
X(B_CLONE,       "",     "clone",      "(T) -> T",                "A copy of a value, of the same type")                                \
X(B_HASH,        "",     "hash_of",    "(Any) -> Int",            "Stable hash of a value")                              \
X(B_CHAR_OF,     "",     "char_of",    "(Int) -> Text",           "The character with this code point, as UTF-8")        \
X(B_RANGE_LIST,  "",     "range",      "(Int, Int) -> [Int]",     "List of integers from start up to end")               \
X(B_SET,         "",     "set",        "([T]) -> {T}",            "Make a set out of a list")                            \
/* ------------------------------------------------------------------ fs */                          \
X(FS_READ,       "fs",   "read",       "(Text) -> Text?",         "Read a whole file as text")                           \
X(FS_WRITE,      "fs",   "write",      "(Text, Text) -> Bool",    "Write text to a file")                                \
X(FS_APPEND,     "fs",   "append",     "(Text, Text) -> Bool",    "Append text to a file")                               \
X(FS_EXISTS,     "fs",   "exists",     "(Text) -> Bool",          "True when a path exists")                             \
X(FS_REMOVE,     "fs",   "remove",     "(Text) -> Bool",          "Delete a file")                                       \
X(FS_LIST,       "fs",   "list",       "(Text) -> [Text]",        "Names inside a directory")                            \
X(FS_MKDIR,      "fs",   "mkdir",      "(Text) -> Bool",          "Create a directory and its parents")                  \
X(FS_IS_DIR,     "fs",   "is_dir",     "(Text) -> Bool",          "True when a path is a directory")                     \
X(FS_IS_FILE,    "fs",   "is_file",    "(Text) -> Bool",          "True when a path is a regular file")                  \
X(FS_SIZE,       "fs",   "size",       "(Text) -> Int",           "Size of a file in bytes")                             \
X(FS_COPY,       "fs",   "copy",       "(Text, Text) -> Bool",    "Copy a file")                                         \
X(FS_RENAME,     "fs",   "rename",     "(Text, Text) -> Bool",    "Rename or move a file")                               \
X(FS_READ_BYTES, "fs",   "read_bytes", "(Text) -> [Int]?",        "Read a file as a list of bytes")                      \
X(FS_WRITE_BYTES,"fs",   "write_bytes","(Text, [Int]) -> Bool",   "Write a list of bytes to a file")                     \
/* ---------------------------------------------------------------- path */                          \
X(PATH_JOIN,     "path", "join",       "(Text, Text) -> Text",    "Join two path parts")                                 \
X(PATH_DIR,      "path", "dirname",    "(Text) -> Text",          "Directory part of a path")                            \
X(PATH_BASE,     "path", "basename",   "(Text) -> Text",          "File name part of a path")                            \
X(PATH_EXT,      "path", "ext",        "(Text) -> Text",          "Extension of a path")                                 \
X(PATH_ABS,      "path", "abs",        "(Text) -> Text",          "Absolute form of a path")                             \
/* ---------------------------------------------------------------- time */                          \
X(TIME_NOW,      "time", "now",        "() -> Num",               "Seconds since the epoch, with fractions")             \
X(TIME_MS,       "time", "ms",         "() -> Int",               "Milliseconds since the epoch")                        \
X(TIME_CLOCK,    "time", "clock",      "() -> Num",               "High resolution timer in seconds")                    \
X(TIME_SLEEP,    "time", "sleep",      "(Num) -> Nil",            "Pause for a number of seconds")                       \
X(TIME_FORMAT,   "time", "format",     "(Num, Text) -> Text",     "Format a timestamp (%Y-%m-%d %H:%M:%S)")              \
X(TIME_PARTS,    "time", "parts",      "(Num) -> [Text: Int]",    "Year, month, day, hour, minute, second")              \
/* ---------------------------------------------------------------- rand */                          \
X(RAND_SEED,     "rand", "seed",       "(Int) -> Nil",            "Seed the generator")                                  \
X(RAND_INT,      "rand", "int",        "(Int, Int) -> Int",       "Random integer in a range")                           \
X(RAND_NUM,      "rand", "num",        "() -> Num",               "Random number between 0 and 1")                       \
X(RAND_CHOICE,   "rand", "choice",     "([T]) -> T?",             "Random item from a list")                             \
X(RAND_SHUFFLE,  "rand", "shuffle",    "([T]) -> [T]",            "Shuffled copy of a list")                             \
X(RAND_NORMAL,   "rand", "normal",     "(Num, Num) -> Num",       "Normally distributed random number")                  \
/* ----------------------------------------------------------------- sys */                          \
X(SYS_ARGS,      "sys",  "args",       "() -> [Text]",            "Command line arguments")                              \
X(SYS_ENV,       "sys",  "env",        "(Text) -> Text?",         "Value of an environment variable")                    \
X(SYS_SET_ENV,   "sys",  "set_env",    "(Text, Text) -> Nil",     "Set an environment variable")                         \
X(SYS_EXIT,      "sys",  "exit",       "(Int) -> Nil",            "Stop the program with an exit code")                  \
X(SYS_PLATFORM,  "sys",  "platform",   "() -> Text",              "Operating system name")                               \
X(SYS_ARCH,      "sys",  "arch",       "() -> Text",              "Processor architecture")                              \
X(SYS_CPUS,      "sys",  "cpu_count",  "() -> Int",               "Number of logical processors")                        \
X(SYS_RUN,       "sys",  "run",        "(Text) -> Text",          "Run a shell command and capture its output")          \
X(SYS_MEMORY,    "sys",  "memory",     "() -> Int",               "Bytes currently allocated by the runtime")            \
/* ---------------------------------------------------------------- json */                          \
X(JSON_PARSE,    "json", "parse",      "(Text) -> Any?",          "Parse JSON text into values")                         \
X(JSON_WRITE,    "json", "write",      "(Any) -> Text",           "Serialise a value as JSON")                           \
X(JSON_PRETTY,   "json", "pretty",     "(Any) -> Text",           "Serialise a value as indented JSON")                  \
/* ----------------------------------------------------------------- csv */                          \
X(CSV_PARSE,     "csv",  "parse",      "(Text) -> [[Text]]",      "Parse CSV text into rows")                            \
X(CSV_WRITE,     "csv",  "write",      "([[Text]]) -> Text",      "Write rows as CSV text")                              \
/* ---------------------------------------------------------------- hash */                          \
X(HASH_SHA256,   "hash", "sha256",     "(Text) -> Text",          "SHA-256 digest as hex text")                          \
X(HASH_CRC32,    "hash", "crc32",      "(Text) -> Int",           "CRC-32 checksum")                                     \
X(HASH_FNV,      "hash", "fnv64",      "(Text) -> Int",           "FNV-1a 64 bit hash")                                  \
X(HASH_B64ENC,   "hash", "base64",     "(Text) -> Text",          "Base64 encode")                                       \
X(HASH_B64DEC,   "hash", "unbase64",   "(Text) -> Text",          "Base64 decode")                                       \
X(HASH_HMAC,     "hash", "hmac_sha256","(Text, Text) -> Text",    "HMAC-SHA-256 of a message with a key")                \
/* ------------------------------------------------------------------ net */                         \
X(NET_LISTEN,    "net",  "listen",     "(Int) -> Int",            "Listen for TCP connections on a port")                \
X(NET_ACCEPT,    "net",  "accept",     "(Int) -> Int",            "Accept one TCP connection")                           \
X(NET_CONNECT,   "net",  "connect",    "(Text, Int) -> Int",      "Open a TCP connection")                               \
X(NET_SEND,      "net",  "send",       "(Int, Text) -> Int",      "Send text on a socket")                               \
X(NET_RECV,      "net",  "recv",       "(Int, Int) -> Text",      "Receive up to n bytes")                               \
X(NET_CLOSE,     "net",  "close",      "(Int) -> Nil",            "Close a socket")                                      \
X(NET_RESOLVE,   "net",  "resolve",    "(Text) -> Text?",         "Resolve a host name to an address")                   \
X(NET_UDP_OPEN,  "net",  "udp_open",   "(Int) -> Int",            "Open a UDP socket on a port")                         \
X(NET_UDP_SEND,  "net",  "udp_send",   "(Int, Text, Int, Text) -> Int", "Send a UDP datagram")                           \
X(NET_UDP_RECV,  "net",  "udp_recv",   "(Int, Int) -> Text",      "Receive a UDP datagram")                              \
X(NET_HOST,      "net",  "hostname",   "() -> Text",              "Name of this machine")                                \
/* ---------------------------------------------------------------- http */                          \
X(HTTP_GET,      "http", "get",        "(Text) -> Text?",         "Fetch a URL with GET")                                \
X(HTTP_POST,     "http", "post",       "(Text, Text) -> Text?",   "Send a POST request")                                 \
X(HTTP_SERVE,    "http", "serve",      "(Int, fn(Any) -> Any) -> Nil", "Run an HTTP server on a port")                   \
X(HTTP_STOP,     "http", "stop",       "() -> Nil",               "Stop the running HTTP server")                        \
X(HTTP_URLENC,   "http", "url_encode", "(Text) -> Text",          "Percent-encode text for a URL")                       \
X(HTTP_URLDEC,   "http", "url_decode", "(Text) -> Text",          "Decode percent-encoded text")                         \
/* -------------------------------------------------------------- threads */                         \
X(THREAD_SPAWN,  "thread","spawn",     "(fn() -> Any) -> Future<Any>", "Run a function on another thread")               \
X(THREAD_CPUS,   "thread","cpus",      "() -> Int",               "Number of hardware threads")                          \
X(THREAD_SLEEP,  "thread","sleep",     "(Num) -> Nil",            "Pause this thread")                                   \
X(THREAD_LOCK,   "thread","lock",      "() -> Int",               "Create a mutual exclusion lock")                      \
X(THREAD_ACQ,    "thread","acquire",   "(Int) -> Nil",            "Take a lock")                                         \
X(THREAD_REL,    "thread","release",   "(Int) -> Nil",            "Release a lock")                                      \
X(THREAD_ATOMIC, "thread","atomic_add","(Int, Int) -> Int",       "Atomically add to a counter")                         \
X(THREAD_COUNTER,"thread","counter",   "(Int) -> Int",            "Create an atomic counter with a start value")         \
X(THREAD_CHAN,   "thread","channel",   "(Int) -> Chan<Any>",      "Create a channel with a buffer size")                 \
/* ----------------------------------------------------------------- task */                         \
X(TASK_SLEEP,    "task", "sleep",      "(Num) -> Nil",            "Suspend the current task")                            \
X(TASK_YIELD,    "task", "yield",      "() -> Nil",               "Give other tasks a turn")                             \
X(TASK_RUN,      "task", "run_all",    "() -> Nil",               "Run queued tasks until they finish")                  \
X(TASK_COUNT,    "task", "pending",    "() -> Int",               "How many tasks are waiting")                          \
/* ------------------------------------------------------------------ db */                          \
X(DB_OPEN,       "db",   "open",       "(Text) -> Int",           "Open or create an Ember database file")               \
X(DB_EXEC,       "db",   "exec",       "(Int, Text) -> Int",      "Run a statement, give the number of affected rows")   \
X(DB_QUERY,      "db",   "query",      "(Int, Text) -> [[Text: Any]]", "Run a query and return rows")                    \
X(DB_CLOSE,      "db",   "close",      "(Int) -> Nil",            "Close a database")                                    \
X(DB_BEGIN,      "db",   "begin",      "(Int) -> Nil",            "Start a transaction")                                 \
X(DB_COMMIT,     "db",   "commit",     "(Int) -> Nil",            "Commit a transaction")                                \
X(DB_ROLLBACK,   "db",   "rollback",   "(Int) -> Nil",            "Undo a transaction")                                  \
X(DB_TABLES,     "db",   "tables",     "(Int) -> [Text]",         "Names of the tables in a database")                   \
/* ------------------------------------------------------------- tensors */                          \
X(T_NEW,         "tensor","make",      "([Int], [Num]) -> Any",   "Create a tensor with a shape and data")               \
X(T_ZEROS,       "tensor","zeros",     "([Int]) -> Any",          "Tensor filled with zeros")                            \
X(T_RANDOM,      "tensor","random",    "([Int], Num) -> Any",     "Tensor filled with random values")                    \
X(T_SHAPE,       "tensor","shape",     "(Any) -> [Int]",          "Shape of a tensor")                                   \
X(T_GET,         "tensor","at",        "(Any, Int) -> Num",       "Read one element by flat index")                      \
X(T_SET,         "tensor","put",       "(Any, Int, Num) -> Nil",  "Write one element by flat index")                     \
X(T_ADD,         "tensor","add",       "(Any, Any) -> Any",       "Element-wise addition")                               \
X(T_SUB,         "tensor","sub",       "(Any, Any) -> Any",       "Element-wise subtraction")                            \
X(T_MUL,         "tensor","mul",       "(Any, Any) -> Any",       "Element-wise multiplication")                         \
X(T_SCALE,       "tensor","scale",     "(Any, Num) -> Any",       "Multiply every element by a number")                  \
X(T_MATMUL,      "tensor","matmul",    "(Any, Any) -> Any",       "Matrix multiplication")                               \
X(T_TRANSPOSE,   "tensor","transpose", "(Any) -> Any",            "Transpose a 2D tensor")                               \
X(T_RELU,        "tensor","relu",      "(Any) -> Any",            "Rectified linear activation")                         \
X(T_SIGMOID,     "tensor","sigmoid",   "(Any) -> Any",            "Logistic activation")                                 \
X(T_TANH,        "tensor","tanh",      "(Any) -> Any",            "Hyperbolic tangent activation")                       \
X(T_SOFTMAX,     "tensor","softmax",   "(Any) -> Any",            "Softmax over the last dimension")                     \
X(T_SUM,         "tensor","sum",       "(Any) -> Num",            "Sum of every element")                                \
X(T_MEAN,        "tensor","mean",      "(Any) -> Num",            "Average of every element")                            \
X(T_ARGMAX,      "tensor","argmax",    "(Any) -> Int",            "Index of the largest element")                        \
X(T_TOLIST,      "tensor","to_list",   "(Any) -> [Num]",          "Elements as a flat list")                             \
X(T_DOT,         "tensor","dot",       "(Any, Any) -> Num",       "Dot product of two tensors")                          \
/* ------------------------------------------------------------------ ui */                          \
X(UI_WINDOW,     "ui",   "window",     "(Text, Int, Int) -> Int", "Create a window and return its id")                   \
X(UI_NODE,       "ui",   "node",       "(Int, Text, Text) -> Int","Add a widget to a parent and return its id")          \
X(UI_SET,        "ui",   "set",        "(Int, Text, Any) -> Nil", "Set a widget property")                               \
X(UI_GET,        "ui",   "get",        "(Int, Text) -> Any",      "Read a widget property")                              \
X(UI_ON,         "ui",   "on",         "(Int, Text, fn() -> Nil) -> Nil", "Attach an event handler")                     \
X(UI_RUN,        "ui",   "run",        "() -> Nil",               "Run the user interface event loop")                   \
X(UI_QUIT,       "ui",   "quit",       "() -> Nil",               "Close the user interface")                            \
X(UI_RENDER,     "ui",   "describe",   "() -> Text",              "JSON description of the current UI tree")             \
X(UI_EMIT,       "ui",   "emit",       "(Int, Text) -> Nil",      "Deliver an event to a widget (used by hosts)")        \
X(UI_BACKEND,    "ui",   "backend",    "() -> Text",              "Name of the active UI backend")                       \
/* ---------------------------------------------------------------- draw */                          \
X(GFX_CANVAS,    "draw", "canvas",     "(Int, Int) -> Any",       "Create an image buffer")                              \
X(GFX_CLEAR,     "draw", "clear",      "(Any, Int) -> Nil",       "Fill an image with a colour")                         \
X(GFX_PIXEL,     "draw", "pixel",      "(Any, Int, Int, Int) -> Nil", "Set one pixel")                                   \
X(GFX_LINE,      "draw", "line",       "(Any, Int, Int, Int, Int, Int) -> Nil", "Draw a line")                           \
X(GFX_RECT,      "draw", "rect",       "(Any, Int, Int, Int, Int, Int) -> Nil", "Draw a filled rectangle")               \
X(GFX_CIRCLE,    "draw", "circle",     "(Any, Int, Int, Int, Int) -> Nil", "Draw a filled circle")                       \
X(GFX_TEXT,      "draw", "text",       "(Any, Int, Int, Text, Int) -> Nil", "Draw text with the built-in font")          \
X(GFX_SAVE,      "draw", "save_png",   "(Any, Text) -> Bool",     "Write an image to a PNG file")                        \
X(GFX_RGB,       "draw", "rgb",        "(Int, Int, Int) -> Int",  "Pack a colour from red, green and blue")              \
X(GFX_SIZE,      "draw", "size",       "(Any) -> [Int]",          "Width and height of an image")

typedef struct {
    const char *module;
    const char *name;
    const char *sig;
    const char *doc;
    int         id;
} NativeFn;

enum {
#define X(id, m, n, s, d) NF_##id,
    SPRFST_NATIVE_LIST(X)
#undef X
    NF_COUNT
};

extern const NativeFn SPRFST_NATIVES[NF_COUNT];

/* list of importable native module names (no '@' entries) */
const char **natives_module_names(int *count);
int natives_lookup(const char *module, const char *name);   /* -> id or -1 */

#endif

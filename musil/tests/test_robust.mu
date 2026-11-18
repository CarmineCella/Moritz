; test_robust.mu — what used to crash the interpreter or change the program behind its back.
;
; Every check is a (check ...) from test.mu. A passing run prints only the final line.
; Run with: musil tests/test_robust.mu

load "test.mu"

; --- the reader and the structural walks refuse what is nested too deep, with an error ------------------
var deep-text (concat (repeat "(" 200000) (repeat ")" 200000))
check (contains? (error-of (function () (parse deep-text))) "nested too deep") "parse: 200000 nested parentheses are an error, not a crash"
check (contains? (error-of (function () (parse (concat (repeat "'" 100000) "x")))) "nested too deep") "parse: 100000 quotes"
check (equal? (parse "(a (b (c)))") (parse "(a (b (c)))")) "parse: ordinary nesting still reads"
var da (list)
var db (list)
for (var k 0) (< k 200000) (var k (+ k 1)) {
    set da (list da)
    set db (list db)
}
check (contains? (error-of (function () (equal? da db))) "nested too deep") "equal?: lists nested 200000 deep are an error, not a crash"
check (equal? (list 1 (list 2 (list 3))) (list 1 (list 2 (list 3)))) "equal?: nested lists still compare"
check (contains? (error-of (function () (str da))) "") "str of a deep list does not crash"
set da nil
set db nil

; --- C++ exceptions and absurd sizes are Musil errors -----------------------------------------------------
check (contains? (error-of (function () (range 0 1 1e-300))) "too large") "range with a tiny step: an error"
check (contains? (error-of (function () (range 1e12))) "too large") "range 1e12: an error, not the machine's memory"
check (contains? (error-of (function () (range 0 inf))) "too large") "range to infinity: an error"
check (== (length (range 0 inf -1)) 0) "range with no elements"
check (equal? (try (range 1e12) catch e "caught") "caught") "try catches it"
check (contains? (error-of (function () (regex-match? "(" "x"))) "bad pattern") "a bad regular expression"

; --- the literals of the program text are never changed in place -----------------------------------------
function counter () {
    var x 0
    setidx x 0 (+ (getidx x 0) 1)
    return x
}
counter
counter
check (== (counter) 1) "a number literal bound by var is the variable's own: each call starts from 0"
function grow () {
    var L '()
    push L 1
    return (length L)
}
grow
check (== (grow) 1) "a quoted list is a fresh list each time: push does not grow the program"
function nested-quote () {
    var L '((1 2) (3 4))
    push (head L) 9
    return (length (head L))
}
nested-quote
check (== (nested-quote) 3) "a quoted list's inner lists are fresh too"
function keep-param (x) {
    setidx x 0 5
    return x
}
keep-param 1
check (== (keep-param 1) 5) "a literal argument is the parameter's own"
function zero () 0
check (contains? (error-of (function () (setidx (zero) 0 7))) "constant") "setidx of a literal returned as it is: refused"
check (== (zero) 0) "...and the function still returns 0"
var shared (vec 1 2 3)
var alias shared
setidx alias 0 9
check (== (getidx shared 0) 9) "vectors are still shared by reference (copy makes an independent one)"

; --- an error after a call reports the line of the caller, not the callee's last line --------------------
function two-lines () {
    var q 1
    (+ q 0)
}
function report-line () {
    var r (try (+ (two-lines) "x") catch e e)
    return r
}
var err-line (report-line)
check (contains? err-line "test_robust.mu:76:") (concat "the error names the line of the + (76): " err-line)
function multi-line-call () (+ 1
    (two-lines)
    "x")
check (contains? (error-of multi-line-call) "test_robust.mu:81:") (concat "a call over several lines: its first line " (error-of multi-line-call))

; --- files: only regular files are read ------------------------------------------------------------------
var zero-error (error-of (function () (read "/dev/zero")))      ; Windows has no /dev/zero: there, cannot open
check (or (contains? zero-error "not a regular file") (contains? zero-error "cannot open")) "read /dev/zero: refused"
check (contains? (error-of (function () (read "/tmp"))) "not a regular file") "read a directory: refused"


report "test_robust"

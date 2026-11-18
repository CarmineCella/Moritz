; test_semantics.mu — what 0.11 changes in the language: expr's && and || short-circuit, return, break and
; continue anywhere, a module loaded again when it changed, the libraries' names kept from the program's.
;
; Every check is a (check ...) from test.mu. A passing run prints only the final line.
; Run with: musil tests/test_semantics.mu

load "test.mu"

; --- return, break and continue are not exceptions: the same meaning everywhere ---------------------------
function first-over (v x) {
    for (var k 0) (< k (length v)) (var k (+ k 1)) {
        try { if (> (getidx v k) x) { return k } } catch e (error "not reached")
    }
    return -1
}
check (== (first-over (vec 1 5 9) 4) 1) "return from a loop inside try"
check (== (first-over (vec 1 2) 4) -1) "...and the fall-through"
var visited (list)
for (var i 0) (< i 3) (var i (+ i 1)) {
    for (var j 0) (< j 3) (var j (+ j 1)) {
        if (== j 1) { continue }
        if (== j 2) { break }
        push visited (list i j)
    }
}
check (equal? visited (list (list 0 0) (list 1 0) (list 2 0))) "break and continue act on the innermost loop"
var steps 0
for (var k 0) (< k 5) (var k (+ k 1)) {
    set steps (+ steps 1)
    if (< k 10) { continue }
}
check (== steps 5) "continue in for still runs the step"
function from-map () (map (list 1 2 3) (function (x) (if (== x 2) (return 99) x)))
check (equal? (from-map) (list 1 99 3)) "a return inside a lambda leaves the lambda, not its caller"
check (contains? (error-of (function () (break))) "outside loop") "break outside a loop is an error"
check (contains? (try (eval '(return 1)) catch e e) "return outside function") "return outside a function is an error"
function deep-return (n) {
    while 1 {
        while 1 {
            if (> n 0) { return (* n 2) }
            break
        }
        return 0
    }
}
check (== (deep-return 4) 8) "return from two loops deep"
check (== (expr ((deep-return 2) + 1)) 5) "a call inside expr"

; --- the libraries' functions keep their own names -------------------------------------------------------
var sum 0
check (== (mean (vec 1 2 3)) 2) "a program's (var sum 0) does not break mean"
check (== sum 0) "...and the program's sum is its own"
function length (x) "mine"
check (== (median (vec 3 1 2)) 2) "a program's own length does not break median"
check (equal? (length 1) "mine") "...and the program calls its own"
set tau 7
check (== tau 7) "set changes a library variable"
check (contains? (str (vars)) "mean") "vars lists the library's names"

; --- load runs a module again when it changed ------------------------------------------------------------
var mod "/tmp/musil_test_robust_mod.mu"
write mod "var mod-version 1\n"
load mod
check (== mod-version 1) "load: the module ran"
set mod-version 0
load mod
check (== mod-version 0) "load: a second load of the same file does nothing"
write mod "var mod-version 2\nvar mod-extra 1\n"
load mod
check (== mod-version 2) "load: a module that changed runs again"
remove mod

report "test_semantics"

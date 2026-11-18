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

report "test_semantics"

; COMMAND-LINE: --arrays-exp
; EXPECT: unsat
(set-logic QF_ALIA)
(set-info :status unsat)
(declare-const all1 (Array Int Int))
(declare-const a Int)
(declare-const i Int)
(assert (= all1 ((as const (Array Int Int)) 1)))
(assert (= a (select all1 i)))
(assert (not (= a 1)))
(check-sat)

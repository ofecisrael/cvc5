(set-logic ALL)
(set-info :status unsat)
(declare-fun x () (Nullable Int))
(declare-fun y () (Nullable Int))
(declare-fun z () Int)
(assert (= x (as nullable.null (Nullable Int))))
(assert (= y (nullable.some z)))
(assert (= x y))
(check-sat)

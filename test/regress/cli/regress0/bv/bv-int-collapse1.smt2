; COMMAND-LINE: --solve-bv-as-int=sum
; EXPECT: unsat
(set-logic ALL)
(set-info :status unsat)
(declare-fun t () (_ BitVec 16))
(assert (not (= t ((_ int_to_bv 16) (ubv_to_int t)))))
(check-sat)
